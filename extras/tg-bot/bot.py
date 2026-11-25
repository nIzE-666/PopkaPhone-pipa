#!/usr/bin/env python3
"""Main entry point for pipa-tg-bot (Russian Telegram Control Hub for Xiaomi Pad 6)."""

from __future__ import annotations

import asyncio
import logging
import os
import secrets
import signal
import sys
import time
from pathlib import Path
from typing import Any, Awaitable, Callable, Dict, List, Optional, Tuple

# Support direct execution (`python bot.py`) and package import
_APP_DIR = Path(__file__).resolve().parent
if str(_APP_DIR) not in sys.path:
    sys.path.insert(0, str(_APP_DIR))

from aiogram import BaseMiddleware, Bot, Dispatcher, F, Router
from aiogram.client.default import DefaultBotProperties
from aiogram.client.session.aiohttp import AiohttpSession
from aiogram.enums import ParseMode
from aiogram.exceptions import TelegramBadRequest
from aiogram.filters import Command, CommandObject, CommandStart
from aiogram.fsm.context import FSMContext
from aiogram.fsm.state import State, StatesGroup
from aiogram.fsm.storage.memory import MemoryStorage
from aiogram.types import (
    BotCommand,
    BufferedInputFile,
    CallbackQuery,
    FSInputFile,
    Message,
    TelegramObject,
)

from callbacks import pack_cb, unpack_cb
from commands import BOT_COMMANDS, build_help_text
from config import BotConfig, OwnerAuthManager, PCConfig, load_config
from drivers.android_tv import AndroidTVDriver
from drivers.media_server import LocalMediaServer
from drivers.network import NetworkDriver
from drivers.pc import PCDriver
from drivers.samsung_tv import SamsungTVDriver
from drivers.tablet import SERVICES_BY_KEY, TabletDriver
from helpers import (
    AlertWatcher,
    parse_open_command,
    truncate_shell_output,
    unique_target_path,
)
from ui import (
    ButtonSpec,
    KeyboardRows,
    build_alerts_panel,
    build_android_tv_panel,
    build_camera_panel,
    build_clipboard_panel,
    build_confirm_dialog,
    build_main_dashboard,
    build_mode_panel,
    build_network_panel,
    build_pc_detail_panel,
    build_pc_list_panel,
    build_samsung_tv_panel,
    build_scenes_panel,
    build_services_panel,
    build_status_summary,
    build_tablet_panel,
    build_terminal_panel,
    build_tv_selector_panel,
    build_vpn_panel,
    esc,
    to_aiogram_markup,
)


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("pipa-tg-bot")

MAX_TELEGRAM_UPLOAD_BYTES = 48 * 1024 * 1024  # 48 MiB safe margin under Bot API 50 MiB limit

TERM_QUICK_CMDS: Dict[str, str] = {
    "ls": "ls -la",
    "free": "free -h",
    "df": "df -h",
    "up": "uptime",
}


class BotStates(StatesGroup):
    waiting_android_tv_text = State()
    waiting_samsung_tv_text = State()
    waiting_samsung_tv_cast = State()
    waiting_clip_tablet = State()
    waiting_clip_pc = State()


class TerminalStates(StatesGroup):
    active = State()


class AppContext:
    """Shared drivers and runtime state."""

    def __init__(self, config: BotConfig) -> None:
        self.config = config
        self.auth = OwnerAuthManager(config)
        self.tablet = TabletDriver(wifi_interface=config.network.wifi_interface)
        self.samsung_tv = SamsungTVDriver(
            host=config.samsung_tv.host,
            port=config.samsung_tv.port,
            fallback_port=config.samsung_tv.fallback_port,
            app_name=config.samsung_tv.app_name,
            token_path=config.samsung_token_path,
            mac=config.samsung_tv.mac,
            broadcast=config.samsung_tv.broadcast,
            dlna_port=config.samsung_tv.dlna_port,
        )
        self.android_tv = AndroidTVDriver(
            host=config.android_tv.host,
            adb_port=config.android_tv.adb_port,
            cast_port=config.android_tv.cast_port,
        )
        self.media_server = LocalMediaServer(
            advertise_host=config.network.tablet_ip,
            port=config.media_server_port,
        )
        self.network = NetworkDriver(config)
        self.alert_watcher = AlertWatcher(config.alerts)
        self.vpn_profiles_cache: List[str] = []
        self.pending_cast_files: Dict[str, Path] = {}

    def default_pc(self) -> Optional[PCConfig]:
        return self.config.get_pc("mypc") or (self.config.pcs[0] if self.config.pcs else None)

    def register_cast_candidate(self, path: Path) -> str:
        key = secrets.token_hex(6)
        self.pending_cast_files[key] = path
        while len(self.pending_cast_files) > 64:
            oldest = next(iter(self.pending_cast_files))
            self.pending_cast_files.pop(oldest, None)
        return key

    def pc_driver(self, pc: PCConfig) -> PCDriver:
        return PCDriver(
            host=pc.host,
            port=pc.port,
            token=self.config.effective_pc_token(pc),
            mac=pc.mac,
            broadcast=pc.broadcast,
        )


class OwnerAuthMiddleware(BaseMiddleware):
    """Enforces strict owner-only access with zero-friction first-start /pair flow."""

    def __init__(self, auth: OwnerAuthManager) -> None:
        super().__init__()
        self.auth = auth

    async def __call__(
        self,
        handler: Callable[[TelegramObject, Dict[str, Any]], Awaitable[Any]],
        event: TelegramObject,
        data: Dict[str, Any],
    ) -> Any:
        user = data.get("event_from_user")
        user_id = getattr(user, "id", None)

        if self.auth.is_paired:
            if self.auth.is_owner(user_id):
                return await handler(event, data)
            logger.warning("Blocked unauthorized Telegram user_id=%s", user_id)
            if isinstance(event, CallbackQuery):
                await event.answer("🔒 Доступ запрещён.", show_alert=True)
            elif isinstance(event, Message):
                await event.answer("🔒 Доступ запрещён. Этот бот обслуживает только владельца.")
            return None

        # Not paired yet: allow only /pair <CODE> (and explain pairing on /start)
        if isinstance(event, Message):
            text = (event.text or "").strip()
            if text.startswith("/pair"):
                return await handler(event, data)
            await event.answer(
                "🔐 <b>Требуется первичная привязка владельца</b>\n\n"
                "Отправьте команду <code>/pair КОД</code>, где 6-значный код указан в журнале службы "
                "(<code>journalctl --user -u pipa-tg-bot</code>) или в файле "
                "<code>~/.config/pipa-bot/pair_code.txt</code> на планшете."
            )
            return None

        if isinstance(event, CallbackQuery):
            await event.answer("Сначала выполните привязку: /pair КОД", show_alert=True)
            return None

        return None


async def edit_or_send(
    target: Message | CallbackQuery,
    text: str,
    rows: KeyboardRows,
) -> None:
    """Update current message cleanly in-place, falling back to sending a new message."""
    markup = to_aiogram_markup(rows)
    if isinstance(target, CallbackQuery):
        msg = target.message
        if isinstance(msg, Message):
            try:
                await msg.edit_text(text, reply_markup=markup, disable_web_page_preview=True)
                return
            except TelegramBadRequest as exc:
                if "message is not modified" in str(exc).lower():
                    return
                await msg.answer(text, reply_markup=markup, disable_web_page_preview=True)
                return
        return
    await target.answer(text, reply_markup=markup, disable_web_page_preview=True)


def create_router(ctx: AppContext) -> Router:
    router = Router(name="pipa_hub")

    # ------------------------------------------------------------------
    # Pairing & Dashboard Commands
    # ------------------------------------------------------------------

    @router.message(Command("pair"))
    async def cmd_pair(message: Message, command: CommandObject) -> None:
        if not message.from_user:
            return
        code = (command.args or "").strip()
        ok, reply = ctx.auth.try_pair(message.from_user.id, code)
        await message.answer(esc(reply))
        if ok:
            status = await ctx.tablet.get_status()
            text, rows = build_main_dashboard(status, notice="Привязка завершена — добро пожаловать!")
            await edit_or_send(message, text, rows)

    @router.message(CommandStart())
    @router.message(Command("menu"))
    async def cmd_start(message: Message, state: FSMContext) -> None:
        await state.clear()
        status = await ctx.tablet.get_status()
        text, rows = build_main_dashboard(status)
        await edit_or_send(message, text, rows)

    @router.message(Command("cancel"))
    async def cmd_cancel(message: Message, state: FSMContext) -> None:
        await state.clear()
        await message.answer("✅ Текущий ввод отменён.")

    @router.message(Command("help"))
    async def cmd_help(message: Message) -> None:
        await message.answer(build_help_text(), disable_web_page_preview=True)

    @router.message(Command("status"))
    async def cmd_status(message: Message) -> None:
        text, rows = await _render_status(ctx)
        await edit_or_send(message, text, rows)

    @router.message(Command("tablet"))
    async def cmd_tablet(message: Message) -> None:
        status = await ctx.tablet.get_status()
        text, rows = build_tablet_panel(status)
        await edit_or_send(message, text, rows)

    @router.message(Command("screen"))
    async def cmd_screen(message: Message) -> None:
        wait_msg = await message.answer("📸 Делаю скриншот экрана планшета…")
        png, caption = await ctx.tablet.take_screenshot()
        if png:
            await message.answer_photo(
                BufferedInputFile(png, filename="pipa-screenshot.png"),
                caption=f"📱 {caption}",
            )
            try:
                await wait_msg.delete()
            except TelegramBadRequest:
                pass
        else:
            await wait_msg.edit_text(f"❌ {esc(caption)}")

    @router.message(Command("cam"))
    async def cmd_cam(message: Message, command: CommandObject) -> None:
        raw_args = (command.args or "").strip().lower().split()
        if not raw_args:
            text, rows = build_camera_panel(torch_on=ctx.tablet.is_torch_on())
            await edit_or_send(message, text, rows)
            return

        sub = raw_args[0]
        if sub in ("back", "front"):
            zoom = 0
            flash = "off"
            for tok in raw_args[1:]:
                if tok.isdigit():
                    zoom = max(0, min(99, int(tok)))
                elif tok in ("off", "auto", "on"):
                    flash = tok
            await _capture_and_send_camera_photo(ctx, message, facing=sub, zoom=zoom, flash=flash)
            return

        if sub == "torch":
            mode = raw_args[1] if len(raw_args) > 1 else "toggle"
            if mode == "on":
                target_on = True
            elif mode == "off":
                target_on = False
            else:
                target_on = not ctx.tablet.is_torch_on()
            ok, notice = await ctx.tablet.set_torch(target_on)
            text, rows = build_camera_panel(
                torch_on=ctx.tablet.is_torch_on(),
                notice=notice if ok else f"❌ {notice}",
            )
            await edit_or_send(message, text, rows)
            return

        if sub == "open":
            ok, notice = await ctx.tablet.launch_camera_app()
            text, rows = build_camera_panel(
                torch_on=ctx.tablet.is_torch_on(),
                notice=notice if ok else f"❌ {notice}",
            )
            await edit_or_send(message, text, rows)
            return

        text, rows = build_camera_panel(
            torch_on=ctx.tablet.is_torch_on(),
            notice="Использование: /cam, /cam back, /cam front, /cam torch on|off, /cam open",
        )
        await edit_or_send(message, text, rows)

    @router.message(Command("vpn"))
    async def cmd_vpn(message: Message) -> None:
        text, rows = await _render_vpn(ctx)
        await edit_or_send(message, text, rows)

    @router.message(Command("mode"))
    async def cmd_mode(message: Message) -> None:
        parts = (message.text or "").split()
        notice = ""
        if len(parts) > 1 and parts[1].lower() in ("home", "away", "auto"):
            _, notice = await ctx.tablet.set_travel_mode(parts[1].lower())
        info = await ctx.tablet.get_travel_mode()
        text, rows = build_mode_panel(info, notice=notice)
        await edit_or_send(message, text, rows)

    @router.message(Command("services"))
    async def cmd_services(message: Message) -> None:
        svcs = await ctx.tablet.list_services()
        text, rows = build_services_panel(svcs)
        await edit_or_send(message, text, rows)

    @router.message(Command("pc"))
    async def cmd_pc(message: Message) -> None:
        text, rows = await _render_pc_entry(ctx)
        await edit_or_send(message, text, rows)

    @router.message(Command("tv"))
    async def cmd_tv(message: Message) -> None:
        s_on, a_on = await asyncio.gather(ctx.samsung_tv.is_online(), ctx.android_tv.is_online())
        text, rows = build_tv_selector_panel(ctx.config, s_on, a_on)
        await edit_or_send(message, text, rows)

    @router.message(Command("tv_text"))
    async def cmd_tv_text(message: Message, command: CommandObject, state: FSMContext) -> None:
        raw_text = (command.args or "").strip()
        if raw_text:
            await state.clear()
            ok, notice = await ctx.samsung_tv.send_text(raw_text)
            online = await ctx.samsung_tv.is_online()
            text, rows = build_samsung_tv_panel(ctx.config, online, notice=notice if ok else f"❌ {notice}")
            await edit_or_send(message, text, rows)
            return
        await state.set_state(BotStates.waiting_samsung_tv_text)
        await message.answer(
            "⌨️ <b>Ввод текста на Samsung TV</b>\n"
            "Отправьте текст (на русском или английском) следующим сообщением, или /cancel для отмены."
        )

    @router.message(Command("tv_cast"))
    async def cmd_tv_cast(message: Message, command: CommandObject, state: FSMContext) -> None:
        raw_url = (command.args or "").strip()
        if raw_url:
            await state.clear()
            ok, notice = await ctx.samsung_tv.cast_url(raw_url)
            online = await ctx.samsung_tv.is_online()
            text, rows = build_samsung_tv_panel(ctx.config, online, notice=notice if ok else f"❌ {notice}")
            await edit_or_send(message, text, rows)
            return
        await state.set_state(BotStates.waiting_samsung_tv_cast)
        await message.answer(
            "📺 <b>Cast URL на Samsung TV (DLNA)</b>\n"
            "Отправьте прямую ссылку <code>http://</code> или <code>https://</code> на видео, аудио или фото (или /cancel)."
        )

    @router.message(Command("clip"))
    async def cmd_clip(message: Message, state: FSMContext) -> None:
        await state.clear()
        text, rows = await _render_clipboard(ctx)
        await edit_or_send(message, text, rows)

    @router.message(Command("scenes"))
    async def cmd_scenes(message: Message, state: FSMContext) -> None:
        await state.clear()
        text, rows = build_scenes_panel()
        await edit_or_send(message, text, rows)

    @router.message(Command("term"))
    async def cmd_term(message: Message, state: FSMContext) -> None:
        data = await state.get_data()
        cwd = data.get("cwd") or str(Path.home())
        await state.set_state(TerminalStates.active)
        await state.update_data(cwd=cwd)
        text, rows = build_terminal_panel(cwd)
        await edit_or_send(message, text, rows)

    @router.message(Command("net"))
    async def cmd_net(message: Message) -> None:
        wait_msg = await message.answer("🌐 Сканирую домашнюю сеть…")
        text, rows = await _render_network(ctx, deep=False)
        await wait_msg.edit_text(text, reply_markup=to_aiogram_markup(rows), disable_web_page_preview=True)

    @router.message(Command("alerts"))
    async def cmd_alerts(message: Message) -> None:
        text, rows = build_alerts_panel(ctx.config.alerts)
        await edit_or_send(message, text, rows)

    # ------------------------------------------------------------------
    # Terminal, URL Opening & File Transfer Commands
    # ------------------------------------------------------------------

    @router.message(Command("run"))
    @router.message(Command("cmd"))
    async def cmd_run(message: Message, command: CommandObject) -> None:
        cmd_str = (command.args or "").strip()
        if not cmd_str:
            await message.answer("Использование: <code>/run &lt;команда&gt;</code> или <code>/cmd &lt;команда&gt;</code>\nПример: <code>/run uptime</code>")
            return
        rc, output = await ctx.tablet.run_shell(cmd_str, timeout=30.0)
        header = f"🖥 <code>{esc(cmd_str)}</code>\nКод выхода: <b>{rc}</b>"
        cleaned = output.strip() or "(пустой вывод)"
        short, needs_file = truncate_shell_output(cleaned, limit=3600)
        if not needs_file:
            await message.answer(f"{header}\n<pre>{esc(short)}</pre>")
        else:
            await message.answer(f"{header}\nВывод слишком длинный ({len(cleaned)} симв.) — отправляю файлом.")
            await message.answer_document(
                BufferedInputFile(cleaned.encode("utf-8"), filename="command_output.txt"),
                caption=f"Полный вывод: {cmd_str[:120]}",
            )

    @router.message(Command("open"))
    async def cmd_open(message: Message, command: CommandObject) -> None:
        target, url = parse_open_command(command.args or "")
        if not url:
            await message.answer(
                "Использование:\n"
                "• <code>/open https://example.com</code> — открыть на планшете\n"
                "• <code>/open pc https://example.com</code> — открыть на ПК\n"
                "• <code>/open tv https://example.com</code> — открыть на Android TV"
            )
            return
        if target == "pc":
            pc = ctx.default_pc()
            if not pc:
                await message.answer("❌ Нет настроенных ПК.")
                return
            ok, msg, _ = await ctx.pc_driver(pc).action("open_url", url=url)
            await message.answer(f"{'✅' if ok else '❌'} {esc(msg)}")
        elif target == "tv":
            ok, msg = await ctx.android_tv.open_url(url)
            await message.answer(f"{'✅' if ok else '❌'} {esc(msg)}")
        else:
            ok, msg = await ctx.tablet.open_url(url)
            await message.answer(f"{'✅' if ok else '❌'} {esc(msg)}")

    @router.message(Command("get"))
    async def cmd_get(message: Message, command: CommandObject) -> None:
        raw_path = (command.args or "").strip()
        if not raw_path:
            await message.answer("Использование: <code>/get /путь/к/файлу</code> (поддерживается <code>~</code>)")
            return
        path = Path(raw_path).expanduser()
        if not path.is_file():
            await message.answer(f"❌ Файл не найден: <code>{esc(path)}</code>")
            return
        size = path.stat().st_size
        if size > MAX_TELEGRAM_UPLOAD_BYTES:
            await message.answer(
                f"❌ Файл слишком большой ({size / (1024 * 1024):.1f} МБ). Лимит Telegram Bot API — 48 МБ."
            )
            return
        await message.answer_document(FSInputFile(path), caption=f"📄 <code>{esc(path)}</code>")

    # ------------------------------------------------------------------
    # FSM: Text Inputs, Cast URL, Clipboard Write, Interactive Terminal
    # ------------------------------------------------------------------

    @router.message(BotStates.waiting_android_tv_text, F.text)
    async def handle_android_tv_text(message: Message, state: FSMContext) -> None:
        text_val = (message.text or "").strip()
        await state.clear()
        if not text_val:
            await message.answer("❌ Пустой текст отменён.")
            return
        ok, notice = await ctx.android_tv.send_text(text_val)
        online = await ctx.android_tv.is_online()
        panel_text, rows = build_android_tv_panel(ctx.config, online, notice=notice if ok else f"❌ {notice}")
        await edit_or_send(message, panel_text, rows)

    @router.message(BotStates.waiting_samsung_tv_text, F.text)
    async def handle_samsung_tv_text(message: Message, state: FSMContext) -> None:
        text_val = (message.text or "").strip()
        await state.clear()
        if not text_val:
            await message.answer("❌ Пустой текст отменён.")
            return
        ok, notice = await ctx.samsung_tv.send_text(text_val)
        online = await ctx.samsung_tv.is_online()
        panel_text, rows = build_samsung_tv_panel(ctx.config, online, notice=notice if ok else f"❌ {notice}")
        await edit_or_send(message, panel_text, rows)

    @router.message(BotStates.waiting_samsung_tv_cast, F.text)
    async def handle_samsung_tv_cast(message: Message, state: FSMContext) -> None:
        url_val = (message.text or "").strip()
        await state.clear()
        if not url_val:
            await message.answer("❌ Пустая ссылка отменена.")
            return
        ok, notice = await ctx.samsung_tv.cast_url(url_val)
        online = await ctx.samsung_tv.is_online()
        panel_text, rows = build_samsung_tv_panel(ctx.config, online, notice=notice if ok else f"❌ {notice}")
        await edit_or_send(message, panel_text, rows)

    @router.message(BotStates.waiting_clip_tablet, F.text)
    async def handle_clip_tablet_write(message: Message, state: FSMContext) -> None:
        text_val = message.text or ""
        await state.clear()
        ok, notice = await ctx.tablet.set_clipboard(text_val)
        panel_text, rows = await _render_clipboard(ctx, notice=notice if ok else f"❌ {notice}")
        await edit_or_send(message, panel_text, rows)

    @router.message(BotStates.waiting_clip_pc, F.text)
    async def handle_clip_pc_write(message: Message, state: FSMContext) -> None:
        text_val = message.text or ""
        await state.clear()
        pc = ctx.default_pc()
        if not pc:
            await message.answer("❌ Нет настроенных ПК.")
            return
        ok, notice = await ctx.pc_driver(pc).set_clipboard(text_val)
        panel_text, rows = await _render_clipboard(ctx, notice=notice if ok else f"❌ {notice}")
        await edit_or_send(message, panel_text, rows)

    @router.message(TerminalStates.active, F.text & ~F.text.startswith("/"))
    async def handle_terminal_input(message: Message, state: FSMContext) -> None:
        cmd_str = (message.text or "").strip()
        if not cmd_str:
            return
        data = await state.get_data()
        cwd = data.get("cwd") or str(Path.home())
        rc, cleaned, new_cwd = await _execute_terminal_step(ctx, cmd_str, cwd)
        await state.update_data(cwd=new_cwd)
        short, needs_file = truncate_shell_output(cleaned, limit=3300)
        display_out = short if not needs_file else (short + "\n… (полный вывод отправлен файлом)")
        text, rows = build_terminal_panel(new_cwd, command=cmd_str, rc=rc, output=display_out)
        await edit_or_send(message, text, rows)
        if needs_file:
            await message.answer_document(
                BufferedInputFile(cleaned.encode("utf-8"), filename="terminal_output.txt"),
                caption=f"Полный вывод: {cmd_str[:120]}",
            )

    # ------------------------------------------------------------------
    # Incoming File/Media Uploads -> ~/Downloads/tg-bot/ + Samsung TV Cast
    # ------------------------------------------------------------------

    @router.message(F.document | F.photo | F.video | F.audio | F.voice)
    async def handle_file_upload(message: Message, bot: Bot) -> None:
        dl_dir = Path(ctx.config.downloads_dir).expanduser()
        dl_dir.mkdir(parents=True, exist_ok=True)

        file_id: Optional[str] = None
        orig_name = "upload.bin"

        if message.document:
            file_id = message.document.file_id
            orig_name = message.document.file_name or "document.bin"
        elif message.photo:
            best = message.photo[-1]
            file_id = best.file_id
            orig_name = f"photo_{int(time.time())}.jpg"
        elif message.video:
            file_id = message.video.file_id
            orig_name = message.video.file_name or f"video_{int(time.time())}.mp4"
        elif message.audio:
            file_id = message.audio.file_id
            orig_name = message.audio.file_name or f"audio_{int(time.time())}.mp3"
        elif message.voice:
            file_id = message.voice.file_id
            orig_name = f"voice_{int(time.time())}.ogg"

        if not file_id:
            return

        dest = unique_target_path(dl_dir, orig_name)
        try:
            tg_file = await bot.get_file(file_id)
            if not tg_file.file_path:
                await message.answer("❌ Не удалось получить ссылку на файл от Telegram.")
                return
            await bot.download_file(tg_file.file_path, destination=dest)
            size_kb = dest.stat().st_size / 1024.0
            fkey = ctx.register_cast_candidate(dest)
            rows: KeyboardRows = [
                [ButtonSpec("📺 Включить на Samsung TV", pack_cb("s", "castf", fkey))],
            ]
            await message.answer(
                f"📥 <b>Файл сохранён на планшете</b>\n"
                f"• Путь: <code>{esc(dest)}</code>\n"
                f"• Размер: {size_kb:.1f} КБ",
                reply_markup=to_aiogram_markup(rows),
            )
        except Exception as exc:
            logger.exception("Failed to download incoming file")
            await message.answer(f"❌ Ошибка сохранения файла: {esc(exc)}")

    # ------------------------------------------------------------------
    # Interactive Callback Query Router
    # ------------------------------------------------------------------

    @router.callback_query()
    async def handle_callback(callback: CallbackQuery, state: FSMContext) -> None:
        data = callback.data or ""
        try:
            scope, action, args = unpack_cb(data)
        except ValueError:
            await callback.answer("Некорректная кнопка.", show_alert=False)
            return

        # Clear any pending text/terminal FSM state when navigating elsewhere
        keep_state = (
            (scope == "a" and action == "text")
            or (scope == "s" and action in ("text", "cast"))
            or (scope == "cl" and action in ("wt", "wp"))
            or (scope == "tm" and action in ("open", "q"))
        )
        if not keep_state:
            await state.clear()

        # --- Main Menu & Summary ---
        if scope == "m":
            if action == "home":
                await callback.answer("Обновляю…")
                status = await ctx.tablet.get_status()
                text, rows = build_main_dashboard(status)
                await edit_or_send(callback, text, rows)
                return
            if action == "status":
                await callback.answer("Собираю сводку…")
                text, rows = await _render_status(ctx)
                await edit_or_send(callback, text, rows)
                return
            if action == "cam":
                await callback.answer()
                text, rows = build_camera_panel(torch_on=ctx.tablet.is_torch_on())
                await edit_or_send(callback, text, rows)
                return

        # --- Camera & Flashlight Controls ---
        if scope == "cam":
            if action == "snap" and len(args) >= 3:
                facing = args[0]
                try:
                    zoom = int(args[1])
                except ValueError:
                    zoom = 0
                flash = args[2]
                await callback.answer("📸 Снимаю фото…")
                if isinstance(callback.message, Message):
                    await _capture_and_send_camera_photo(
                        ctx,
                        callback.message,
                        facing=facing,
                        zoom=zoom,
                        flash=flash,
                    )
                return
            if action == "torch":
                mode = args[0] if args else "toggle"
                if mode == "on":
                    target_on = True
                elif mode == "off":
                    target_on = False
                else:
                    target_on = not ctx.tablet.is_torch_on()
                ok, notice = await ctx.tablet.set_torch(target_on)
                await callback.answer(notice[:180], show_alert=not ok)
                text, rows = build_camera_panel(
                    torch_on=ctx.tablet.is_torch_on(),
                    notice=notice if ok else f"❌ {notice}",
                )
                await edit_or_send(callback, text, rows)
                return
            if action == "open":
                ok, notice = await ctx.tablet.launch_camera_app()
                await callback.answer(notice[:180], show_alert=not ok)
                text, rows = build_camera_panel(
                    torch_on=ctx.tablet.is_torch_on(),
                    notice=notice if ok else f"❌ {notice}",
                )
                await edit_or_send(callback, text, rows)
                return
            await callback.answer()
            text, rows = build_camera_panel(torch_on=ctx.tablet.is_torch_on())
            await edit_or_send(callback, text, rows)
            return

        # --- Tablet Controls ---
        if scope == "t":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "bri" and args:
                ok, notice = await ctx.tablet.set_brightness_pct(int(args[0]))
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "bstep" and args:
                ok, notice = await ctx.tablet.adjust_brightness(int(args[0]))
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "scroff":
                ok, notice = await ctx.tablet.screen_off()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "scron":
                ok, notice = await ctx.tablet.screen_on()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "volup":
                ok, notice = await ctx.tablet.volume_step("up")
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "voldn":
                ok, notice = await ctx.tablet.volume_step("down")
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "mute":
                ok, notice = await ctx.tablet.toggle_mute()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "shot":
                await callback.answer("📸 Делаю скриншот…")
                png, caption = await ctx.tablet.take_screenshot()
                if png and isinstance(callback.message, Message):
                    await callback.message.answer_photo(
                        BufferedInputFile(png, filename="pipa-screenshot.png"),
                        caption=f"📱 {caption}",
                    )
                elif isinstance(callback.message, Message):
                    await callback.message.answer(f"❌ {esc(caption)}")
                return
            status = await ctx.tablet.get_status()
            text, rows = build_tablet_panel(status, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Travel / Security Mode Controls ---
        if scope == "md":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "set" and args:
                target = args[0]
                await callback.answer(f"Переключаю режим: {target}…")
                _, notice = await ctx.tablet.set_travel_mode(target)
            info = await ctx.tablet.get_travel_mode()
            text, rows = build_mode_panel(info, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- VPN Controls ---
        if scope == "v":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "on" and args:
                idx = int(args[0])
                if not ctx.vpn_profiles_cache:
                    ctx.vpn_profiles_cache = await ctx.tablet.list_vpn_profiles()
                if 0 <= idx < len(ctx.vpn_profiles_cache):
                    prof = ctx.vpn_profiles_cache[idx]
                    await callback.answer(f"Подключаю {prof}…")
                    _, notice = await ctx.tablet.vpn_connect(prof)
                else:
                    await callback.answer("Список VPN устарел, обновляю.", show_alert=True)
            elif action == "off":
                await callback.answer("Отключаю VPN…")
                _, notice = await ctx.tablet.vpn_disconnect()
            text, rows = await _render_vpn(ctx, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Services Controls ---
        if scope == "sv":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action in ("start", "stop", "rst") and args:
                op = "restart" if action == "rst" else action
                await callback.answer("Выполняю…")
                _, notice = await ctx.tablet.service_action(args[0], op)
            svcs = await ctx.tablet.list_services()
            text, rows = build_services_panel(svcs, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- PC Controls ---
        if scope == "p":
            if action == "list":
                await callback.answer()
                text, rows = await _render_pc_list(ctx)
                await edit_or_send(callback, text, rows)
                return
            if not args:
                await callback.answer("ПК не указан.")
                return
            pc = ctx.config.get_pc(args[0])
            if not pc:
                await callback.answer("ПК не найден в конфигурации.", show_alert=True)
                return
            drv = ctx.pc_driver(pc)
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "wol":
                ok, notice = await drv.wake()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "shot":
                await callback.answer("📸 Запрашиваю скриншот ПК…")
                png, caption = await drv.take_screenshot()
                if png and isinstance(callback.message, Message):
                    await callback.message.answer_photo(
                        BufferedInputFile(png, filename=f"{pc.id}-screenshot.png"),
                        caption=f"💻 {caption}",
                    )
                elif isinstance(callback.message, Message):
                    await callback.message.answer(f"❌ {esc(caption)}")
                return
            elif action == "act" and len(args) >= 2:
                ok, notice, _ = await drv.action(args[1])
                await callback.answer(notice[:180], show_alert=not ok)
            ok_st, pstat, _ = await drv.get_status()
            text, rows = build_pc_detail_panel(pc, ok_st, pstat, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- TV Selector ---
        if scope == "tv":
            await callback.answer()
            s_on, a_on = await asyncio.gather(ctx.samsung_tv.is_online(), ctx.android_tv.is_online())
            text, rows = build_tv_selector_panel(ctx.config, s_on, a_on)
            await edit_or_send(callback, text, rows)
            return

        # --- Samsung TV Remote, Text Input & DLNA Cast ---
        if scope == "s":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "k" and args:
                ok, notice = await ctx.samsung_tv.send_key(args[0])
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "app" and args:
                ok, notice = await ctx.samsung_tv.launch_app(args[0])
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "wol":
                ok, notice = await ctx.samsung_tv.wake()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "text":
                await state.set_state(BotStates.waiting_samsung_tv_text)
                await callback.answer("Отправьте текст следующим сообщением.")
                if isinstance(callback.message, Message):
                    await callback.message.answer(
                        "⌨️ <b>Ввод текста на Samsung TV</b>\n"
                        "Отправьте текст (русский или английский) следующим сообщением (или /cancel для отмены)."
                    )
                return
            elif action == "cast":
                await state.set_state(BotStates.waiting_samsung_tv_cast)
                await callback.answer("Отправьте медиа-ссылку следующим сообщением.")
                if isinstance(callback.message, Message):
                    await callback.message.answer(
                        "📺 <b>Cast URL на Samsung TV (DLNA)</b>\n"
                        "Отправьте прямую ссылку <code>http://</code> или <code>https://</code> следующим сообщением (или /cancel)."
                    )
                return
            elif action == "cstop":
                ok, notice = await ctx.samsung_tv.cast_stop()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "cpause":
                ok, notice = await ctx.samsung_tv.cast_pause()
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "castf" and args:
                path = ctx.pending_cast_files.get(args[0])
                if not path or not path.is_file():
                    await callback.answer("❌ Файл больше не найден или устарел.", show_alert=True)
                    return
                await callback.answer("📺 Отправляю файл на Samsung TV…")
                try:
                    if not ctx.media_server.is_running:
                        await ctx.media_server.start()
                    tablet_ip = await ctx.tablet.get_wifi_ip() or ctx.config.network.tablet_ip
                    _, media_url, mime_type = ctx.media_server.register_file(path, advertise_host=tablet_ip)
                    ok, msg = await ctx.samsung_tv.cast_url(media_url, title=path.name, mime_type=mime_type)
                    if isinstance(callback.message, Message):
                        ctl_rows: KeyboardRows = [
                            [
                                ButtonSpec("⏸ Пауза", pack_cb("s", "cpause")),
                                ButtonSpec("⏹ Стоп Cast", pack_cb("s", "cstop")),
                            ],
                            [ButtonSpec("📺 Пульт Samsung TV", pack_cb("s", "open"))],
                        ]
                        await callback.message.answer(
                            f"{'✅' if ok else '❌'} {esc(msg)}\n"
                            f"• Файл: <code>{esc(path.name)}</code>\n"
                            f"• Локальный поток: <code>{esc(media_url)}</code>",
                            reply_markup=to_aiogram_markup(ctl_rows),
                        )
                except Exception as exc:
                    logger.exception("Failed to cast local file to Samsung TV")
                    if isinstance(callback.message, Message):
                        await callback.message.answer(f"❌ Ошибка Cast на Samsung TV: {esc(exc)}")
                return
            online = await ctx.samsung_tv.is_online()
            text, rows = build_samsung_tv_panel(ctx.config, online, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Android TV Remote ---
        if scope == "a":
            notice = ""
            if action == "open":
                await callback.answer()
            elif action == "k" and args:
                ok, notice = await ctx.android_tv.send_key(args[0])
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "app" and args:
                ok, notice = await ctx.android_tv.launch_app(args[0])
                await callback.answer(notice[:180], show_alert=not ok)
            elif action == "shot":
                await callback.answer("📸 Делаю скриншот Android TV…")
                png, caption = await ctx.android_tv.take_screenshot()
                if png and isinstance(callback.message, Message):
                    await callback.message.answer_photo(
                        BufferedInputFile(png, filename="android-tv.png"),
                        caption=f"📺 {caption}",
                    )
                elif isinstance(callback.message, Message):
                    await callback.message.answer(f"❌ {esc(caption)}")
                return
            elif action == "text":
                await state.set_state(BotStates.waiting_android_tv_text)
                await callback.answer("Отправьте текст следующим сообщением.")
                if isinstance(callback.message, Message):
                    await callback.message.answer(
                        "⌨️ <b>Ввод текста на Android TV</b>\n"
                        "Отправьте нужный текст следующим сообщением (или /cancel для отмены)."
                    )
                return
            online = await ctx.android_tv.is_online()
            text, rows = build_android_tv_panel(ctx.config, online, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Home Network ---
        if scope == "n":
            deep = action == "scan"
            await callback.answer("🔍 Сканирую подсеть…" if deep else "Обновляю…")
            text, rows = await _render_network(ctx, deep=deep)
            await edit_or_send(callback, text, rows)
            return

        # --- Alerts Settings ---
        if scope == "al":
            notice = ""
            if action == "tog" and args:
                which = args[0]
                if which == "all":
                    ctx.config.alerts.enabled = not ctx.config.alerts.enabled
                    notice = "Все уведомления включены." if ctx.config.alerts.enabled else "Все уведомления отключены."
                elif which == "batt":
                    ctx.config.alerts.low_battery_enabled = not ctx.config.alerts.low_battery_enabled
                    notice = "Уведомление о низком заряде переключено."
                elif which == "heat":
                    ctx.config.alerts.overheat_enabled = not ctx.config.alerts.overheat_enabled
                    notice = "Уведомление о перегреве переключено."
                elif which == "boot":
                    ctx.config.alerts.startup_notify = not ctx.config.alerts.startup_notify
                    notice = "Уведомление о запуске бота переключено."
                ctx.config.save()
                await callback.answer(notice)
            else:
                await callback.answer()
            text, rows = build_alerts_panel(ctx.config.alerts, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Bidirectional Clipboard ---
        if scope == "cl":
            notice = ""
            pc = ctx.default_pc()
            if action == "open":
                await callback.answer("Обновляю буфер обмена…")
            elif action == "t2p":
                if not pc:
                    await callback.answer("❌ Нет настроенных ПК.", show_alert=True)
                    return
                ok_r, t_text = await ctx.tablet.get_clipboard()
                if not ok_r:
                    notice = f"❌ {t_text}"
                    await callback.answer(notice[:180], show_alert=True)
                else:
                    ok_w, msg_w = await ctx.pc_driver(pc).set_clipboard(t_text)
                    notice = f"✅ Планшет → ПК: {msg_w}" if ok_w else f"❌ {msg_w}"
                    await callback.answer(notice[:180], show_alert=not ok_w)
            elif action == "p2t":
                if not pc:
                    await callback.answer("❌ Нет настроенных ПК.", show_alert=True)
                    return
                ok_r, p_text, err_r = await ctx.pc_driver(pc).get_clipboard()
                if not ok_r:
                    notice = f"❌ {err_r}"
                    await callback.answer(notice[:180], show_alert=True)
                else:
                    ok_w, msg_w = await ctx.tablet.set_clipboard(p_text)
                    notice = f"✅ ПК → Планшет: {msg_w}" if ok_w else f"❌ {msg_w}"
                    await callback.answer(notice[:180], show_alert=not ok_w)
            elif action == "wt":
                await state.set_state(BotStates.waiting_clip_tablet)
                await callback.answer("Отправьте текст для буфера планшета.")
                if isinstance(callback.message, Message):
                    await callback.message.answer(
                        "✍️ <b>Запись в буфер обмена планшета</b>\n"
                        "Отправьте нужный текст следующим сообщением (или /cancel для отмены)."
                    )
                return
            elif action == "wp":
                await state.set_state(BotStates.waiting_clip_pc)
                await callback.answer("Отправьте текст для буфера ПК.")
                if isinstance(callback.message, Message):
                    await callback.message.answer(
                        "✍️ <b>Запись в буфер обмена ПК</b>\n"
                        "Отправьте нужный текст следующим сообщением (или /cancel для отмены)."
                    )
                return
            text, rows = await _render_clipboard(ctx, notice=notice)
            await edit_or_send(callback, text, rows)
            return

        # --- Macro Scenes ---
        if scope == "sc":
            if action == "open":
                await callback.answer()
                text, rows = build_scenes_panel()
                await edit_or_send(callback, text, rows)
                return
            if action == "kino":
                app_choice = "kinopoisk" if (args and args[0] == "kp") else "youtube"
                label = "Кинопоиск" if app_choice == "kinopoisk" else "YouTube"
                await callback.answer(f"🎬 Запускаю сценарий «Кино ({label})»…")
                wait_text, wait_rows = build_scenes_panel(notice=f"⏳ Выполняю сценарий «Кино ({label})»…")
                await edit_or_send(callback, wait_text, wait_rows)
                report = await _run_scene_kino(ctx, app_key=app_choice)
                text, rows = build_scenes_panel(report_lines=report)
                await edit_or_send(callback, text, rows)
                return
            if action == "work":
                await callback.answer("💻 Запускаю сценарий «Работа»…")
                wait_text, wait_rows = build_scenes_panel(notice="⏳ Выполняю сценарий «Работа»…")
                await edit_or_send(callback, wait_text, wait_rows)
                report = await _run_scene_work(ctx)
                text, rows = build_scenes_panel(report_lines=report)
                await edit_or_send(callback, text, rows)
                return

        # --- Interactive Persistent Terminal ---
        if scope == "tm":
            if action == "exit":
                await state.clear()
                await callback.answer("Терминал закрыт.")
                status = await ctx.tablet.get_status()
                text, rows = build_main_dashboard(status, notice="Интерактивный терминал закрыт.")
                await edit_or_send(callback, text, rows)
                return
            data_state = await state.get_data()
            cwd = data_state.get("cwd") or str(Path.home())
            await state.set_state(TerminalStates.active)
            await state.update_data(cwd=cwd)
            if action == "open":
                await callback.answer("Терминал активен.")
                text, rows = build_terminal_panel(cwd)
                await edit_or_send(callback, text, rows)
                return
            if action == "q" and args:
                cmd_str = TERM_QUICK_CMDS.get(args[0])
                if not cmd_str:
                    await callback.answer("Неизвестная быстрая команда.")
                    return
                await callback.answer(f"Выполняю: {cmd_str}")
                rc, cleaned, new_cwd = await _execute_terminal_step(ctx, cmd_str, cwd)
                await state.update_data(cwd=new_cwd)
                short, needs_file = truncate_shell_output(cleaned, limit=3300)
                display_out = short if not needs_file else (short + "\n… (полный вывод отправлен файлом)")
                text, rows = build_terminal_panel(new_cwd, command=cmd_str, rc=rc, output=display_out)
                await edit_or_send(callback, text, rows)
                if needs_file and isinstance(callback.message, Message):
                    await callback.message.answer_document(
                        BufferedInputFile(cleaned.encode("utf-8"), filename="terminal_output.txt"),
                        caption=f"Полный вывод: {cmd_str[:120]}",
                    )
                return

        # --- 2-Step Confirmations for Destructive Actions ---
        if scope == "cf" and args:
            token = args[0]
            if action == "ask":
                await callback.answer()
                title, warn, cancel_cb = _describe_confirm_action(ctx, token)
                text, rows = build_confirm_dialog(title, warn, token, cancel_cb)
                await edit_or_send(callback, text, rows)
                return
            if action == "yes":
                await callback.answer("Выполняю…")
                notice, back_scope = await _execute_confirmed_action(ctx, token)
                if back_scope == "tablet":
                    status = await ctx.tablet.get_status()
                    text, rows = build_tablet_panel(status, notice=notice)
                elif back_scope == "services":
                    svcs = await ctx.tablet.list_services()
                    text, rows = build_services_panel(svcs, notice=notice)
                elif back_scope == "scenes":
                    report_lines = [line for line in notice.splitlines() if line.strip()]
                    text, rows = build_scenes_panel(report_lines=report_lines)
                elif back_scope.startswith("pc:"):
                    pid = back_scope.split(":", 1)[1]
                    pc = ctx.config.get_pc(pid)
                    if pc:
                        ok_st, pstat, _ = await ctx.pc_driver(pc).get_status()
                        text, rows = build_pc_detail_panel(pc, ok_st, pstat, notice=notice)
                    else:
                        text, rows = await _render_pc_list(ctx, notice=notice)
                else:
                    status = await ctx.tablet.get_status()
                    text, rows = build_main_dashboard(status, notice=notice)
                await edit_or_send(callback, text, rows)
                return

        await callback.answer()

    return router


def _describe_confirm_action(ctx: AppContext, token: str) -> Tuple[str, str, str]:
    if token == "treb":
        return (
            "Перезагрузка планшета",
            "Xiaomi Pad 6 будет перезагружен в Linux. Связь с ботом прервётся примерно на минуту.",
            pack_cb("t", "open"),
        )
    if token == "toneui":
        return (
            "Переход в Android (One UI)",
            "Слот загрузки переключится на Android (One UI) и планшет перезагрузится. Linux-бот остановится до возврата в Linux.",
            pack_cb("t", "open"),
        )
    if token.startswith("svr_"):
        key = token[4:]
        spec = SERVICES_BY_KEY.get(key)
        label = spec.label if spec else key
        return (
            f"Перезапуск {label}",
            f"Критическая служба {label} будет перезапущена. Интерфейс планшета может мигнуть на пару секунд.",
            pack_cb("sv", "open"),
        )
    if token.startswith("preb_"):
        pid = token[5:]
        pc = ctx.config.get_pc(pid)
        name = pc.name if pc else pid
        return (
            f"Перезагрузка {name}",
            f"На компьютер {name} будет отправлена команда перезагрузки (через 3 секунды).",
            pack_cb("p", "open", pid),
        )
    if token.startswith("poff_"):
        pid = token[5:]
        pc = ctx.config.get_pc(pid)
        name = pc.name if pc else pid
        return (
            f"Выключение {name}",
            f"Компьютер {name} будет выключен (через 3 секунды). Включить его обратно можно через Wake-on-LAN.",
            pack_cb("p", "open", pid),
        )
    if token == "sc_night":
        return (
            "Сценарий «🌙 Ночь»",
            "Samsung TV будет выключен (если в сети), ПК (Windows) переведён в спящий режим, яркость планшета снижена до 5% и включён лимит заряда батареи 80%.",
            pack_cb("sc", "open"),
        )
    return ("Опасное действие", "Подтвердите выполнение операции.", pack_cb("m", "home"))


async def _execute_confirmed_action(ctx: AppContext, token: str) -> Tuple[str, str]:
    if token == "treb":
        _, msg = await ctx.tablet.reboot()
        return msg, "tablet"
    if token == "toneui":
        _, msg = await ctx.tablet.reboot_one_ui()
        return msg, "tablet"
    if token.startswith("svr_"):
        key = token[4:]
        _, msg = await ctx.tablet.service_action(key, "restart")
        return msg, "services"
    if token.startswith("preb_"):
        pid = token[5:]
        pc = ctx.config.get_pc(pid)
        if not pc:
            return "ПК не найден.", "pc_list"
        _, msg, _ = await ctx.pc_driver(pc).action("reboot")
        return msg, f"pc:{pid}"
    if token.startswith("poff_"):
        pid = token[5:]
        pc = ctx.config.get_pc(pid)
        if not pc:
            return "ПК не найден.", "pc_list"
        _, msg, _ = await ctx.pc_driver(pc).action("shutdown")
        return msg, f"pc:{pid}"
    if token == "sc_night":
        report = await _run_scene_night(ctx)
        return "\n".join(report), "scenes"
    return "Неизвестное действие.", "home"


async def _execute_terminal_step(ctx: AppContext, command: str, cwd: str) -> Tuple[int, str, str]:
    rc, output, new_cwd = await ctx.tablet.run_terminal_command(command, cwd=cwd, timeout=30.0)
    cleaned = output.strip() or "(пустой вывод)"
    return rc, cleaned, new_cwd


async def _capture_and_send_camera_photo(
    ctx: AppContext,
    message: Message,
    facing: str = "back",
    zoom: int = 0,
    flash: str = "off",
) -> None:
    label = "Фронтальная камера (8 МП)" if facing == "front" else "Задняя камера (13 МП)"
    wait_msg = await message.answer(f"📷 Снимаю: {label}…")
    ok, result = await ctx.tablet.snap_camera(facing=facing, zoom=zoom, flash=flash)
    if not ok:
        await wait_msg.edit_text(f"❌ {esc(result)}")
        return

    photo_path = Path(result)
    parts = [f"📷 {label}"]
    if zoom >= 65:
        parts.append("зум 4×")
    elif zoom >= 30:
        parts.append("зум 2×")
    elif zoom > 0:
        parts.append(f"зум {zoom}")
    if flash == "on" and facing == "back":
        parts.append("вспышка")
    elif flash == "auto" and facing == "back":
        parts.append("авто-вспышка")
    caption = " · ".join(parts)

    try:
        await message.answer_photo(FSInputFile(photo_path), caption=caption)
        try:
            await wait_msg.delete()
        except TelegramBadRequest:
            pass
    finally:
        try:
            photo_path.unlink(missing_ok=True)
        except OSError:
            pass


async def _render_clipboard(ctx: AppContext, notice: str = "") -> Tuple[str, KeyboardRows]:
    pc = ctx.default_pc()
    if pc:
        (t_ok, t_val), (p_ok, p_val, p_err) = await asyncio.gather(
            ctx.tablet.get_clipboard(),
            ctx.pc_driver(pc).get_clipboard(timeout=4.0),
        )
        pc_name = pc.name
        pc_display = p_val if p_ok else p_err
    else:
        t_ok, t_val = await ctx.tablet.get_clipboard()
        pc_name = "ПК не настроен"
        p_ok = False
        pc_display = "Нет ПК в конфигурации"
    return build_clipboard_panel(t_ok, t_val, pc_name, p_ok, pc_display, notice=notice)


async def _run_scene_kino(ctx: AppContext, app_key: str = "youtube") -> List[str]:
    steps: List[str] = ["🎬 Сценарий «Кино»:"]
    tv_online = await ctx.samsung_tv.is_online()
    if tv_online:
        steps.append("1. ✅ Samsung TV уже включён.")
    else:
        ok_w, msg_w = await ctx.samsung_tv.wake()
        steps.append(f"1. {'✅' if ok_w else '⚠️'} {msg_w}")
        if ok_w:
            await asyncio.sleep(2.0)

    ok_app, msg_app = await ctx.samsung_tv.launch_app(app_key)
    steps.append(f"2. {'✅' if ok_app else '⚠️'} {msg_app}")

    ok_bri, msg_bri = await ctx.tablet.set_brightness_pct(20)
    steps.append(f"3. {'✅' if ok_bri else '⚠️'} Планшет: {msg_bri}")

    pc = ctx.default_pc()
    if pc:
        drv = ctx.pc_driver(pc)
        if await drv.is_online(timeout=2.0):
            ok_m, msg_m, _ = await drv.action("mute")
            steps.append(f"4. {'✅' if ok_m else '⚠️'} {pc.name}: {msg_m}")
        else:
            steps.append(f"4. ℹ️ {pc.name} сейчас не в сети — звук не трогали.")
    else:
        steps.append("4. ℹ️ ПК не настроен в конфигурации.")
    return steps


async def _run_scene_work(ctx: AppContext) -> List[str]:
    steps: List[str] = ["💻 Сценарий «Работа»:"]
    pc = ctx.default_pc()
    if pc:
        drv = ctx.pc_driver(pc)
        ok_w, msg_w = await drv.wake()
        steps.append(f"1. {'✅' if ok_w else '⚠️'} {msg_w}")
    else:
        drv = None
        steps.append("1. ⚠️ ПК не настроен в конфигурации.")

    ok_bri, msg_bri = await ctx.tablet.set_brightness_pct(75)
    steps.append(f"2. {'✅' if ok_bri else '⚠️'} Планшет: {msg_bri}")

    t_stat = await ctx.tablet.get_status()
    batt = t_stat.get("battery") or {}
    cap = batt.get("capacity")
    vpn = t_stat.get("vpn_short") or "Выкл"
    steps.append(f"3. 📱 Планшет: заряд {cap if cap is not None else '—'}%, VPN: {vpn}")

    if pc and drv:
        ok_st, pstat, err_st = await drv.get_status(timeout=3.0)
        if ok_st and pstat:
            steps.append(
                f"4. 🟢 {pc.name}: в сети (CPU {pstat.get('cpu_pct', 0):.0f}%, RAM {pstat.get('ram_pct', 0):.0f}%)"
            )
        else:
            steps.append(f"4. 🟡 {pc.name}: просыпается ({err_st})")
    return steps


async def _run_scene_night(ctx: AppContext) -> List[str]:
    steps: List[str] = ["🌙 Сценарий «Ночь»:"]
    if await ctx.samsung_tv.is_online():
        ok_tv, msg_tv = await ctx.samsung_tv.send_key("power")
        steps.append(f"1. {'✅' if ok_tv else '⚠️'} {msg_tv}")
    else:
        steps.append("1. ℹ️ Samsung TV уже выключен.")

    pc = ctx.default_pc()
    if pc:
        drv = ctx.pc_driver(pc)
        if await drv.is_online(timeout=2.0):
            ok_sl, msg_sl, _ = await drv.action("sleep")
            steps.append(f"2. {'✅' if ok_sl else '⚠️'} {pc.name}: {msg_sl}")
        else:
            steps.append(f"2. ℹ️ {pc.name} уже не в сети.")
    else:
        steps.append("2. ℹ️ ПК не настроен в конфигурации.")

    ok_bri, msg_bri = await ctx.tablet.set_brightness_pct(5)
    steps.append(f"3. {'✅' if ok_bri else '⚠️'} Планшет: {msg_bri}")

    ok_nc, msg_nc = await ctx.tablet.set_night_charging(True)
    steps.append(f"4. {'✅' if ok_nc else '⚠️'} Батарея: {msg_nc}")
    return steps


async def _render_status(ctx: AppContext) -> Tuple[str, KeyboardRows]:
    async def check_pc(pc: PCConfig) -> Tuple[PCConfig, bool, Optional[Dict[str, Any]]]:
        ok, stat, _ = await ctx.pc_driver(pc).get_status(timeout=3.0)
        return pc, ok, stat

    tablet_stat, s_on, a_on, pc_states = await asyncio.gather(
        ctx.tablet.get_status(),
        ctx.samsung_tv.is_online(),
        ctx.android_tv.is_online(),
        asyncio.gather(*(check_pc(pc) for pc in ctx.config.pcs)),
    )
    return build_status_summary(tablet_stat, pc_states, s_on, a_on, ctx.config)


async def _render_vpn(ctx: AppContext, notice: str = "") -> Tuple[str, KeyboardRows]:
    profiles = await ctx.tablet.list_vpn_profiles()
    ctx.vpn_profiles_cache = profiles
    raw_status, _, active = await ctx.tablet.vpn_status()
    return build_vpn_panel(profiles, raw_status, active_profile=active, notice=notice)


async def _render_pc_list(ctx: AppContext, notice: str = "") -> Tuple[str, KeyboardRows]:
    async def check(pc: PCConfig) -> Tuple[PCConfig, bool]:
        return pc, await ctx.pc_driver(pc).is_online(timeout=2.0)

    states = await asyncio.gather(*(check(pc) for pc in ctx.config.pcs))
    return build_pc_list_panel(states, notice=notice)


async def _render_pc_entry(ctx: AppContext) -> Tuple[str, KeyboardRows]:
    if len(ctx.config.pcs) == 1:
        pc = ctx.config.pcs[0]
        ok, stat, _ = await ctx.pc_driver(pc).get_status(timeout=3.0)
        return build_pc_detail_panel(pc, ok, stat)
    return await _render_pc_list(ctx)


async def _render_network(ctx: AppContext, deep: bool = False) -> Tuple[str, KeyboardRows]:
    wifi, ping_ms, (_, vpn_short, _), devices = await asyncio.gather(
        ctx.network.get_wifi_info(),
        ctx.network.ping_internet(),
        ctx.tablet.vpn_status(),
        ctx.network.scan_lan(deep_sweep=deep),
    )
    notice = "Полное сканирование подсети завершено." if deep else ""
    return build_network_panel(wifi, vpn_short, ping_ms, devices, notice=notice)


async def alert_watcher_loop(bot: Bot, ctx: AppContext, stop_event: asyncio.Event) -> None:
    """Background task sending boot ping and periodic low-battery / overheat alerts."""
    if ctx.config.owner_id and ctx.config.alerts.enabled and ctx.config.alerts.startup_notify:
        try:
            st = await ctx.tablet.get_status()
            batt = st.get("battery") or {}
            cap = batt.get("capacity")
            ip = st.get("wifi_ip") or ctx.config.network.tablet_ip
            await bot.send_message(
                ctx.config.owner_id,
                f"🚀 <b>pipa-tg-bot запущен</b>\n"
                f"📱 Xiaomi Pad 6 в сети (<code>{esc(ip)}</code>)"
                + (f" · Заряд: <b>{cap}%</b>" if cap is not None else ""),
            )
        except Exception as exc:
            logger.warning("Could not send startup alert: %s", exc)

    while not stop_event.is_set():
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=ctx.config.alerts.poll_interval_sec)
            break
        except asyncio.TimeoutError:
            pass

        if not ctx.config.owner_id or not ctx.config.alerts.enabled:
            continue
        try:
            st = await ctx.tablet.get_status()
            messages = ctx.alert_watcher.evaluate(st, now=time.monotonic())
            for alert_text in messages:
                await bot.send_message(ctx.config.owner_id, alert_text)
        except Exception as exc:
            logger.warning("Alert watcher check failed: %s", exc)


async def register_commands(bot: Bot) -> None:
    me = await bot.get_me()
    tg_cmds = [BotCommand(command=c.command, description=c.description) for c in BOT_COMMANDS]
    await bot.set_my_commands(tg_cmds)
    logger.info("Connected as @%s (id=%s); registered %d Russian Telegram bot commands.", me.username, me.id, len(tg_cmds))


async def main() -> None:
    config = load_config()
    if not config.bot_token:
        logger.error(
            "BOT_TOKEN is missing! Set BOT_TOKEN in %s or environment.",
            config.env_path,
        )
        sys.exit(2)

    ctx = AppContext(config)
    if not ctx.auth.is_paired:
        logger.warning("=" * 60)
        logger.warning("OWNER_ID is not set! Zero-friction pairing enabled.")
        logger.warning("PAIR CODE: %s  (saved to %s)", ctx.auth.pair_code, config.pair_code_path)
        logger.warning("Send `/pair %s` to the bot from your Telegram account.", ctx.auth.pair_code)
        logger.warning("=" * 60)
    else:
        logger.info("Owner locked to Telegram user_id=%s", config.owner_id)

    session = None
    if config.telegram_proxy:
        logger.info("Using Telegram proxy: %s", config.telegram_proxy)
        session = AiohttpSession(proxy=config.telegram_proxy)

    bot = Bot(
        token=config.bot_token,
        session=session,
        default=DefaultBotProperties(parse_mode=ParseMode.HTML),
    )
    dp = Dispatcher(storage=MemoryStorage())
    auth_mw = OwnerAuthMiddleware(ctx.auth)
    dp.message.middleware(auth_mw)
    dp.callback_query.middleware(auth_mw)
    dp.include_router(create_router(ctx))

    await register_commands(bot)

    try:
        media_port = await ctx.media_server.start()
        logger.info("Local LAN media server listening on 0.0.0.0:%d for Samsung TV DLNA cast.", media_port)
    except OSError as exc:
        logger.warning("Could not start LocalMediaServer on port %d: %s", config.media_server_port, exc)

    stop_event = asyncio.Event()
    watcher_task = asyncio.create_task(alert_watcher_loop(bot, ctx, stop_event))

    loop = asyncio.get_running_loop()
    for sig_name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, sig_name, None)
        if sig is not None:
            try:
                loop.add_signal_handler(sig, stop_event.set)
            except (NotImplementedError, RuntimeError):
                pass

    try:
        polling_task = asyncio.create_task(
            dp.start_polling(bot, handle_signals=False, allowed_updates=dp.resolve_used_update_types())
        )
        stop_wait = asyncio.create_task(stop_event.wait())
        done, pending = await asyncio.wait(
            {polling_task, stop_wait},
            return_when=asyncio.FIRST_COMPLETED,
        )
        if stop_wait in done and not polling_task.done():
            await dp.stop_polling()
            await polling_task
        for task in pending:
            task.cancel()
    finally:
        stop_event.set()
        watcher_task.cancel()
        try:
            await watcher_task
        except asyncio.CancelledError:
            pass
        await ctx.media_server.stop()
        await bot.session.close()
        logger.info("pipa-tg-bot stopped cleanly.")


if __name__ == "__main__":
    asyncio.run(main())
