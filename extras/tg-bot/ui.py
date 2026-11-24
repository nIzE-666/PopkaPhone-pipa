"""Russian inline-keyboard UI builders and HTML formatters for pipa-tg-bot."""

from __future__ import annotations

import html
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from .callbacks import pack_cb
    from .config import AlertsConfig, BotConfig, PCConfig
except ImportError:
    from callbacks import pack_cb
    from config import AlertsConfig, BotConfig, PCConfig


@dataclass(frozen=True)
class ButtonSpec:
    text: str
    callback_data: str


KeyboardRows = List[List[ButtonSpec]]


def esc(value: Any) -> str:
    return html.escape(str(value), quote=True)


def render_bar(percent: Optional[float], width: int = 10) -> str:
    """Render compact visual progress bar like '███████░░░ 68%'."""
    if percent is None:
        return "░" * width + " —"
    clamped = max(0.0, min(100.0, float(percent)))
    filled = int(round((clamped / 100.0) * width))
    return f"{'█' * filled}{'░' * (width - filled)} {int(round(clamped))}%"


def to_aiogram_markup(rows: KeyboardRows) -> Any:
    """Convert platform-independent ButtonSpec rows to aiogram InlineKeyboardMarkup."""
    from aiogram.types import InlineKeyboardButton, InlineKeyboardMarkup

    return InlineKeyboardMarkup(
        inline_keyboard=[
            [InlineKeyboardButton(text=btn.text, callback_data=btn.callback_data) for btn in row]
            for row in rows
        ]
    )


def format_duration(seconds: Optional[float]) -> str:
    if seconds is None or seconds < 0:
        return "—"
    total = int(seconds)
    days, rem = divmod(total, 86400)
    hours, rem = divmod(rem, 3600)
    mins, _ = divmod(rem, 60)
    if days > 0:
        return f"{days}д {hours}ч {mins}м"
    if hours > 0:
        return f"{hours}ч {mins}м"
    return f"{mins}м"


def _batt_icon(pct: Optional[int], status: str) -> str:
    st = (status or "").lower()
    if "charging" in st and "dis" not in st:
        return "⚡"
    if pct is None:
        return "🔋"
    if pct <= 15:
        return "🪫"
    return "🔋"


def _online_badge(online: Optional[bool]) -> str:
    if online is True:
        return "🟢 В сети"
    if online is False:
        return "🔴 Не в сети"
    return "⚪ Неизвестно"


# ---------------------------------------------------------------------------
# Main Dashboard & Quick Summary
# ---------------------------------------------------------------------------

def build_main_dashboard(
    tablet: Optional[Dict[str, Any]] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = ["🎛 <b>Центр управления — Xiaomi Pad 6 (pipa)</b>"]
    if tablet:
        batt = tablet.get("battery") or {}
        pct = batt.get("capacity")
        st = batt.get("status") or "—"
        btemp = batt.get("temp_c")
        bri = tablet.get("brightness_pct")
        vol = tablet.get("volume_pct")
        muted = tablet.get("muted")
        cpu_pct = tablet.get("cpu_pct")
        cpu_temp = tablet.get("cpu_temp_c")
        ram_pct = tablet.get("ram_pct")
        vpn_line = tablet.get("vpn_short") or "Выкл"

        bicon = _batt_icon(pct, st)
        temp_str = f" · {btemp:.1f}°C" if btemp is not None else ""
        vol_str = "🔇 Без звука" if muted else (f"🔊 {vol}%" if vol is not None else "🔊 —")
        bri_str = f"☀️ {bri}%" if bri is not None else "☀️ —"
        cpu_str = f"{cpu_pct:.0f}%" if cpu_pct is not None else "—"
        ctemp_str = f" ({cpu_temp:.0f}°C)" if cpu_temp is not None else ""
        ram_str = f"{ram_pct:.0f}%" if ram_pct is not None else "—"

        lines.extend(
            [
                "",
                f"{bicon} <b>Заряд:</b> <code>{render_bar(pct, 8)}</code> ({esc(st)}{temp_str})",
                f"🧠 <b>CPU:</b> {cpu_str}{ctemp_str}  ·  <b>RAM:</b> {ram_str}",
                f"{bri_str}  ·  {vol_str}  ·  🛡 <b>VPN:</b> {esc(vpn_line)}",
            ]
        )
    else:
        lines.extend(["", "Выберите раздел ниже для управления устройствами."])

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("📱 Планшет", pack_cb("t", "open")),
            ButtonSpec("💻 Компьютеры", pack_cb("p", "list")),
        ],
        [
            ButtonSpec("📺 Телевизоры", pack_cb("tv", "menu")),
            ButtonSpec("🌐 Домашняя сеть", pack_cb("n", "open")),
        ],
        [
            ButtonSpec("✨ Сценарии", pack_cb("sc", "open")),
            ButtonSpec("📋 Буфер обмена", pack_cb("cl", "open")),
        ],
        [
            ButtonSpec("💻 Терминал", pack_cb("tm", "open")),
            ButtonSpec("🛡 VPN", pack_cb("v", "open")),
        ],
        [
            ButtonSpec("⚙️ Службы", pack_cb("sv", "open")),
            ButtonSpec("📸 Скриншот", pack_cb("t", "shot")),
        ],
        [
            ButtonSpec("📷 Камера", pack_cb("m", "cam")),
            ButtonSpec("🔔 Уведомления", pack_cb("al", "open")),
        ],
        [
            ButtonSpec("📊 Сводка", pack_cb("m", "status")),
            ButtonSpec("🔄 Обновить", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


def build_status_summary(
    tablet: Dict[str, Any],
    pc_states: Sequence[Tuple[PCConfig, bool, Optional[Dict[str, Any]]]],
    samsung_online: bool,
    android_online: bool,
    config: BotConfig,
) -> Tuple[str, KeyboardRows]:
    batt = tablet.get("battery") or {}
    pct = batt.get("capacity")
    st = batt.get("status") or "—"
    btemp = batt.get("temp_c")
    ram_used = tablet.get("ram_used_mb")
    ram_total = tablet.get("ram_total_mb")
    disk_free = tablet.get("disk_free_gb")
    disk_total = tablet.get("disk_total_gb")

    lines = [
        "📊 <b>Быстрая сводка устройств</b>",
        "",
        "<b>📱 Планшет (pipa)</b>",
        f"• Заряд: <code>{render_bar(pct, 8)}</code> ({esc(st)}{f', {btemp:.1f}°C' if btemp is not None else ''})",
        f"• CPU: {tablet.get('cpu_pct', 0):.0f}%"
        + (f" ({tablet['cpu_temp_c']:.1f}°C)" if tablet.get("cpu_temp_c") is not None else "")
        + (f" · RAM: {ram_used}/{ram_total} МБ" if ram_total else ""),
        (
            f"• Диск: свободно {disk_free:.1f} из {disk_total:.1f} ГБ · Аптайм: {format_duration(tablet.get('uptime_sec'))}"
            if disk_total
            else f"• Аптайм: {format_duration(tablet.get('uptime_sec'))}"
        ),
        f"• Wi-Fi: {esc(tablet.get('wifi_ip') or config.network.tablet_ip)} · VPN: {esc(tablet.get('vpn_short') or 'Выкл')}",
        "",
        "<b>💻 Компьютеры</b>",
    ]
    for pc, online, pstat in pc_states:
        extra = ""
        if online and pstat:
            cpu = pstat.get("cpu_pct")
            ram = pstat.get("ram_pct")
            if cpu is not None and ram is not None:
                extra = f" (CPU {cpu:.0f}%, RAM {ram:.0f}%)"
        lines.append(f"• {esc(pc.name)} (<code>{esc(pc.host)}</code>): {_online_badge(online)}{extra}")

    lines.extend(
        [
            "",
            "<b>📺 Телевизоры</b>",
            f"• {esc(config.samsung_tv.name)} (<code>{esc(config.samsung_tv.host)}</code>): {_online_badge(samsung_online)}",
            f"• {esc(config.android_tv.name)} (<code>{esc(config.android_tv.host)}</code>): {_online_badge(android_online)}",
        ]
    )

    rows: KeyboardRows = [
        [
            ButtonSpec("🔄 Обновить сводку", pack_cb("m", "status")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ]
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Tablet Panel
# ---------------------------------------------------------------------------

def build_tablet_panel(tablet: Dict[str, Any], notice: str = "") -> Tuple[str, KeyboardRows]:
    batt = tablet.get("battery") or {}
    pct = batt.get("capacity")
    st = batt.get("status") or "—"
    btemp = batt.get("temp_c")
    watts = batt.get("power_w")
    bri_pct = tablet.get("brightness_pct")
    vol_pct = tablet.get("volume_pct")
    muted = tablet.get("muted")
    ram_used = tablet.get("ram_used_mb")
    ram_total = tablet.get("ram_total_mb")
    disk_free = tablet.get("disk_free_gb")
    disk_total = tablet.get("disk_total_gb")

    power_part = f" · {watts:+.1f} Вт" if watts is not None else ""
    btemp_part = f" · {btemp:.1f}°C" if btemp is not None else ""
    vol_display = "🔇 Выключен" if muted else f"<code>{render_bar(vol_pct, 8)}</code>"

    lines = [
        "📱 <b>Планшет Xiaomi Pad 6 (pipa)</b>",
        "",
        f"{_batt_icon(pct, st)} <b>Батарея:</b> <code>{render_bar(pct, 8)}</code> ({esc(st)}{btemp_part}{power_part})",
        f"☀️ <b>Яркость:</b> <code>{render_bar(bri_pct, 8)}</code>",
        f"🔊 <b>Громкость:</b> {vol_display}",
        f"🧠 <b>CPU:</b> {tablet.get('cpu_pct', 0):.0f}%"
        + (f" ({tablet['cpu_temp_c']:.1f}°C)" if tablet.get("cpu_temp_c") is not None else "")
        + (f"  ·  <b>RAM:</b> {ram_used}/{ram_total} МБ" if ram_total else ""),
        (
            f"💾 <b>Диск:</b> {disk_free:.1f}/{disk_total:.1f} ГБ своб.  ·  ⏱ {format_duration(tablet.get('uptime_sec'))}"
            if disk_total
            else f"⏱ <b>Аптайм:</b> {format_duration(tablet.get('uptime_sec'))}"
        ),
        f"🛡 <b>VPN:</b> {esc(tablet.get('vpn_short') or 'Выкл')}",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("☀️ 10%", pack_cb("t", "bri", 10)),
            ButtonSpec("☀️ 30%", pack_cb("t", "bri", 30)),
            ButtonSpec("☀️ 60%", pack_cb("t", "bri", 60)),
            ButtonSpec("☀️ 100%", pack_cb("t", "bri", 100)),
        ],
        [
            ButtonSpec("🔅 -10%", pack_cb("t", "bstep", -10)),
            ButtonSpec("🔆 +10%", pack_cb("t", "bstep", 10)),
            ButtonSpec("🌙 Экран Выкл", pack_cb("t", "scroff")),
            ButtonSpec("💡 Экран Вкл", pack_cb("t", "scron")),
        ],
        [
            ButtonSpec("🔉 -", pack_cb("t", "voldn")),
            ButtonSpec("🔇 Без зв.", pack_cb("t", "mute")),
            ButtonSpec("🔊 +", pack_cb("t", "volup")),
        ],
        [
            ButtonSpec("📸 Скриншот", pack_cb("t", "shot")),
            ButtonSpec("🛡 VPN", pack_cb("v", "open")),
            ButtonSpec("⚙️ Службы", pack_cb("sv", "open")),
        ],
        [
            ButtonSpec("🔁 Перезагрузка", pack_cb("cf", "ask", "treb")),
            ButtonSpec("🤖 В One UI", pack_cb("cf", "ask", "toneui")),
        ],
        [
            ButtonSpec("🔄 Обновить", pack_cb("t", "open")),
            ButtonSpec("⬅️ Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# VPN Panel
# ---------------------------------------------------------------------------

def build_vpn_panel(
    profiles: Sequence[str],
    status_text: str,
    active_profile: Optional[str] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = [
        "🛡 <b>Управление VPN</b>",
        "",
        f"<b>Текущий статус:</b>\n<pre>{esc(status_text or 'Неизвестно')}</pre>",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = []
    # 2 profile buttons per row; use index to guarantee callback_data <= 64 bytes
    buf: List[ButtonSpec] = []
    for idx, prof in enumerate(profiles):
        prefix = "✅ " if active_profile and active_profile == prof else "🌐 "
        buf.append(ButtonSpec(f"{prefix}{prof}", pack_cb("v", "on", idx)))
        if len(buf) == 2:
            rows.append(buf)
            buf = []
    if buf:
        rows.append(buf)

    rows.append([ButtonSpec("⛔ Отключить VPN", pack_cb("v", "off"))])
    rows.append(
        [
            ButtonSpec("🔄 Обновить", pack_cb("v", "open")),
            ButtonSpec("📱 Планшет", pack_cb("t", "open")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ]
    )
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Services Panel
# ---------------------------------------------------------------------------

def build_services_panel(
    services: Sequence[Dict[str, Any]],
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = ["⚙️ <b>Службы планшета</b>", ""]
    rows: KeyboardRows = []

    for svc in services:
        key = svc["key"]
        label = svc["label"]
        active = bool(svc.get("active"))
        state = svc.get("state") or ("active" if active else "inactive")
        critical = bool(svc.get("critical"))
        icon = "🟢" if active else "🔴"
        lines.append(f"{icon} <b>{esc(label)}</b> — <code>{esc(state)}</code>")

        if critical:
            rows.append(
                [
                    ButtonSpec(f"🔁 Перезапустить {label}", pack_cb("cf", "ask", f"svr_{key}")),
                ]
            )
        else:
            toggle_btn = (
                ButtonSpec(f"⏹ Стоп {label}", pack_cb("sv", "stop", key))
                if active
                else ButtonSpec(f"▶️ Старт {label}", pack_cb("sv", "start", key))
            )
            rows.append(
                [
                    toggle_btn,
                    ButtonSpec("🔁 Рестарт", pack_cb("sv", "rst", key)),
                ]
            )

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows.append(
        [
            ButtonSpec("🔄 Обновить", pack_cb("sv", "open")),
            ButtonSpec("📱 Планшет", pack_cb("t", "open")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ]
    )
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# PC Panels
# ---------------------------------------------------------------------------

def build_pc_list_panel(
    pc_states: Sequence[Tuple[PCConfig, bool]],
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = ["💻 <b>Компьютеры</b>", ""]
    rows: KeyboardRows = []
    if not pc_states:
        lines.append("Список компьютеров пуст. Добавьте ПК в <code>~/.config/pipa-bot/config.json</code>.")
    for pc, online in pc_states:
        badge = "🟢" if online else "🔴"
        lines.append(f"{badge} <b>{esc(pc.name)}</b> — <code>{esc(pc.host)}</code>")
        rows.append([ButtonSpec(f"{badge} {pc.name}", pack_cb("p", "open", pc.id))])

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows.append(
        [
            ButtonSpec("🔄 Обновить", pack_cb("p", "list")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ]
    )
    return "\n".join(lines), rows


def build_pc_detail_panel(
    pc: PCConfig,
    online: bool,
    status: Optional[Dict[str, Any]] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = [
        f"💻 <b>{esc(pc.name)}</b>",
        f"🌐 <code>{esc(pc.host)}:{pc.port}</code>  ·  {_online_badge(online)}",
    ]
    if pc.mac:
        lines.append(f"🔌 MAC: <code>{esc(pc.mac)}</code>")

    if online and status:
        cpu = status.get("cpu_pct")
        ram_used = status.get("ram_used_mb")
        ram_total = status.get("ram_total_mb")
        ram_pct = status.get("ram_pct")
        uptime = status.get("uptime_sec")
        active_win = status.get("active_window")
        lines.append("")
        if cpu is not None:
            lines.append(f"🧠 <b>CPU:</b> <code>{render_bar(cpu, 8)}</code>")
        if ram_pct is not None:
            ram_extra = f" ({ram_used}/{ram_total} МБ)" if ram_total else ""
            lines.append(f"💾 <b>RAM:</b> <code>{render_bar(ram_pct, 8)}</code>{ram_extra}")
        if uptime is not None:
            lines.append(f"⏱ <b>Аптайм:</b> {format_duration(uptime)}")
        if active_win:
            lines.append(f"🪟 <b>Окно:</b> {esc(active_win[:80])}")
        batt = status.get("battery")
        if isinstance(batt, dict) and batt.get("percent") is not None:
            ac = "⚡ от сети" if batt.get("ac_online") else "🔋 от батареи"
            lines.append(f"🔋 <b>Батарея:</b> {batt['percent']}% ({ac})")
    elif not online:
        lines.extend(["", "Агент <code>pipa-pc-agent</code> не отвечает. Вы можете разбудить ПК через Wake-on-LAN."])

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    pid = pc.id
    rows: KeyboardRows = [
        [
            ButtonSpec("⚡ Разбудить (WoL)", pack_cb("p", "wol", pid)),
            ButtonSpec("📸 Скриншот", pack_cb("p", "shot", pid)),
        ],
        [
            ButtonSpec("🔒 Блокировать", pack_cb("p", "act", pid, "lock")),
            ButtonSpec("😴 Сон", pack_cb("p", "act", pid, "sleep")),
        ],
        [
            ButtonSpec("🔉 -", pack_cb("p", "act", pid, "vol_down")),
            ButtonSpec("🔇 Mute", pack_cb("p", "act", pid, "vol_mute")),
            ButtonSpec("🔊 +", pack_cb("p", "act", pid, "vol_up")),
        ],
        [
            ButtonSpec("⏮ Пред.", pack_cb("p", "act", pid, "media_prev")),
            ButtonSpec("⏯ Пауза", pack_cb("p", "act", pid, "media_play_pause")),
            ButtonSpec("⏭ След.", pack_cb("p", "act", pid, "media_next")),
        ],
        [
            ButtonSpec("🔁 Перезагрузка", pack_cb("cf", "ask", f"preb_{pid}")),
            ButtonSpec("⏻ Выключить", pack_cb("cf", "ask", f"poff_{pid}")),
        ],
        [
            ButtonSpec("🔄 Обновить", pack_cb("p", "open", pid)),
            ButtonSpec("⬅️ Компьютеры", pack_cb("p", "list")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# TV Panels (Selector, Samsung TV, Android TV)
# ---------------------------------------------------------------------------

def build_tv_selector_panel(
    config: BotConfig,
    samsung_online: Optional[bool] = None,
    android_online: Optional[bool] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = [
        "📺 <b>Телевизоры и медиаприставки</b>",
        "",
        f"• <b>{esc(config.samsung_tv.name)}</b> (<code>{esc(config.samsung_tv.host)}</code>): {_online_badge(samsung_online)}",
        f"• <b>{esc(config.android_tv.name)}</b> (<code>{esc(config.android_tv.host)}</code>): {_online_badge(android_online)}",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [ButtonSpec(f"📺 {config.samsung_tv.name}", pack_cb("s", "open"))],
        [ButtonSpec(f"🤖 {config.android_tv.name}", pack_cb("a", "open"))],
        [
            ButtonSpec("🔄 Проверить сеть", pack_cb("tv", "menu")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


def build_samsung_tv_panel(
    config: BotConfig,
    online: Optional[bool] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    tv = config.samsung_tv
    lines = [
        f"📺 <b>Пульт — {esc(tv.name)}</b>",
        f"🌐 <code>{esc(tv.host)}:{tv.port}</code>  ·  {_online_badge(online)}",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("🔌 Питание", pack_cb("s", "k", "power")),
            ButtonSpec("📺 Источник", pack_cb("s", "k", "source")),
            ButtonSpec("▶️ YouTube", pack_cb("s", "app", "youtube")),
        ],
        [
            ButtonSpec("·", pack_cb("s", "open")),
            ButtonSpec("⬆️", pack_cb("s", "k", "up")),
            ButtonSpec("·", pack_cb("s", "open")),
        ],
        [
            ButtonSpec("⬅️", pack_cb("s", "k", "left")),
            ButtonSpec("🔘 OK", pack_cb("s", "k", "enter")),
            ButtonSpec("➡️", pack_cb("s", "k", "right")),
        ],
        [
            ButtonSpec("🔙 Назад", pack_cb("s", "k", "back")),
            ButtonSpec("⬇️", pack_cb("s", "k", "down")),
            ButtonSpec("🏠 Домой", pack_cb("s", "k", "home")),
        ],
        [
            ButtonSpec("🔊+", pack_cb("s", "k", "vol_up")),
            ButtonSpec("🔊-", pack_cb("s", "k", "vol_down")),
            ButtonSpec("🔇", pack_cb("s", "k", "mute")),
            ButtonSpec("⏯ Пауза", pack_cb("s", "k", "play_pause")),
        ],
        [
            ButtonSpec("⌨️ Ввести текст", pack_cb("s", "text")),
            ButtonSpec("📺 Cast URL", pack_cb("s", "cast")),
            ButtonSpec("⏹ Стоп Cast", pack_cb("s", "cstop")),
        ],
        [
            ButtonSpec("🔼 Кан+", pack_cb("s", "k", "ch_up")),
            ButtonSpec("🔽 Кан-", pack_cb("s", "k", "ch_down")),
            ButtonSpec("⚡ WoL", pack_cb("s", "wol")),
        ],
        [
            ButtonSpec("⬅️ Телевизоры", pack_cb("tv", "menu")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


def build_android_tv_panel(
    config: BotConfig,
    online: Optional[bool] = None,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    tv = config.android_tv
    lines = [
        f"🤖 <b>Пульт — {esc(tv.name)}</b>",
        f"🌐 <code>{esc(tv.host)}:{tv.adb_port}</code> (ADB)  ·  {_online_badge(online)}",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("🔌 Питание", pack_cb("a", "k", "power")),
            ButtonSpec("📸 Скриншот ТВ", pack_cb("a", "shot")),
            ButtonSpec("⌨️ Ввод текста", pack_cb("a", "text")),
        ],
        [
            ButtonSpec("·", pack_cb("a", "open")),
            ButtonSpec("⬆️", pack_cb("a", "k", "up")),
            ButtonSpec("·", pack_cb("a", "open")),
        ],
        [
            ButtonSpec("⬅️", pack_cb("a", "k", "left")),
            ButtonSpec("🔘 OK", pack_cb("a", "k", "ok")),
            ButtonSpec("➡️", pack_cb("a", "k", "right")),
        ],
        [
            ButtonSpec("🔙 Назад", pack_cb("a", "k", "back")),
            ButtonSpec("⬇️", pack_cb("a", "k", "down")),
            ButtonSpec("🏠 Домой", pack_cb("a", "k", "home")),
        ],
        [
            ButtonSpec("🔊+", pack_cb("a", "k", "vol_up")),
            ButtonSpec("🔊-", pack_cb("a", "k", "vol_down")),
            ButtonSpec("🔇", pack_cb("a", "k", "mute")),
            ButtonSpec("⏯ Пауза", pack_cb("a", "k", "play_pause")),
        ],
        [
            ButtonSpec("▶️ YouTube", pack_cb("a", "app", "youtube")),
            ButtonSpec("🎬 Кинопоиск", pack_cb("a", "app", "kinopoisk")),
            ButtonSpec("🟣 Twitch", pack_cb("a", "app", "twitch")),
            ButtonSpec("⚙️ Настр.", pack_cb("a", "app", "settings")),
        ],
        [
            ButtonSpec("⬅️ Телевизоры", pack_cb("tv", "menu")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Network Panel
# ---------------------------------------------------------------------------

def build_network_panel(
    wifi: Dict[str, Any],
    vpn_short: str,
    ping_ms: Optional[float],
    devices: Sequence[Dict[str, Any]],
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    ssid = wifi.get("ssid") or "—"
    ip = wifi.get("ip") or "—"
    signal = wifi.get("signal_dbm")
    sig_str = f"{signal} дБм" if signal is not None else "—"
    ping_str = f"{ping_ms:.1f} мс" if ping_ms is not None else "нет ответа"

    lines = [
        "🌐 <b>Домашняя сеть и Wi-Fi</b>",
        "",
        f"📶 <b>Wi-Fi:</b> {esc(ssid)}  ·  IP: <code>{esc(ip)}</code>",
        f"📡 <b>Сигнал:</b> {sig_str}  ·  🌍 <b>Пинг:</b> {ping_str}",
        f"🛡 <b>VPN:</b> {esc(vpn_short or 'Выкл')}",
        "",
        f"<b>Активные устройства в сети ({len(devices)}):</b>",
    ]
    if not devices:
        lines.append("• Устройства не обнаружены.")
    else:
        for dev in devices[:25]:
            label = f" — <b>{esc(dev['label'])}</b>" if dev.get("label") else ""
            mac = f" (<code>{esc(dev['mac'])}</code>)" if dev.get("mac") else ""
            state = dev.get("state") or ""
            dot = "🟢" if state in ("REACHABLE", "ONLINE", "DELAY", "PROBE") else "⚪"
            lines.append(f"{dot} <code>{esc(dev['ip'])}</code>{label}{mac}")
        if len(devices) > 25:
            lines.append(f"… и ещё {len(devices) - 25}")

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("🔍 Сканировать сеть", pack_cb("n", "scan")),
            ButtonSpec("🛡 VPN", pack_cb("v", "open")),
        ],
        [
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Alerts Panel
# ---------------------------------------------------------------------------

def build_alerts_panel(alerts: AlertsConfig, notice: str = "") -> Tuple[str, KeyboardRows]:
    def flag(v: bool) -> str:
        return "✅ Вкл" if v else "⛔ Выкл"

    lines = [
        "🔔 <b>Настройка авто-уведомлений</b>",
        "",
        f"• Общий переключатель: <b>{flag(alerts.enabled)}</b>",
        f"• Низкий заряд (&lt; {alerts.low_battery_pct}%): <b>{flag(alerts.low_battery_enabled)}</b>",
        f"• Перегрев (CPU &gt; {alerts.max_cpu_temp_c:.0f}°C / Бат. &gt; {alerts.max_battery_temp_c:.0f}°C): <b>{flag(alerts.overheat_enabled)}</b>",
        f"• Уведомление при запуске бота: <b>{flag(alerts.startup_notify)}</b>",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec(
                f"{'🔕 Выкл все' if alerts.enabled else '🔔 Вкл все'}",
                pack_cb("al", "tog", "all"),
            )
        ],
        [
            ButtonSpec(
                f"🪫 Низкий заряд: {'Вкл' if alerts.low_battery_enabled else 'Выкл'}",
                pack_cb("al", "tog", "batt"),
            ),
            ButtonSpec(
                f"🌡 Перегрев: {'Вкл' if alerts.overheat_enabled else 'Выкл'}",
                pack_cb("al", "tog", "heat"),
            ),
        ],
        [
            ButtonSpec(
                f"🚀 При старте: {'Вкл' if alerts.startup_notify else 'Выкл'}",
                pack_cb("al", "tog", "boot"),
            ),
        ],
        [
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# 2-Step Confirmation Dialog
# ---------------------------------------------------------------------------

def build_confirm_dialog(
    title: str,
    warning: str,
    confirm_token: str,
    cancel_cb: str,
) -> Tuple[str, KeyboardRows]:
    lines = [
        f"⚠️ <b>Подтверждение: {esc(title)}</b>",
        "",
        esc(warning),
        "",
        "Вы уверены, что хотите продолжить?",
    ]
    rows: KeyboardRows = [
        [
            ButtonSpec("✅ Да, выполнить", pack_cb("cf", "yes", confirm_token)),
            ButtonSpec("❌ Отмена", cancel_cb),
        ]
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Bidirectional Clipboard Panel
# ---------------------------------------------------------------------------

def _format_clipboard_preview(ok: bool, text: str, max_chars: int = 240) -> str:
    if not ok:
        return f"🔴 <i>{esc(text or 'Недоступен')}</i>"
    if not text:
        return "<i>(пусто)</i>"
    trimmed = text if len(text) <= max_chars else text[:max_chars] + "…"
    return f"<pre>{esc(trimmed)}</pre><i>({len(text)} симв.)</i>"


def build_clipboard_panel(
    tablet_ok: bool,
    tablet_text: str,
    pc_name: str,
    pc_ok: bool,
    pc_text: str,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = [
        "📋 <b>Буфер обмена (Планшет ↔ ПК)</b>",
        "",
        "<b>📱 Планшет (pipa):</b>",
        _format_clipboard_preview(tablet_ok, tablet_text),
        "",
        f"<b>💻 {esc(pc_name)}:</b>",
        _format_clipboard_preview(pc_ok, pc_text),
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("📋 Планшет → ПК", pack_cb("cl", "t2p")),
            ButtonSpec("💻 ПК → Планшет", pack_cb("cl", "p2t")),
        ],
        [
            ButtonSpec("✍️ Записать в буфер планшета", pack_cb("cl", "wt")),
        ],
        [
            ButtonSpec("✍️ Записать в буфер ПК", pack_cb("cl", "wp")),
        ],
        [
            ButtonSpec("🔄 Обновить", pack_cb("cl", "open")),
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Macro Scenes Panel
# ---------------------------------------------------------------------------

def build_scenes_panel(
    notice: str = "",
    report_lines: Sequence[str] = (),
) -> Tuple[str, KeyboardRows]:
    lines = [
        "✨ <b>Сценарии в одно нажатие</b>",
        "",
        "• <b>🎬 Кино</b> — включает Samsung TV (WoL при необходимости), запускает YouTube или Кинопоиск, снижает яркость планшета до 20% и глушит звук ПК.",
        "• <b>💻 Работа</b> — будит ПК (Windows) (WoL), выставляет яркость планшета 75% и проверяет статус ПК и планшета.",
        "• <b>🌙 Ночь</b> — выключает Samsung TV, усыпляет ПК (Windows) (с подтверждением), снижает яркость до 5% и включает лимит заряда 80%.",
    ]
    if report_lines:
        lines.append("")
        lines.append("<b>Отчёт выполнения:</b>")
        for step in report_lines:
            lines.append(esc(step))
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("🎬 Кино (YouTube)", pack_cb("sc", "kino", "yt")),
            ButtonSpec("🎬 Кино (Кинопоиск)", pack_cb("sc", "kino", "kp")),
        ],
        [
            ButtonSpec("💻 Работа", pack_cb("sc", "work")),
            ButtonSpec("🌙 Ночь", pack_cb("cf", "ask", "sc_night")),
        ],
        [
            ButtonSpec("🏠 Главная", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Interactive Persistent Terminal Panel
# ---------------------------------------------------------------------------

def build_terminal_panel(
    cwd: str,
    command: str = "",
    rc: Optional[int] = None,
    output: str = "",
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    lines = [
        "💻 <b>Интерактивный терминал (bash)</b>",
        f"📂 <code>{esc(cwd)}</code>",
    ]
    if command:
        badge = "✅" if rc == 0 else "❌"
        lines.append(f"$ <code>{esc(command)}</code>  ·  {badge} код <b>{rc}</b>")
        cleaned = output.strip() if output is not None else ""
        lines.append(f"<pre>{esc(cleaned or '(пустой вывод)')}</pre>")
    else:
        lines.extend(
            [
                "",
                "Отправьте любую shell-команду обычным сообщением — текущая папка (<code>cd</code>) сохраняется между командами.",
            ]
        )

    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("📂 ls -la", pack_cb("tm", "q", "ls")),
            ButtonSpec("🧠 free -h", pack_cb("tm", "q", "free")),
        ],
        [
            ButtonSpec("💾 df -h", pack_cb("tm", "q", "df")),
            ButtonSpec("⏱ uptime", pack_cb("tm", "q", "up")),
        ],
        [
            ButtonSpec("🚪 Выйти из терминала", pack_cb("tm", "exit")),
        ],
    ]
    return "\n".join(lines), rows


# ---------------------------------------------------------------------------
# Camera & Flashlight Panel
# ---------------------------------------------------------------------------

def build_camera_panel(
    torch_on: bool = False,
    notice: str = "",
) -> Tuple[str, KeyboardRows]:
    torch_badge = "ВКЛ" if torch_on else "ВЫКЛ"
    lines = [
        "📷 <b>Камера планшета — Xiaomi Pad 6 (pipa)</b>",
        "",
        "• <b>Задняя:</b> 13 МП (автофокус, зум до 6×, вспышка/фонарик)",
        "• <b>Фронтальная:</b> 8 МП",
        f"• <b>Фонарик:</b> {'🟡 ВКЛ' if torch_on else '⚫ ВЫКЛ'}",
    ]
    if notice:
        lines.extend(["", f"💬 {esc(notice)}"])

    rows: KeyboardRows = [
        [
            ButtonSpec("📸 Задняя (13 МП)", pack_cb("cam", "snap", "back", 0, "off")),
            ButtonSpec("🤳 Фронтальная (8 МП)", pack_cb("cam", "snap", "front", 0, "off")),
        ],
        [
            ButtonSpec("⚡ Задняя + вспышка", pack_cb("cam", "snap", "back", 0, "on")),
        ],
        [
            ButtonSpec("🔍 Задняя 2×", pack_cb("cam", "snap", "back", 35, "off")),
            ButtonSpec("🔍 Задняя 4×", pack_cb("cam", "snap", "back", 70, "off")),
        ],
        [
            ButtonSpec(f"🔦 Фонарик: {torch_badge}", pack_cb("cam", "torch", "toggle")),
        ],
        [
            ButtonSpec("📱 Открыть Камеру на экране", pack_cb("cam", "open")),
        ],
        [
            ButtonSpec("⬅️ Меню", pack_cb("m", "home")),
        ],
    ]
    return "\n".join(lines), rows
