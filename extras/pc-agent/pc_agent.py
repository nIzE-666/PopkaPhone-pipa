#!/usr/bin/env python3
"""Lightweight zero-dependency local HTTP control agent for Windows PC (mypc).

Runs using Python 3 standard library only (`py -3 pc_agent.py`).
Binds to 0.0.0.0:18765, enforces LAN/RFC1918 source IP filtering and
constant-time `X-Pipa-Token` shared-secret authentication.
"""

from __future__ import annotations

import argparse
import ctypes
import hmac
import ipaddress
import json
import logging
import os
import platform
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


VERSION = "1.1.0"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 18765
MAX_BODY_BYTES = 262144
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

ALLOWED_LAN_NETWORKS = (
    ipaddress.ip_network("127.0.0.0/8"),
    ipaddress.ip_network("10.0.0.0/8"),
    ipaddress.ip_network("172.16.0.0/12"),
    ipaddress.ip_network("192.168.0.0/16"),
    ipaddress.ip_network("::1/128"),
)

VK_CODES: Dict[str, int] = {
    "mute": 0xAD,
    "vol_mute": 0xAD,
    "vol_down": 0xAE,
    "vol_up": 0xAF,
    "media_next": 0xB0,
    "media_prev": 0xB1,
    "media_play_pause": 0xB3,
}

SUPPORTED_ACTIONS = frozenset(
    {
        "lock",
        "sleep",
        "hibernate",
        "reboot",
        "shutdown",
        "vol_up",
        "vol_down",
        "vol_mute",
        "mute",
        "media_play_pause",
        "media_next",
        "media_prev",
        "open_url",
        "run_cmd",
    }
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] pipa-pc-agent: %(message)s",
)
logger = logging.getLogger("pipa-pc-agent")


def default_token_path() -> Path:
    return Path.home() / ".pipa-pc-agent" / "token.txt"


def ensure_token(token_path: Optional[Path] = None, env_token: Optional[str] = None) -> Tuple[str, Path, bool]:
    """Load or auto-generate 32-byte hex shared secret token."""
    override = env_token if env_token is not None else os.environ.get("PIPA_PC_AGENT_TOKEN", "")
    path = token_path or default_token_path()
    if override.strip():
        return override.strip(), path, False

    if path.is_file():
        existing = path.read_text(encoding="utf-8").strip()
        if existing:
            return existing, path, False

    path.parent.mkdir(parents=True, exist_ok=True)
    generated = secrets.token_hex(32)
    path.write_text(generated + "\n", encoding="utf-8")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return generated, path, True


def is_allowed_client_ip(ip_str: str) -> bool:
    """Defense-in-depth check: allow strictly loopback and RFC1918 private LAN IPs."""
    if not ip_str:
        return False
    try:
        addr = ipaddress.ip_address(ip_str.strip())
    except ValueError:
        return False
    if isinstance(addr, ipaddress.IPv6Address) and addr.ipv4_mapped is not None:
        addr = addr.ipv4_mapped
    return any(addr in net for net in ALLOWED_LAN_NETWORKS)


# ---------------------------------------------------------------------------
# Native Windows System Executor (zero external pip dependencies)
# ---------------------------------------------------------------------------

class _MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.c_ulong),
        ("dwMemoryLoad", ctypes.c_ulong),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


class _SYSTEM_POWER_STATUS(ctypes.Structure):
    _fields_ = [
        ("ACLineStatus", ctypes.c_byte),
        ("BatteryFlag", ctypes.c_byte),
        ("BatteryLifePercent", ctypes.c_byte),
        ("SystemStatusFlag", ctypes.c_byte),
        ("BatteryLifeTime", ctypes.c_ulong),
        ("BatteryFullLifeTime", ctypes.c_ulong),
    ]


class _FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", ctypes.c_uint32), ("dwHighDateTime", ctypes.c_uint32)]


def _filetime_to_int(ft: _FILETIME) -> int:
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


class WindowsSystemExecutor:
    """Executes telemetry, screenshot, and control actions on Windows via ctypes & PowerShell."""

    def __init__(self) -> None:
        self._cpu_lock = threading.Lock()
        self._prev_times: Optional[Tuple[int, int, int]] = None

    def _sample_cpu_pct(self) -> float:
        if os.name != "nt":
            return 0.0
        kernel32 = ctypes.windll.kernel32
        idle, kernel, user = _FILETIME(), _FILETIME(), _FILETIME()
        if not kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            return 0.0
        cur = (_filetime_to_int(idle), _filetime_to_int(kernel), _filetime_to_int(user))
        with self._cpu_lock:
            prev = self._prev_times
            self._prev_times = cur
        if prev is None:
            time.sleep(0.12)
            idle2, kernel2, user2 = _FILETIME(), _FILETIME(), _FILETIME()
            if not kernel32.GetSystemTimes(ctypes.byref(idle2), ctypes.byref(kernel2), ctypes.byref(user2)):
                return 0.0
            prev = cur
            cur = (_filetime_to_int(idle2), _filetime_to_int(kernel2), _filetime_to_int(user2))
            with self._cpu_lock:
                self._prev_times = cur
        d_idle = cur[0] - prev[0]
        d_total = (cur[1] - prev[1]) + (cur[2] - prev[2])
        if d_total <= 0:
            return 0.0
        return round(max(0.0, min(100.0, (1.0 - (d_idle / d_total)) * 100.0)), 1)

    def _read_memory(self) -> Tuple[int, int, float]:
        if os.name != "nt":
            return 0, 0, 0.0
        stat = _MEMORYSTATUSEX()
        stat.dwLength = ctypes.sizeof(_MEMORYSTATUSEX)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
            return 0, 0, 0.0
        total_mb = round(stat.ullTotalPhys / (1024 * 1024))
        avail_mb = round(stat.ullAvailPhys / (1024 * 1024))
        used_mb = max(0, total_mb - avail_mb)
        pct = round((used_mb / total_mb) * 100.0, 1) if total_mb else 0.0
        return used_mb, total_mb, pct

    def _read_uptime_sec(self) -> int:
        if os.name != "nt":
            return 0
        get_tick = ctypes.windll.kernel32.GetTickCount64
        get_tick.restype = ctypes.c_uint64
        return int(get_tick() // 1000)

    def _read_battery(self) -> Optional[Dict[str, Any]]:
        if os.name != "nt":
            return None
        sps = _SYSTEM_POWER_STATUS()
        if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(sps)):
            return None
        # BatteryFlag 128 = no system battery; BatteryLifePercent 255 = unknown
        if (sps.BatteryFlag & 0x80) or (sps.BatteryLifePercent & 0xFF) == 255:
            return None
        return {
            "percent": int(sps.BatteryLifePercent & 0xFF),
            "ac_online": (sps.ACLineStatus & 0xFF) == 1,
        }

    def _read_active_window(self) -> str:
        if os.name != "nt":
            return ""
        user32 = ctypes.windll.user32
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            return ""
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return ""
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        return buf.value.strip()

    def get_status(self) -> Dict[str, Any]:
        used_mb, total_mb, ram_pct = self._read_memory()
        return {
            "ok": True,
            "hostname": socket.gethostname(),
            "os": f"{platform.system()} {platform.release()}",
            "uptime_sec": self._read_uptime_sec(),
            "cpu_pct": self._sample_cpu_pct(),
            "ram_used_mb": used_mb,
            "ram_total_mb": total_mb,
            "ram_pct": ram_pct,
            "battery": self._read_battery(),
            "active_window": self._read_active_window(),
        }

    def take_screenshot(self) -> bytes:
        if os.name != "nt":
            raise RuntimeError("Screenshot capture is supported on Windows only")
        fd, tmp_path = tempfile.mkstemp(prefix="mypc-shot-", suffix=".png")
        os.close(fd)
        ps_script = (
            "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; "
            "$sig = '[DllImport(\"user32.dll\")] public static extern bool SetProcessDPIAware();'; "
            "try { (Add-Type -MemberDefinition $sig -Name 'PipaDpi' -Namespace 'Win32' -PassThru)::SetProcessDPIAware() | Out-Null } catch {}; "
            "$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "
            "$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height); "
            "$g = [System.Drawing.Graphics]::FromImage($bmp); "
            "$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size); "
            f"$bmp.Save('{tmp_path.replace(chr(39), chr(39) * 2)}', [System.Drawing.Imaging.ImageFormat]::Png); "
            "$g.Dispose(); $bmp.Dispose();"
        )
        try:
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            res = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", ps_script],
                capture_output=True,
                timeout=12,
                creationflags=creationflags,
                check=False,
            )
            if res.returncode != 0:
                err_txt = res.stderr.decode("utf-8", errors="replace").strip()
                raise RuntimeError(err_txt or f"PowerShell exited with {res.returncode}")
            data = Path(tmp_path).read_bytes()
            if not data.startswith(PNG_MAGIC):
                raise RuntimeError("Captured output is not a valid PNG image")
            return data
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    def get_clipboard(self) -> str:
        """Read current Windows clipboard UTF-8 text via PowerShell (empty string if non-text)."""
        if os.name != "nt":
            raise RuntimeError("Clipboard access is supported on Windows only")
        ps_script = (
            "$t = try { Get-Clipboard -Raw -Format Text -ErrorAction Stop } catch { $null }; "
            "if ($null -ne $t) { "
            "$b = [System.Text.Encoding]::UTF8.GetBytes([string]$t); "
            "$s = [Console]::OpenStandardOutput(); "
            "$s.Write($b, 0, $b.Length); "
            "$s.Flush() }"
        )
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        res = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-STA", "-WindowStyle", "Hidden", "-Command", ps_script],
            capture_output=True,
            timeout=10,
            creationflags=creationflags,
            check=False,
        )
        if res.returncode != 0:
            err_txt = res.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(err_txt or f"PowerShell exited with {res.returncode}")
        return res.stdout.decode("utf-8", errors="replace")

    def set_clipboard(self, text: str) -> int:
        """Set Windows clipboard UTF-8 text via stdin to Set-Clipboard (injection-safe)."""
        if os.name != "nt":
            raise RuntimeError("Clipboard access is supported on Windows only")
        if not isinstance(text, str):
            raise ValueError("Clipboard 'text' must be a string")
        ps_script = (
            "$r = New-Object System.IO.StreamReader([Console]::OpenStandardInput(), [System.Text.Encoding]::UTF8); "
            "$t = $r.ReadToEnd(); "
            "if ($t.Length -eq 0) { Set-Clipboard -Value $null } else { Set-Clipboard -Value $t }"
        )
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        res = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-STA", "-WindowStyle", "Hidden", "-Command", ps_script],
            input=text.encode("utf-8"),
            capture_output=True,
            timeout=10,
            creationflags=creationflags,
            check=False,
        )
        if res.returncode != 0:
            err_txt = res.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(err_txt or f"PowerShell exited with {res.returncode}")
        return len(text)

    def _press_vk(self, vk_code: int) -> None:
        if os.name != "nt":
            raise RuntimeError("Virtual key press requires Windows")
        user32 = ctypes.windll.user32
        KEYEVENTF_KEYUP = 0x0002
        user32.keybd_event(vk_code, 0, 0, 0)
        time.sleep(0.02)
        user32.keybd_event(vk_code, 0, KEYEVENTF_KEYUP, 0)

    def perform_action(self, action: str, params: Dict[str, Any]) -> Dict[str, Any]:
        if action not in SUPPORTED_ACTIONS:
            raise ValueError(f"Unsupported action: {action}")

        if action in VK_CODES:
            self._press_vk(VK_CODES[action])
            labels = {
                "vol_up": "Громкость ПК увеличена.",
                "vol_down": "Громкость ПК уменьшена.",
                "vol_mute": "Звук ПК переключён (Mute).",
                "mute": "Звук ПК переключён (Mute).",
                "media_play_pause": "Медиа: Пауза / Воспроизведение.",
                "media_next": "Медиа: Следующий трек.",
                "media_prev": "Медиа: Предыдущий трек.",
            }
            return {"ok": True, "action": action, "message": labels[action]}

        if action == "lock":
            if os.name != "nt":
                raise RuntimeError("Lock requires Windows")
            ctypes.windll.user32.LockWorkStation()
            return {"ok": True, "action": action, "message": "Экран ПК заблокирован."}

        if action == "sleep":
            if os.name != "nt":
                raise RuntimeError("Sleep requires Windows")
            # Schedule briefly in background thread so HTTP response arrives cleanly before suspend
            def _do_sleep() -> None:
                time.sleep(0.4)
                ctypes.windll.powrprof.SetSuspendState(0, 1, 0)

            threading.Thread(target=_do_sleep, daemon=True).start()
            return {"ok": True, "action": action, "message": "ПК переходит в спящий режим."}

        if action == "hibernate":
            subprocess.Popen(["shutdown", "/h"], creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            return {"ok": True, "action": action, "message": "ПК переходит в гибернацию."}

        if action == "reboot":
            subprocess.Popen(["shutdown", "/r", "/t", "3"], creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            return {"ok": True, "action": action, "message": "Перезагрузка ПК через 3 секунды."}

        if action == "shutdown":
            subprocess.Popen(["shutdown", "/s", "/t", "3"], creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            return {"ok": True, "action": action, "message": "Выключение ПК через 3 секунды."}

        if action == "open_url":
            url = str(params.get("url") or "").strip()
            if not url.startswith(("http://", "https://")):
                raise ValueError("URL must start with http:// or https://")
            webbrowser.open(url, new=2)
            return {"ok": True, "action": action, "message": f"Ссылка открыта в браузере ПК: {url}"}

        if action == "run_cmd":
            cmd = str(params.get("cmd") or "").strip()
            if not cmd:
                raise ValueError("Missing 'cmd' parameter")
            timeout = max(1, min(60, int(params.get("timeout", 20))))
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            wrapped_cmd = "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; " + cmd
            res = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", wrapped_cmd],
                capture_output=True,
                timeout=timeout,
                creationflags=creationflags,
                check=False,
            )
            stdout_txt = res.stdout.decode("utf-8", errors="replace")
            stderr_txt = res.stderr.decode("utf-8", errors="replace")
            return {
                "ok": res.returncode == 0,
                "action": action,
                "exit_code": res.returncode,
                "stdout": stdout_txt[:8192],
                "stderr": stderr_txt[:4096],
                "message": f"Команда завершена с кодом {res.returncode}.",
            }

        raise ValueError(f"Unsupported action: {action}")


# ---------------------------------------------------------------------------
# HTTP Server & Security Guards
# ---------------------------------------------------------------------------

class MyPCHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True

    def __init__(
        self,
        server_address: Tuple[str, int],
        token: str,
        executor: Any,
        ip_validator: Any = is_allowed_client_ip,
    ) -> None:
        self.expected_token = token
        self.executor = executor
        self.ip_validator = ip_validator
        super().__init__(server_address, MyPCRequestHandler)


class MyPCRequestHandler(BaseHTTPRequestHandler):
    server: MyPCHTTPServer

    def log_message(self, fmt: str, *args: Any) -> None:
        logger.info("%s - %s", self.client_address[0], fmt % args)

    def _send_json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, status: int, content_type: str, data: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _authorize(self) -> bool:
        client_ip = self.client_address[0]
        if not self.server.ip_validator(client_ip):
            logger.warning("Rejected non-LAN client IP: %s", client_ip)
            self._send_json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "Forbidden: client IP outside LAN"})
            return False

        provided = (self.headers.get("X-Pipa-Token") or "").strip()
        if not provided or not hmac.compare_digest(provided, self.server.expected_token):
            logger.warning("Unauthorized request from %s (invalid X-Pipa-Token)", client_ip)
            self._send_json(HTTPStatus.UNAUTHORIZED, {"ok": False, "error": "Unauthorized: invalid X-Pipa-Token"})
            return False

        return True

    def do_GET(self) -> None:
        if not self._authorize():
            return
        path = self.path.split("?", 1)[0]

        if path == "/health":
            self._send_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "service": "pipa-pc-agent",
                    "version": VERSION,
                    "hostname": socket.gethostname(),
                },
            )
            return

        if path == "/status":
            try:
                data = self.server.executor.get_status()
                self._send_json(HTTPStatus.OK, data)
            except Exception as exc:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(exc)})
            return

        if path == "/screenshot":
            try:
                png = self.server.executor.take_screenshot()
                self._send_bytes(HTTPStatus.OK, "image/png", png)
            except Exception as exc:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(exc)})
            return

        if path == "/clipboard":
            try:
                text = self.server.executor.get_clipboard()
                self._send_json(HTTPStatus.OK, {"ok": True, "text": text})
            except Exception as exc:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(exc)})
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": f"Unknown endpoint: {path}"})

    def _read_json_body(self) -> Optional[Dict[str, Any]]:
        try:
            content_len = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Invalid Content-Length"})
            return None

        if content_len <= 0 or content_len > MAX_BODY_BYTES:
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Invalid or oversized JSON body"})
            return None

        raw = self.rfile.read(content_len)
        try:
            body = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Malformed JSON payload"})
            return None

        if not isinstance(body, dict):
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "JSON body must be an object"})
            return None
        return body

    def do_POST(self) -> None:
        if not self._authorize():
            return
        path = self.path.split("?", 1)[0]
        if path not in ("/action", "/clipboard"):
            self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": f"Unknown endpoint: {path}"})
            return

        body = self._read_json_body()
        if body is None:
            return

        if path == "/clipboard":
            if "text" not in body or not isinstance(body["text"], str):
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Missing or invalid 'text' field"})
                return
            text_val = body["text"]
            try:
                length = self.server.executor.set_clipboard(text_val)
                self._send_json(HTTPStatus.OK, {"ok": True, "length": int(length) if length is not None else len(text_val)})
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
            except subprocess.TimeoutExpired:
                self._send_json(HTTPStatus.GATEWAY_TIMEOUT, {"ok": False, "error": "Clipboard operation timed out"})
            except Exception as exc:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(exc)})
            return

        if not body.get("action"):
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "Missing 'action' field"})
            return

        action = str(body["action"]).strip()
        try:
            result = self.server.executor.perform_action(action, body)
            self._send_json(HTTPStatus.OK, result)
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
        except subprocess.TimeoutExpired:
            self._send_json(HTTPStatus.GATEWAY_TIMEOUT, {"ok": False, "error": "Command timed out"})
        except Exception as exc:
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(exc)})


def create_server(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    token: Optional[str] = None,
    executor: Optional[Any] = None,
    ip_validator: Any = is_allowed_client_ip,
) -> MyPCHTTPServer:
    if token is None:
        token, _, _ = ensure_token()
    exec_impl = executor if executor is not None else WindowsSystemExecutor()
    return MyPCHTTPServer((host, int(port)), token=token, executor=exec_impl, ip_validator=ip_validator)


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="pipa-pc-agent — local Windows control agent for pipa-tg-bot")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Bind address (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"Listen port (default: {DEFAULT_PORT})")
    parser.add_argument("--token-file", type=Path, default=None, help="Path to shared token file")
    parser.add_argument("--print-token", action="store_true", help="Print token and exit")
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> None:
    args = parse_args(argv)
    token, token_path, created = ensure_token(args.token_file)
    if args.print_token:
        print(token)
        return

    if created:
        logger.info("Generated new X-Pipa-Token at %s", token_path)
    else:
        logger.info("Loaded X-Pipa-Token from %s", token_path)

    server = create_server(host=args.host, port=args.port, token=token)
    logger.info("Listening on http://%s:%d (LAN-only + X-Pipa-Token)", args.host, args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("Shutting down pipa-pc-agent.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
