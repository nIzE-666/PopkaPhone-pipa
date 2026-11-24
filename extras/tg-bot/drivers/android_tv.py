"""Android TV / Chromecast ADB-over-Wi-Fi control driver."""

from __future__ import annotations

import asyncio
from typing import Dict, List, Optional, Tuple


ANDROID_KEYEVENTS: Dict[str, int] = {
    "up": 19,
    "down": 20,
    "left": 21,
    "right": 22,
    "ok": 23,
    "enter": 23,
    "back": 4,
    "home": 3,
    "menu": 82,
    "vol_up": 24,
    "vol_down": 25,
    "mute": 164,
    "play_pause": 85,
    "next": 87,
    "prev": 88,
    "power": 26,
    "wakeup": 224,
    "sleep": 223,
}

ANDROID_TV_APPS: Dict[str, str] = {
    "youtube": "com.google.android.youtube.tv",
    "kinopoisk": "ru.kinopoisk.tv",
    "twitch": "tv.twitch.android.app",
    "settings": "com.android.tv.settings",
    "smarttube": "com.teamsmart.videomanager.tv",
}

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
_ADB_SHELL_SPECIAL = set("\\\"'`$&|;<>()?*[]~#!")


def adb_target(host: str, port: int = 5555) -> str:
    return f"{host}:{int(port)}"


def escape_adb_text(text: str) -> str:
    """Escape text safely for `adb shell input text <arg>`."""
    out: List[str] = []
    for ch in text:
        if ch == " ":
            out.append("%s")
        elif ch in ("\r", "\n", "\t"):
            out.append("%s")
        elif ch in _ADB_SHELL_SPECIAL:
            out.append("\\" + ch)
        else:
            out.append(ch)
    return "".join(out)


def build_connect_cmd(host: str, port: int = 5555) -> List[str]:
    return ["adb", "connect", adb_target(host, port)]


def build_keyevent_cmd(host: str, key: str, port: int = 5555) -> List[str]:
    if isinstance(key, int):
        code = key
    elif str(key).isdigit():
        code = int(key)
    elif key in ANDROID_KEYEVENTS:
        code = ANDROID_KEYEVENTS[key]
    else:
        raise ValueError(f"Unsupported Android TV key: {key!r}")
    return ["adb", "-s", adb_target(host, port), "shell", "input", "keyevent", str(code)]


def build_text_cmd(host: str, text: str, port: int = 5555) -> List[str]:
    escaped = escape_adb_text(text)
    if not escaped:
        raise ValueError("Text for Android TV input cannot be empty")
    return ["adb", "-s", adb_target(host, port), "shell", "input", "text", escaped]


def build_app_launch_cmd(host: str, app: str, port: int = 5555) -> List[str]:
    pkg = ANDROID_TV_APPS.get(app.lower(), app)
    if not pkg or any(c.isspace() or c in ";|&$`" for c in pkg):
        raise ValueError(f"Invalid Android package name: {app!r}")
    return [
        "adb",
        "-s",
        adb_target(host, port),
        "shell",
        "monkey",
        "-p",
        pkg,
        "-c",
        "android.intent.category.LAUNCHER",
        "1",
    ]


def build_open_url_cmd(host: str, url: str, port: int = 5555) -> List[str]:
    cleaned = url.strip()
    if not cleaned.startswith(("http://", "https://")):
        raise ValueError("URL must start with http:// or https://")
    return [
        "adb",
        "-s",
        adb_target(host, port),
        "shell",
        "am",
        "start",
        "-a",
        "android.intent.action.VIEW",
        "-d",
        cleaned,
    ]


def build_screencap_cmd(host: str, port: int = 5555) -> List[str]:
    return ["adb", "-s", adb_target(host, port), "exec-out", "screencap", "-p"]


class AndroidTVDriver:
    """Async Android TV controller via ADB over Wi-Fi."""

    def __init__(
        self,
        host: str = "192.168.1.160",
        adb_port: int = 5555,
        cast_port: int = 8008,
    ) -> None:
        self.host = host
        self.adb_port = adb_port
        self.cast_port = cast_port

    async def _run_adb(self, args: List[str], timeout: float = 10.0) -> Tuple[int, bytes, str]:
        try:
            proc = await asyncio.create_subprocess_exec(
                *args,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.communicate()
                return 124, b"", "Таймаут ADB"
            return (
                proc.returncode if proc.returncode is not None else 1,
                out,
                err.decode("utf-8", errors="replace").strip(),
            )
        except FileNotFoundError:
            return 127, b"", "Утилита adb не установлена на планшете (apt install android-tools-adb)"
        except OSError as exc:
            return 1, b"", str(exc)

    async def is_online(self, timeout: float = 1.5) -> bool:
        for port in (self.adb_port, self.cast_port):
            try:
                _, writer = await asyncio.wait_for(asyncio.open_connection(self.host, port), timeout=timeout)
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
                return True
            except (asyncio.TimeoutError, OSError):
                continue
        return False

    async def ensure_connected(self) -> Tuple[bool, str]:
        rc, out, err = await self._run_adb(build_connect_cmd(self.host, self.adb_port), timeout=6.0)
        text = out.decode("utf-8", errors="replace").strip()
        if rc == 0 and ("connected" in text.lower() or "already" in text.lower()):
            return True, text
        return False, err or text or f"ADB connect код {rc}"

    async def send_key(self, key: str) -> Tuple[bool, str]:
        try:
            cmd = build_keyevent_cmd(self.host, key, self.adb_port)
        except ValueError as exc:
            return False, str(exc)
        ok_conn, conn_msg = await self.ensure_connected()
        if not ok_conn:
            return False, f"Нет ADB-подключения к Android TV: {conn_msg}"
        rc, _, err = await self._run_adb(cmd, timeout=8.0)
        if rc == 0:
            return True, f"Android TV: нажата кнопка {key}."
        return False, f"Ошибка ADB keyevent: {err or rc}"

    async def send_text(self, text: str) -> Tuple[bool, str]:
        try:
            cmd = build_text_cmd(self.host, text, self.adb_port)
        except ValueError as exc:
            return False, str(exc)
        ok_conn, conn_msg = await self.ensure_connected()
        if not ok_conn:
            return False, f"Нет ADB-подключения к Android TV: {conn_msg}"
        rc, _, err = await self._run_adb(cmd, timeout=10.0)
        if rc == 0:
            return True, f"Текст отправлен на Android TV: {text[:40]}"
        return False, f"Ошибка ввода текста ADB: {err or rc}"

    async def launch_app(self, app: str) -> Tuple[bool, str]:
        try:
            cmd = build_app_launch_cmd(self.host, app, self.adb_port)
        except ValueError as exc:
            return False, str(exc)
        ok_conn, conn_msg = await self.ensure_connected()
        if not ok_conn:
            return False, f"Нет ADB-подключения к Android TV: {conn_msg}"
        rc, out, err = await self._run_adb(cmd, timeout=12.0)
        if rc == 0:
            return True, f"Android TV: запуск {app}."
        detail = err or out.decode("utf-8", errors="replace").strip()
        return False, f"Не удалось запустить {app}: {detail[:180]}"

    async def open_url(self, url: str) -> Tuple[bool, str]:
        try:
            cmd = build_open_url_cmd(self.host, url, self.adb_port)
        except ValueError as exc:
            return False, str(exc)
        ok_conn, conn_msg = await self.ensure_connected()
        if not ok_conn:
            return False, f"Нет ADB-подключения к Android TV: {conn_msg}"
        rc, _, err = await self._run_adb(cmd, timeout=10.0)
        if rc == 0:
            return True, f"Ссылка открыта на Android TV: {url}"
        return False, f"Ошибка открытия URL на Android TV: {err or rc}"

    async def take_screenshot(self) -> Tuple[Optional[bytes], str]:
        ok_conn, conn_msg = await self.ensure_connected()
        if not ok_conn:
            return None, f"Нет ADB-подключения к Android TV: {conn_msg}"
        rc, data, err = await self._run_adb(build_screencap_cmd(self.host, self.adb_port), timeout=15.0)
        if rc == 0 and data.startswith(PNG_MAGIC):
            return data, f"Скриншот Android TV ({self.host})"
        return None, f"Не удалось снять скриншот Android TV: {err or f'код {rc}'}"
