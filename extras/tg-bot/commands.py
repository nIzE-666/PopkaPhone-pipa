"""Canonical Russian command registry for setMyCommands and /help."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List, Tuple


VALID_COMMAND_RE = re.compile(r"^[a-z0-9_]{1,32}$")


@dataclass(frozen=True)
class CommandSpec:
    command: str
    description: str
    usage: str
    details: str


BOT_COMMANDS: Tuple[CommandSpec, ...] = (
    CommandSpec(
        command="start",
        description="Главное меню и панель управления",
        usage="/start",
        details="Открывает интерактивную панель управления планшетом, ПК, телевизорами и сетью.",
    ),
    CommandSpec(
        command="status",
        description="Быстрая сводка: планшет, ПК и телевизоры",
        usage="/status",
        details="Показывает заряд, температуру, память планшета и статус доступности ПК и телевизоров.",
    ),
    CommandSpec(
        command="tablet",
        description="Управление планшетом (экран, яркость, звук, One UI)",
        usage="/tablet",
        details="Яркость, громкость, выключение/включение экрана, перезагрузка и переключение в One UI.",
    ),
    CommandSpec(
        command="screen",
        description="Снять и прислать скриншот планшета",
        usage="/screen",
        details="Мгновенно делает снимок экрана Xiaomi Pad 6 и отправляет его в чат.",
    ),
    CommandSpec(
        command="cam",
        description="Камера планшета (задняя, фронтальная, фонарик)",
        usage="/cam [back|front|torch on|off]",
        details="Снимки с задней (13 МП) и фронтальной (8 МП) камер планшета, зум, вспышка, фонарик и запуск Камеры.",
    ),
    CommandSpec(
        command="vpn",
        description="Выбор и переключение VPN-профиля",
        usage="/vpn",
        details="Показывает доступные VPN-профили (/usr/local/bin/vpn), статус подключения и кнопки переключения.",
    ),
    CommandSpec(
        command="services",
        description="Службы (Waydroid, DeepSeek, Desktop Commander)",
        usage="/services",
        details="Статус и управление системными и пользовательскими службами планшета.",
    ),
    CommandSpec(
        command="pc",
        description="Управление компьютерами (WoL, сон, скриншот, медиа)",
        usage="/pc",
        details="Wake-on-LAN, сон, блокировка, скриншот, громкость, медиа-пульт и перезагрузка ПК.",
    ),
    CommandSpec(
        command="tv",
        description="Пульт телевизоров (Samsung TV и Android TV)",
        usage="/tv",
        details="Интерактивный пульт ДУ для Samsung Smart TV и Android TV, запуск приложений, ввод текста и DLNA Cast.",
    ),
    CommandSpec(
        command="tv_text",
        description="Ввести текст на Samsung TV (русский / английский)",
        usage="/tv_text [текст]",
        details="Отправляет текст в активное поле ввода на Samsung TV (или запрашивает текст следующим сообщением).",
    ),
    CommandSpec(
        command="tv_cast",
        description="Включить медиа-ссылку на Samsung TV (DLNA)",
        usage="/tv_cast [url]",
        details="Отправляет HTTP/HTTPS медиа-ссылку на воспроизведение на Samsung TV через DLNA.",
    ),
    CommandSpec(
        command="clip",
        description="Двусторонний буфер обмена (планшет ↔ ПК)",
        usage="/clip",
        details="Показывает буферы обмена планшета и ПК, переносит текст в обе стороны и позволяет записать новый текст.",
    ),
    CommandSpec(
        command="scenes",
        description="Умные сценарии в одно нажатие (Кино, Работа, Ночь)",
        usage="/scenes",
        details="Запускает макро-сценарии: Кино (ТВ + YouTube/Кинопоиск + яркость 20% + Mute ПК), Работа и Ночь.",
    ),
    CommandSpec(
        command="term",
        description="Интерактивный терминал планшета с сохранением папки",
        usage="/term",
        details="Включает постоянный режим bash-терминала с отслеживанием cd и быстрыми кнопками диагностики.",
    ),
    CommandSpec(
        command="net",
        description="Устройства в домашней сети и статус Wi-Fi",
        usage="/net",
        details="Сканирует локальную сеть, показывает сигнал Wi-Fi планшета, IP, VPN и пинг до интернета.",
    ),
    CommandSpec(
        command="run",
        description="Выполнить разовую команду в терминале планшета",
        usage="/run <команда>",
        details="Запускает разовую shell-команду на планшете (также доступен псевдоним /cmd <команда>) и возвращает вывод.",
    ),
    CommandSpec(
        command="open",
        description="Открыть ссылку на планшете или ПК",
        usage="/open [pc] <url>",
        details="Открывает URL в браузере планшета (или на ПК (Windows), если указать /open pc <url>).",
    ),
    CommandSpec(
        command="get",
        description="Скачать файл с планшета в Telegram",
        usage="/get <путь>",
        details="Отправляет указанный файл с планшета в Telegram (поддерживается ~ и файлы до 48 МБ).",
    ),
    CommandSpec(
        command="alerts",
        description="Настройка авто-уведомлений (заряд, нагрев)",
        usage="/alerts",
        details="Включение и отключение фоновых предупреждений о разряде, перегреве и запуске бота.",
    ),
    CommandSpec(
        command="help",
        description="Подробное описание всех команд",
        usage="/help",
        details="Показывает полную справку по командам и возможностям бота.",
    ),
)


def validate_command_catalog(commands: Tuple[CommandSpec, ...] = BOT_COMMANDS) -> List[str]:
    """Validate Telegram Bot API constraints for every registered command."""
    errors: List[str] = []
    seen = set()
    for spec in commands:
        if not VALID_COMMAND_RE.match(spec.command):
            errors.append(f"Invalid command name: {spec.command!r}")
        if spec.command in seen:
            errors.append(f"Duplicate command: {spec.command!r}")
        seen.add(spec.command)
        if not spec.description or len(spec.description) > 256:
            errors.append(f"Invalid description length for /{spec.command}: {len(spec.description)}")
        if not any("\u0400" <= ch <= "\u04ff" for ch in spec.description):
            errors.append(f"Description for /{spec.command} must be in Russian")
    return errors


def build_help_text() -> str:
    """Render full Russian HTML help message covering every command and feature."""
    lines = [
        "❓ <b>Справка — Центр управления Xiaomi Pad 6</b>",
        "",
        "<b>Доступные команды:</b>",
    ]
    for spec in BOT_COMMANDS:
        usage_escaped = spec.usage.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        lines.append(f"• <code>{usage_escaped}</code> — {spec.details}")

    lines.extend(
        [
            "",
            "<b>Передача файлов и Cast на ТВ:</b>",
            "• Отправьте любой файл, фото, видео или аудио боту — он сохранится в <code>~/Downloads/tg-bot/</code> с кнопкой мгновенного запуска на Samsung TV (DLNA).",
            "• Для скачивания файла с планшета используйте <code>/get /путь/к/файлу</code>.",
            "• Для разовых команд работают <code>/run &lt;команда&gt;</code> и <code>/cmd &lt;команда&gt;</code>; для постоянной сессии — <code>/term</code>.",
            "",
            "<b>Безопасность:</b>",
            "• Бот отвечает исключительно привязанному владельцу.",
            "• Опасные действия (перезагрузка, выключение, переход в One UI, сценарий Ночь) требуют 2-шагового подтверждения.",
        ]
    )
    return "\n".join(lines)
