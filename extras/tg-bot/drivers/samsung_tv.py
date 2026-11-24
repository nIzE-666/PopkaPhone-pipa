"""Samsung Tizen Smart TV WebSocket Remote & DLNA MediaRenderer driver."""

from __future__ import annotations

import asyncio
import base64
import json
import mimetypes
import ssl
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, Optional, Sequence, Tuple
from urllib.parse import quote, urlparse
from xml.sax.saxutils import escape as xml_escape

try:
    from .pc import send_wol
except ImportError:
    from pc import send_wol


DEFAULT_DLNA_PORT = 9197
AVTRANSPORT_CONTROL_PATH = "/upnp/control/AVTransport1"
AVTRANSPORT_SERVICE_TYPE = "urn:schemas-upnp-org:service:AVTransport:1"
RENDERING_CONTROL_PATH = "/upnp/control/RenderingControl1"
RENDERING_SERVICE_TYPE = "urn:schemas-upnp-org:service:RenderingControl:1"


SAMSUNG_KEY_MAP: Dict[str, str] = {
    "up": "KEY_UP",
    "down": "KEY_DOWN",
    "left": "KEY_LEFT",
    "right": "KEY_RIGHT",
    "enter": "KEY_ENTER",
    "ok": "KEY_ENTER",
    "back": "KEY_RETURN",
    "home": "KEY_HOME",
    "menu": "KEY_MENU",
    "vol_up": "KEY_VOLUP",
    "vol_down": "KEY_VOLDOWN",
    "mute": "KEY_MUTE",
    "play": "KEY_PLAY",
    "pause": "KEY_PAUSE",
    "play_pause": "KEY_PLAY_BACK",
    "power": "KEY_POWER",
    "source": "KEY_SOURCE",
    "ch_up": "KEY_CHUP",
    "ch_down": "KEY_CHDOWN",
}

SAMSUNG_APP_IDS: Dict[str, str] = {
    "youtube": "111299001912",
    "netflix": "3201907018807",
    "kinopoisk": "3201806016390",
    "twitch": "3201606009684",
    "browser": "org.tizen.browser",
}


def encode_app_name(app_name: str) -> str:
    return base64.b64encode(app_name.encode("utf-8")).decode("ascii")


def build_ws_url(
    host: str,
    port: int = 8002,
    app_name: str = "PipaBot",
    token: Optional[str] = None,
) -> str:
    """Build Samsung Smart TV Tizen Remote WebSocket URL."""
    scheme = "wss" if int(port) == 8002 else "ws"
    b64_name = quote(encode_app_name(app_name), safe="")
    url = f"{scheme}://{host}:{int(port)}/api/v2/channels/samsung.remote.control?name={b64_name}"
    if token:
        url += f"&token={quote(token.strip(), safe='')}"
    return url


def build_key_payload(key: str, cmd: str = "Click") -> Dict[str, Any]:
    """Build ms.remote.control JSON payload for a Tizen remote key."""
    tizen_key = SAMSUNG_KEY_MAP.get(key, key)
    if not tizen_key.startswith("KEY_"):
        raise ValueError(f"Unsupported Samsung TV key: {key!r}")
    return {
        "method": "ms.remote.control",
        "params": {
            "Cmd": cmd,
            "DataOfCmd": tizen_key,
            "Option": "false",
            "TypeOfRemote": "SendRemoteKey",
        },
    }


def build_app_launch_payload(app: str) -> Dict[str, Any]:
    """Build ms.channel.emit JSON payload to launch a Tizen Smart TV app."""
    app_id = SAMSUNG_APP_IDS.get(app.lower(), app)
    return {
        "method": "ms.channel.emit",
        "params": {
            "event": "ed.apps.launch",
            "to": "host",
            "data": {
                "appId": app_id,
                "action_type": "DEEP_LINK",
            },
        },
    }


def build_text_input_payload(text: str) -> Dict[str, Any]:
    """Build ms.remote.control SendInputString payload (base64 UTF-8) for Tizen IME."""
    if not isinstance(text, str):
        raise ValueError("Text input must be a string")
    encoded = base64.b64encode(text.encode("utf-8")).decode("ascii")
    return {
        "method": "ms.remote.control",
        "params": {
            "Cmd": encoded,
            "DataOfCmd": "base64",
            "TypeOfRemote": "SendInputString",
        },
    }


def build_text_end_payload() -> Dict[str, Any]:
    """Build ms.remote.control SendInputEnd payload for Tizen IME."""
    return {
        "method": "ms.remote.control",
        "params": {
            "TypeOfRemote": "SendInputEnd",
        },
    }


def guess_media_mime_and_upnp_class(media_url: str, mime_type: str = "") -> Tuple[str, str]:
    """Resolve MIME type and UPnP media class for DLNA DIDL-Lite metadata."""
    mime = (mime_type or "").strip()
    if not mime:
        parsed_path = urlparse(media_url).path
        mime = mimetypes.guess_type(parsed_path)[0] or "video/mp4"
    lower = mime.lower()
    if lower.startswith("audio/"):
        upnp_class = "object.item.audioItem.musicTrack"
    elif lower.startswith("image/"):
        upnp_class = "object.item.imageItem.photo"
    else:
        upnp_class = "object.item.videoItem.movie"
    return mime, upnp_class


def build_didl_lite_metadata(
    media_url: str,
    title: str = "PipaBot Media",
    mime_type: str = "",
) -> str:
    """Build XML DIDL-Lite metadata block for DLNA SetAVTransportURI."""
    mime, upnp_class = guess_media_mime_and_upnp_class(media_url, mime_type)
    safe_title = xml_escape(title or "PipaBot Media", {'"': "&quot;", "'": "&apos;"})
    safe_url = xml_escape(media_url, {'"': "&quot;", "'": "&apos;"})
    safe_mime = xml_escape(mime, {'"': "&quot;", "'": "&apos;"})
    return (
        '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
        'xmlns:dc="http://purl.org/dc/elements/1.1/" '
        'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
        '<item id="0" parentID="-1" restricted="1">'
        f"<dc:title>{safe_title}</dc:title>"
        f"<upnp:class>{upnp_class}</upnp:class>"
        f'<res protocolInfo="http-get:*:{safe_mime}:*">{safe_url}</res>'
        "</item>"
        "</DIDL-Lite>"
    )


def _wrap_soap_envelope(inner_xml: str) -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f"<s:Body>{inner_xml}</s:Body>"
        "</s:Envelope>"
    )


def build_avtransport_set_uri_soap(
    media_url: str,
    title: str = "PipaBot Media",
    mime_type: str = "",
) -> str:
    """Build UPnP AVTransport:1 SetAVTransportURI SOAP XML envelope."""
    cleaned_url = (media_url or "").strip()
    if not cleaned_url.startswith(("http://", "https://")):
        raise ValueError("Media URL must start with http:// or https://")
    didl = build_didl_lite_metadata(cleaned_url, title=title, mime_type=mime_type)
    escaped_url = xml_escape(cleaned_url, {'"': "&quot;", "'": "&apos;"})
    escaped_didl = xml_escape(didl, {'"': "&quot;", "'": "&apos;"})
    body = (
        f'<u:SetAVTransportURI xmlns:u="{AVTRANSPORT_SERVICE_TYPE}">'
        "<InstanceID>0</InstanceID>"
        f"<CurrentURI>{escaped_url}</CurrentURI>"
        f"<CurrentURIMetaData>{escaped_didl}</CurrentURIMetaData>"
        "</u:SetAVTransportURI>"
    )
    return _wrap_soap_envelope(body)


def build_avtransport_play_soap() -> str:
    """Build UPnP AVTransport:1 Play SOAP XML envelope."""
    body = (
        f'<u:Play xmlns:u="{AVTRANSPORT_SERVICE_TYPE}">'
        "<InstanceID>0</InstanceID>"
        "<Speed>1</Speed>"
        "</u:Play>"
    )
    return _wrap_soap_envelope(body)


def build_avtransport_stop_soap() -> str:
    """Build UPnP AVTransport:1 Stop SOAP XML envelope."""
    body = (
        f'<u:Stop xmlns:u="{AVTRANSPORT_SERVICE_TYPE}">'
        "<InstanceID>0</InstanceID>"
        "</u:Stop>"
    )
    return _wrap_soap_envelope(body)


def build_avtransport_pause_soap() -> str:
    """Build UPnP AVTransport:1 Pause SOAP XML envelope."""
    body = (
        f'<u:Pause xmlns:u="{AVTRANSPORT_SERVICE_TYPE}">'
        "<InstanceID>0</InstanceID>"
        "</u:Pause>"
    )
    return _wrap_soap_envelope(body)


def extract_token_from_message(raw_message: str) -> Optional[str]:
    """Extract persisted pairing token from Samsung TV ms.channel.connect message."""
    try:
        payload = json.loads(raw_message)
    except (TypeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    if payload.get("event") != "ms.channel.connect":
        return None
    data = payload.get("data")
    if isinstance(data, dict) and data.get("token"):
        return str(data["token"]).strip()
    return None


class SamsungTVDriver:
    """Async Samsung Smart TV controller over Tizen WebSocket API and DLNA UPnP."""

    def __init__(
        self,
        host: str = "192.168.1.150",
        port: int = 8002,
        fallback_port: int = 8001,
        app_name: str = "PipaBot",
        token_path: Optional[Path] = None,
        mac: str = "",
        broadcast: str = "192.168.1.255",
        dlna_port: int = DEFAULT_DLNA_PORT,
    ) -> None:
        self.host = host
        self.port = port
        self.fallback_port = fallback_port
        self.app_name = app_name
        self.token_path = token_path or (Path.home() / ".config" / "pipa-bot" / "samsung_token.txt")
        self.mac = mac
        self.broadcast = broadcast
        self.dlna_port = int(dlna_port)

    def load_token(self) -> Optional[str]:
        try:
            if self.token_path.is_file():
                tok = self.token_path.read_text(encoding="utf-8").strip()
                return tok or None
        except OSError:
            pass
        return None

    def save_token(self, token: str) -> None:
        cleaned = token.strip()
        if not cleaned:
            return
        self.token_path.parent.mkdir(parents=True, exist_ok=True)
        self.token_path.write_text(cleaned + "\n", encoding="utf-8")

    async def is_online(self, timeout: float = 1.5) -> bool:
        for port in (self.port, self.fallback_port):
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

    async def _send_payloads(
        self,
        payloads: Sequence[Dict[str, Any]],
        timeout: float = 6.0,
    ) -> Tuple[bool, str]:
        try:
            import websockets
        except ImportError:
            return False, "Библиотека websockets не установлена в venv."

        token = self.load_token()
        last_err = ""

        for port in (self.port, self.fallback_port):
            url = build_ws_url(self.host, port=port, app_name=self.app_name, token=token)
            ssl_ctx = ssl._create_unverified_context() if port == 8002 else None
            try:
                async with websockets.connect(
                    url,
                    ssl=ssl_ctx,
                    open_timeout=timeout,
                    close_timeout=2.0,
                ) as ws:
                    try:
                        # On first pairing (no token yet), Samsung Tizen waits for the user to press "Allow" on TV
                        handshake_timeout = 3.0 if token else 20.0
                        hello = await asyncio.wait_for(ws.recv(), timeout=handshake_timeout)
                        if isinstance(hello, bytes):
                            hello = hello.decode("utf-8", errors="replace")
                        new_tok = extract_token_from_message(hello)
                        if new_tok and new_tok != token:
                            self.save_token(new_tok)
                    except asyncio.TimeoutError:
                        pass

                    for payload in payloads:
                        await ws.send(json.dumps(payload))
                        await asyncio.sleep(0.15)
                    return True, "Команда отправлена на Samsung TV."
            except Exception as exc:
                last_err = str(exc)
                continue

        return False, f"Samsung TV не отвечает ({self.host}): {last_err[:160]}"

    async def _send_payload(self, payload: Dict[str, Any], timeout: float = 6.0) -> Tuple[bool, str]:
        return await self._send_payloads([payload], timeout=timeout)

    async def send_key(self, key: str) -> Tuple[bool, str]:
        try:
            payload = build_key_payload(key)
        except ValueError as exc:
            return False, str(exc)
        ok, msg = await self._send_payload(payload)
        if ok:
            return True, f"Samsung TV: нажата кнопка {payload['params']['DataOfCmd']}."
        return False, msg

    async def send_text(self, text: str, submit: bool = False) -> Tuple[bool, str]:
        """Send UTF-8 text (Russian/English) via Tizen SendInputString, optionally pressing KEY_ENTER."""
        if not isinstance(text, str) or not text:
            return False, "Текст для отправки на Samsung TV не может быть пустым."
        payloads = [build_text_input_payload(text)]
        if submit:
            payloads.append(build_key_payload("enter"))
        ok, msg = await self._send_payloads(payloads)
        if ok:
            suffix = " (+ Enter)" if submit else ""
            return True, f"Samsung TV: текст отправлен ({len(text)} симв.){suffix}."
        return False, msg

    async def launch_app(self, app: str) -> Tuple[bool, str]:
        payload = build_app_launch_payload(app)
        ok, msg = await self._send_payload(payload)
        if ok:
            return True, f"Samsung TV: запуск приложения {app}."
        return False, msg

    async def _send_soap(
        self,
        action: str,
        soap_xml: str,
        control_path: str = AVTRANSPORT_CONTROL_PATH,
        service_type: str = AVTRANSPORT_SERVICE_TYPE,
        timeout: float = 8.0,
    ) -> Tuple[bool, int, str]:
        """Post a UPnP SOAP action to Samsung TV DLNA MediaRenderer."""
        url = f"http://{self.host}:{self.dlna_port}{control_path}"
        body_bytes = soap_xml.encode("utf-8")
        headers = {
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": f'"{service_type}#{action}"',
        }

        try:
            import aiohttp

            client_timeout = aiohttp.ClientTimeout(total=timeout)
            async with aiohttp.ClientSession(timeout=client_timeout) as session:
                async with session.post(url, data=body_bytes, headers=headers) as resp:
                    raw = await resp.text(errors="replace")
                    return (200 <= resp.status < 300), resp.status, raw
        except ImportError:
            pass
        except Exception as exc:
            return False, 0, str(exc)

        def _sync_post() -> Tuple[bool, int, str]:
            req = urllib.request.Request(url, data=body_bytes, headers=headers, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    raw = resp.read().decode("utf-8", errors="replace")
                    return (200 <= resp.status < 300), resp.status, raw
            except urllib.error.HTTPError as err:
                raw = err.read().decode("utf-8", errors="replace")
                return False, err.code, raw
            except Exception as exc:
                return False, 0, str(exc)

        return await asyncio.to_thread(_sync_post)

    async def cast_url(
        self,
        media_url: str,
        title: str = "PipaBot Media",
        mime_type: str = "",
    ) -> Tuple[bool, str]:
        """Cast an HTTP/HTTPS media URL to Samsung TV DLNA AVTransport (SetAVTransportURI + Play)."""
        try:
            set_uri_xml = build_avtransport_set_uri_soap(media_url, title=title, mime_type=mime_type)
        except ValueError as exc:
            return False, str(exc)

        ok_set, status_set, body_set = await self._send_soap("SetAVTransportURI", set_uri_xml)
        if not ok_set:
            detail = body_set.strip()[:140] or f"HTTP {status_set}"
            return False, f"Samsung TV DLNA не принял URL (HTTP {status_set}): {detail}"

        await asyncio.sleep(0.2)
        ok_play, status_play, body_play = await self._send_soap("Play", build_avtransport_play_soap())
        if not ok_play:
            detail = body_play.strip()[:140] or f"HTTP {status_play}"
            return False, f"Samsung TV DLNA не смог запустить воспроизведение (HTTP {status_play}): {detail}"

        return True, f"Воспроизведение запущено на Samsung TV: {title}"

    async def cast_stop(self) -> Tuple[bool, str]:
        """Stop current DLNA playback on Samsung TV."""
        ok, status, body = await self._send_soap("Stop", build_avtransport_stop_soap())
        if ok:
            return True, "Воспроизведение Cast на Samsung TV остановлено."
        detail = body.strip()[:140] or f"HTTP {status}"
        return False, f"Не удалось остановить Cast (HTTP {status}): {detail}"

    async def cast_pause(self) -> Tuple[bool, str]:
        """Pause current DLNA playback on Samsung TV."""
        ok, status, body = await self._send_soap("Pause", build_avtransport_pause_soap())
        if ok:
            return True, "Воспроизведение Cast на Samsung TV поставлено на паузу."
        detail = body.strip()[:140] or f"HTTP {status}"
        return False, f"Не удалось поставить Cast на паузу (HTTP {status}): {detail}"

    async def wake(self) -> Tuple[bool, str]:
        if not self.mac:
            return False, "MAC-адрес Samsung TV не задан в config.json."
        try:
            await asyncio.to_thread(send_wol, self.mac, self.broadcast)
            return True, f"Wake-on-LAN пакет отправлен на Samsung TV ({self.mac})."
        except Exception as exc:
            return False, f"Ошибка WoL для Samsung TV: {exc}"
