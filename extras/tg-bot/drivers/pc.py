"""Wake-on-LAN and Windows PC local agent (pipa-pc-agent) client driver."""

from __future__ import annotations

import asyncio
import json
import re
import socket
import urllib.error
import urllib.request
from typing import Any, Dict, Optional, Tuple


MAC_CLEAN_RE = re.compile(r"^[0-9a-fA-F]{12}$")


def normalize_mac(mac: str) -> str:
    """Normalize MAC address to lowercase colon-separated format xx:xx:xx:xx:xx:xx."""
    cleaned = re.sub(r"[^0-9a-fA-F]", "", mac or "")
    if not MAC_CLEAN_RE.match(cleaned):
        raise ValueError(f"Invalid MAC address: {mac!r}")
    cleaned = cleaned.lower()
    return ":".join(cleaned[i : i + 2] for i in range(0, 12, 2))


def build_wol_packet(mac: str) -> bytes:
    """Construct standard 102-byte Wake-on-LAN magic packet (6x 0xFF + 16x MAC)."""
    norm = normalize_mac(mac)
    mac_bytes = bytes.fromhex(norm.replace(":", ""))
    return b"\xff" * 6 + mac_bytes * 16


def send_wol(mac: str, broadcast: str = "192.168.1.255", port: int = 9) -> bytes:
    """Send a Wake-on-LAN UDP broadcast magic packet."""
    packet = build_wol_packet(mac)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.sendto(packet, (broadcast, int(port)))
    return packet


class PCDriver:
    """Async HTTP client for pipa-pc-agent with Wake-on-LAN support."""

    def __init__(
        self,
        host: str,
        port: int = 18765,
        token: str = "",
        mac: str = "",
        broadcast: str = "192.168.1.255",
    ) -> None:
        self.host = host
        self.port = int(port)
        self.token = token
        self.mac = mac
        self.broadcast = broadcast

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def _headers(self, extra: Optional[Dict[str, str]] = None) -> Dict[str, str]:
        hdrs: Dict[str, str] = {}
        if self.token:
            hdrs["X-Pipa-Token"] = self.token
        if extra:
            hdrs.update(extra)
        return hdrs

    async def _request(
        self,
        method: str,
        path: str,
        json_body: Optional[Dict[str, Any]] = None,
        timeout: float = 8.0,
    ) -> Tuple[int, bytes, str]:
        """Perform HTTP request using aiohttp when available, else stdlib urllib."""
        url = f"{self.base_url}{path}"
        headers = self._headers({"Content-Type": "application/json"} if json_body is not None else None)
        body_bytes = json.dumps(json_body).encode("utf-8") if json_body is not None else None

        try:
            import aiohttp

            client_timeout = aiohttp.ClientTimeout(total=timeout)
            async with aiohttp.ClientSession(timeout=client_timeout) as session:
                async with session.request(method, url, headers=headers, data=body_bytes) as resp:
                    raw = await resp.read()
                    return resp.status, raw, resp.headers.get("Content-Type", "")
        except ImportError:
            pass
        except Exception as exc:
            return 0, str(exc).encode("utf-8"), ""

        def _sync_req() -> Tuple[int, bytes, str]:
            req = urllib.request.Request(url, data=body_bytes, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    return resp.status, resp.read(), resp.headers.get("Content-Type", "")
            except urllib.error.HTTPError as err:
                return err.code, err.read(), err.headers.get("Content-Type", "")
            except Exception as exc:
                return 0, str(exc).encode("utf-8"), ""

        return await asyncio.to_thread(_sync_req)

    async def wake(self) -> Tuple[bool, str]:
        if not self.mac:
            return False, "MAC-адрес ПК не настроен."
        try:
            await asyncio.to_thread(send_wol, self.mac, self.broadcast)
            return True, f"Wake-on-LAN пакет отправлен на {normalize_mac(self.mac)} ({self.broadcast})."
        except Exception as exc:
            return False, f"Ошибка отправки WoL: {exc}"

    async def is_online(self, timeout: float = 2.0) -> bool:
        status, _, _ = await self._request("GET", "/health", timeout=timeout)
        return status == 200

    async def get_status(self, timeout: float = 5.0) -> Tuple[bool, Optional[Dict[str, Any]], str]:
        code, raw, _ = await self._request("GET", "/status", timeout=timeout)
        if code == 200:
            try:
                return True, json.loads(raw.decode("utf-8")), ""
            except json.JSONDecodeError:
                return False, None, "Некорректный JSON от агента ПК."
        if code == 401:
            return False, None, "Ошибка авторизации (проверьте PC_AGENT_TOKEN)."
        if code == 403:
            return False, None, "Доступ запрещён агентом ПК (IP вне разрешённой локальной сети)."
        if code == 0:
            return False, None, "ПК не в сети или агент pipa-pc-agent не запущен."
        return False, None, f"Ошибка агента ПК: HTTP {code}"

    async def take_screenshot(self, timeout: float = 15.0) -> Tuple[Optional[bytes], str]:
        code, raw, _ = await self._request("GET", "/screenshot", timeout=timeout)
        if code == 200 and raw.startswith(b"\x89PNG\r\n\x1a\n"):
            return raw, f"Скриншот ПК ({self.host})"
        if code == 401:
            return None, "Ошибка авторизации ПК-агента (PC_AGENT_TOKEN)."
        if code == 0:
            return None, "ПК не в сети или агент не отвечает."
        try:
            detail = json.loads(raw.decode("utf-8")).get("error", f"HTTP {code}")
        except Exception:
            detail = f"HTTP {code}"
        return None, f"Не удалось снять скриншот ПК: {detail}"

    async def action(self, action_name: str, **params: Any) -> Tuple[bool, str, Optional[Dict[str, Any]]]:
        payload: Dict[str, Any] = {"action": action_name}
        payload.update(params)
        timeout = 30.0 if action_name == "run_cmd" else 10.0
        code, raw, _ = await self._request("POST", "/action", json_body=payload, timeout=timeout)
        if code == 200:
            try:
                data = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                data = {"ok": True}
            msg = str(data.get("message") or f"Команда {action_name} выполнена на ПК.")
            return bool(data.get("ok", True)), msg, data
        if code == 401:
            return False, "Ошибка авторизации ПК-агента (проверьте PC_AGENT_TOKEN).", None
        if code == 0:
            return False, "ПК не в сети или pipa-pc-agent не запущен.", None
        try:
            detail = json.loads(raw.decode("utf-8")).get("error", f"HTTP {code}")
        except Exception:
            detail = raw.decode("utf-8", errors="replace")[:160] or f"HTTP {code}"
        return False, f"Ошибка ПК ({action_name}): {detail}", None

    async def get_clipboard(self, timeout: float = 6.0) -> Tuple[bool, str, str]:
        """Read UTF-8 text from Windows PC clipboard via GET /clipboard."""
        code, raw, _ = await self._request("GET", "/clipboard", timeout=timeout)
        if code == 200:
            try:
                data = json.loads(raw.decode("utf-8"))
                return True, str(data.get("text", "")), ""
            except json.JSONDecodeError:
                return False, "", "Некорректный JSON от агента ПК."
        if code == 401:
            return False, "", "Ошибка авторизации ПК-агента (проверьте PC_AGENT_TOKEN)."
        if code == 403:
            return False, "", "Доступ запрещён агентом ПК (IP вне разрешённой LAN)."
        if code == 0:
            return False, "", "ПК не в сети или pipa-pc-agent не запущен."
        try:
            detail = json.loads(raw.decode("utf-8")).get("error", f"HTTP {code}")
        except Exception:
            detail = f"HTTP {code}"
        return False, "", f"Ошибка чтения буфера ПК: {detail}"

    async def set_clipboard(self, text: str, timeout: float = 6.0) -> Tuple[bool, str]:
        """Write UTF-8 text to Windows PC clipboard via POST /clipboard."""
        code, raw, _ = await self._request("POST", "/clipboard", json_body={"text": text}, timeout=timeout)
        if code == 200:
            try:
                data = json.loads(raw.decode("utf-8"))
                length = int(data.get("length", len(text)))
            except Exception:
                length = len(text)
            return True, f"Скопировано в буфер ПК ({length} симв.)."
        if code == 401:
            return False, "Ошибка авторизации ПК-агента (проверьте PC_AGENT_TOKEN)."
        if code == 403:
            return False, "Доступ запрещён агентом ПК (IP вне разрешённой LAN)."
        if code == 0:
            return False, "ПК не в сети или pipa-pc-agent не запущен."
        try:
            detail = json.loads(raw.decode("utf-8")).get("error", f"HTTP {code}")
        except Exception:
            detail = raw.decode("utf-8", errors="replace")[:160] or f"HTTP {code}"
        return False, f"Ошибка записи буфера ПК: {detail}"
