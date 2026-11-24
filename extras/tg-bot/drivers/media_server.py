"""Lightweight local LAN HTTP media server for casting tablet files to Samsung TV DLNA."""

from __future__ import annotations

import asyncio
import mimetypes
import re
import secrets
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
from urllib.parse import quote, unquote


DEFAULT_MEDIA_HOST = "0.0.0.0"
DEFAULT_MEDIA_PORT = 18766
DEFAULT_MAX_ENTRIES = 64
DEFAULT_TTL_SEC = 24 * 3600

_SAFE_MEDIA_NAME_RE = re.compile(r"[^a-zA-Z0-9._-]+")
_RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")


def sanitize_media_filename(name: str, fallback: str = "media.bin") -> str:
    """Return a path-safe ASCII-friendly filename for HTTP media URLs."""
    base = Path(name or "").name.strip()
    base = _SAFE_MEDIA_NAME_RE.sub("_", base).strip("._ ")
    if not base:
        base = fallback
    return base[:120]


def has_path_traversal(raw_segment: str) -> bool:
    """Detect path traversal or separator injection in raw/decoded URL path segments."""
    if not raw_segment:
        return True
    decoded = unquote(raw_segment)
    for candidate in (raw_segment, decoded):
        if "\x00" in candidate or "/" in candidate or "\\" in candidate:
            return True
        if candidate in (".", "..") or ".." in candidate:
            return True
    return False


def parse_byte_range(range_header: str, file_size: int) -> Optional[Tuple[int, int]]:
    """Parse HTTP `Range: bytes=start-end` into inclusive (start, end), or None if unsatisfiable."""
    if file_size <= 0:
        return None
    m = _RANGE_RE.match((range_header or "").strip())
    if not m:
        return None
    start_s, end_s = m.group(1), m.group(2)
    if not start_s and not end_s:
        return None
    if not start_s:
        suffix_len = int(end_s)
        if suffix_len <= 0:
            return None
        start = max(0, file_size - suffix_len)
        end = file_size - 1
    else:
        start = int(start_s)
        end = int(end_s) if end_s else file_size - 1
    if start >= file_size or end < start:
        return None
    return start, min(end, file_size - 1)


@dataclass
class MediaEntry:
    token: str
    path: Path
    filename: str
    mime_type: str
    created_at: float


class _StdlibMediaHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True

    def __init__(self, server_address: Tuple[str, int], media_server: "LocalMediaServer") -> None:
        self.media_server = media_server
        super().__init__(server_address, _StdlibMediaHandler)


class _StdlibMediaHandler(BaseHTTPRequestHandler):
    server: _StdlibMediaHTTPServer

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def _handle_media(self, head_only: bool) -> None:
        raw_path = self.path.split("?", 1)[0]
        if ".." in raw_path or "%2e%2e" in raw_path.lower() or "\\" in raw_path or "%5c" in raw_path.lower():
            self.send_error(HTTPStatus.FORBIDDEN, "Path traversal rejected")
            return

        parts = [p for p in raw_path.split("/") if p]
        if len(parts) != 3 or parts[0] != "media":
            self.send_error(HTTPStatus.NOT_FOUND, "Not found")
            return

        token_raw, fname_raw = parts[1], parts[2]
        if has_path_traversal(token_raw) or has_path_traversal(fname_raw):
            self.send_error(HTTPStatus.FORBIDDEN, "Path traversal rejected")
            return

        entry = self.server.media_server.lookup(token_raw, unquote(fname_raw))
        if entry is None:
            self.send_error(HTTPStatus.NOT_FOUND, "Media not found")
            return

        try:
            file_size = entry.path.stat().st_size
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "File unavailable")
            return

        range_hdr = self.headers.get("Range")
        if range_hdr:
            parsed = parse_byte_range(range_hdr, file_size)
            if parsed is None:
                self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return
            start, end = parsed
            length = end - start + 1
            self.send_response(HTTPStatus.PARTIAL_CONTENT)
            self.send_header("Content-Type", entry.mime_type)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Content-Length", str(length))
            self.end_headers()
            if not head_only:
                self._stream_range(entry.path, start, length)
            return

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", entry.mime_type)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(file_size))
        self.end_headers()
        if not head_only:
            self._stream_range(entry.path, 0, file_size)

    def _stream_range(self, path: Path, offset: int, length: int) -> None:
        remaining = length
        with path.open("rb") as fh:
            fh.seek(offset)
            while remaining > 0:
                chunk = fh.read(min(65536, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    break
                remaining -= len(chunk)

    def do_GET(self) -> None:
        self._handle_media(head_only=False)

    def do_HEAD(self) -> None:
        self._handle_media(head_only=True)


class LocalMediaServer:
    """Serves registered local tablet files over LAN HTTP for DLNA cast to Samsung TV.

    Uses `aiohttp.web` with `FileResponse` (native Range support) when available,
    falling back seamlessly to a pure-stdlib Range-capable `ThreadingHTTPServer`.
    """

    def __init__(
        self,
        advertise_host: str = "192.168.1.50",
        bind_host: str = DEFAULT_MEDIA_HOST,
        port: int = DEFAULT_MEDIA_PORT,
        max_entries: int = DEFAULT_MAX_ENTRIES,
        ttl_sec: float = DEFAULT_TTL_SEC,
    ) -> None:
        self.advertise_host = advertise_host
        self.bind_host = bind_host
        self.port = int(port)
        self.max_entries = max(1, int(max_entries))
        self.ttl_sec = max(1.0, float(ttl_sec))
        self._lock = threading.Lock()
        self._entries: Dict[str, MediaEntry] = {}
        self._actual_port: int = self.port

        # Runtime handles (either aiohttp or stdlib fallback)
        self._aio_runner: Optional[Any] = None
        self._aio_site: Optional[Any] = None
        self._stdlib_server: Optional[_StdlibMediaHTTPServer] = None
        self._stdlib_thread: Optional[threading.Thread] = None
        self._running: bool = False

    @property
    def is_running(self) -> bool:
        return self._running

    @property
    def actual_port(self) -> int:
        return self._actual_port

    def _cleanup_locked(self, now: float) -> None:
        expired = [
            tok
            for tok, entry in self._entries.items()
            if (now - entry.created_at) > self.ttl_sec or not entry.path.is_file()
        ]
        for tok in expired:
            self._entries.pop(tok, None)
        while len(self._entries) > self.max_entries:
            oldest_key = next(iter(self._entries))
            self._entries.pop(oldest_key, None)

    def register_file(
        self,
        file_path: Path | str,
        filename: Optional[str] = None,
        mime_type: str = "",
        advertise_host: Optional[str] = None,
    ) -> Tuple[str, str, str]:
        """Register a local file and return (token, http_url, mime_type)."""
        resolved = Path(file_path).expanduser().resolve()
        if not resolved.is_file():
            raise FileNotFoundError(f"Media file does not exist: {resolved}")

        safe_name = sanitize_media_filename(filename or resolved.name)
        guessed = mime_type.strip() if mime_type else ""
        if not guessed:
            guessed = mimetypes.guess_type(safe_name)[0] or mimetypes.guess_type(resolved.name)[0] or "application/octet-stream"

        token = secrets.token_hex(8)
        now = time.monotonic()
        entry = MediaEntry(
            token=token,
            path=resolved,
            filename=safe_name,
            mime_type=guessed,
            created_at=now,
        )
        with self._lock:
            self._entries[token] = entry
            self._cleanup_locked(now)

        host = (advertise_host or self.advertise_host).strip() or "127.0.0.1"
        url = f"http://{host}:{self._actual_port}/media/{token}/{quote(safe_name)}"
        return token, url, guessed

    def lookup(self, token: str, filename: str) -> Optional[MediaEntry]:
        """Resolve a registered media entry strictly matching token and filename."""
        if has_path_traversal(token) or has_path_traversal(filename):
            return None
        now = time.monotonic()
        with self._lock:
            self._cleanup_locked(now)
            entry = self._entries.get(token)
            if entry is None:
                return None
            if entry.filename != filename:
                return None
            if not entry.path.is_file():
                self._entries.pop(token, None)
                return None
            return entry

    def entry_count(self) -> int:
        with self._lock:
            self._cleanup_locked(time.monotonic())
            return len(self._entries)

    async def start(self) -> int:
        """Start the local HTTP media server and return the bound port."""
        if self._running:
            return self._actual_port

        try:
            from aiohttp import web

            @web.middleware
            async def traversal_guard(request: web.Request, handler: Any) -> web.StreamResponse:
                raw_target = request.raw_path.split("?", 1)[0]
                if ".." in raw_target or "%2e%2e" in raw_target.lower() or "\\" in raw_target or "%5c" in raw_target.lower():
                    raise web.HTTPForbidden(text="Path traversal rejected")
                return await handler(request)

            async def handle_media(request: web.Request) -> web.StreamResponse:
                token = request.match_info.get("token", "")
                filename = request.match_info.get("filename", "")
                if has_path_traversal(token) or has_path_traversal(filename):
                    raise web.HTTPForbidden(text="Path traversal rejected")
                entry = self.lookup(token, filename)
                if entry is None:
                    raise web.HTTPNotFound(text="Media not found")
                return web.FileResponse(
                    path=entry.path,
                    headers={
                        "Content-Type": entry.mime_type,
                        "Accept-Ranges": "bytes",
                    },
                )

            app = web.Application(middlewares=[traversal_guard])
            app.router.add_get("/media/{token}/{filename}", handle_media, allow_head=True)

            runner = web.AppRunner(app, access_log=None)
            await runner.setup()
            site = web.TCPSite(runner, host=self.bind_host, port=self.port)
            await site.start()

            actual = self.port
            if getattr(site, "_server", None) and getattr(site._server, "sockets", None):
                actual = int(site._server.sockets[0].getsockname()[1])

            self._aio_runner = runner
            self._aio_site = site
            self._actual_port = actual
            self._running = True
            return self._actual_port
        except ImportError:
            pass

        # Pure-stdlib fallback (for environments without aiohttp, e.g. unit test hosts)
        srv = _StdlibMediaHTTPServer((self.bind_host, self.port), self)
        self._actual_port = int(srv.server_address[1])
        thread = threading.Thread(
            target=srv.serve_forever,
            kwargs={"poll_interval": 0.05},
            daemon=True,
            name="pipa-media-server",
        )
        thread.start()
        self._stdlib_server = srv
        self._stdlib_thread = thread
        self._running = True
        return self._actual_port

    async def stop(self) -> None:
        """Stop the local HTTP media server cleanly."""
        if not self._running:
            return
        self._running = False
        if self._aio_runner is not None:
            runner = self._aio_runner
            self._aio_runner = None
            self._aio_site = None
            await runner.cleanup()
        if self._stdlib_server is not None:
            srv = self._stdlib_server
            thread = self._stdlib_thread
            self._stdlib_server = None
            self._stdlib_thread = None
            await asyncio.to_thread(srv.shutdown)
            srv.server_close()
            if thread is not None:
                await asyncio.to_thread(thread.join, 3.0)
