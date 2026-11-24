"""Unit and integration tests for Wave 4: pipa-tg-bot + pipa-pc-agent."""

from __future__ import annotations

import ast
import asyncio
import base64
import http.client
import json
import sys
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any, Dict, List, Optional
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2]
TG_BOT_DIR = ROOT / "extras" / "tg-bot"
PC_AGENT_DIR = ROOT / "extras" / "pc-agent"

for p in (str(TG_BOT_DIR), str(PC_AGENT_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

import callbacks  # noqa: E402
import commands  # noqa: E402
import config as bot_config  # noqa: E402
import helpers  # noqa: E402
import pc_agent  # noqa: E402
import ui  # noqa: E402
from drivers import android_tv, media_server, network, pc as pc_driver, samsung_tv, tablet  # noqa: E402


FAKE_PNG = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDRfake-png-data"


class FakePCExecutor:
    """Deterministic fake executor for testing pc_agent HTTP endpoints."""

    def __init__(self) -> None:
        self.calls: List[tuple[str, Dict[str, Any]]] = []
        self.clipboard_text: str = ""

    def get_status(self) -> Dict[str, Any]:
        return {
            "ok": True,
            "hostname": "mypc",
            "os": "Windows 11",
            "uptime_sec": 7325,
            "cpu_pct": 18.5,
            "ram_used_mb": 10240,
            "ram_total_mb": 32768,
            "ram_pct": 31.3,
            "battery": None,
            "active_window": "Visual Studio Code",
        }

    def take_screenshot(self) -> bytes:
        return FAKE_PNG

    def get_clipboard(self) -> str:
        return self.clipboard_text

    def set_clipboard(self, text: str) -> int:
        self.clipboard_text = text
        return len(text)

    def perform_action(self, action: str, params: Dict[str, Any]) -> Dict[str, Any]:
        if action not in pc_agent.SUPPORTED_ACTIONS:
            raise ValueError(f"Unsupported action: {action}")
        if action == "open_url":
            url = str(params.get("url") or "").strip()
            if not url.startswith(("http://", "https://")):
                raise ValueError("URL must start with http:// or https://")
        self.calls.append((action, params))
        return {"ok": True, "action": action, "message": f"Executed {action}"}


# ---------------------------------------------------------------------------
# 1. Config & Zero-Friction Owner Pairing Tests
# ---------------------------------------------------------------------------

class ConfigAndPairingTests(unittest.TestCase):
    def test_default_config_contains_discovered_lan_devices(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = bot_config.load_config(config_dir=Path(tmp), environ={})
            self.assertEqual(cfg.samsung_tv.host, "192.168.1.150")
            self.assertEqual(cfg.samsung_tv.port, 8002)
            self.assertEqual(cfg.android_tv.host, "192.168.1.160")
            self.assertEqual(cfg.android_tv.adb_port, 5555)
            self.assertEqual(cfg.network.router_ip, "192.168.1.1")
            mypc = cfg.get_pc("mypc")
            self.assertIsNotNone(mypc)
            self.assertEqual(mypc.host, "192.168.1.100")
            self.assertEqual(mypc.mac.lower(), "00:11:22:33:44:55")
            self.assertEqual(mypc.port, 18765)

    def test_loads_bot_env_and_json_and_saves_atomically(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_dir = Path(tmp)
            (cfg_dir / "bot.env").write_text(
                'BOT_TOKEN="12345:SECRET"\nPC_AGENT_TOKEN=tok_abc\n# comment\n',
                encoding="utf-8",
            )
            cfg = bot_config.load_config(config_dir=cfg_dir, environ={})
            self.assertEqual(cfg.bot_token, "12345:SECRET")
            self.assertEqual(cfg.pc_agent_token, "tok_abc")
            self.assertIsNone(cfg.owner_id)

            cfg.owner_id = 987654321
            cfg.alerts.low_battery_pct = 20
            saved = cfg.save()
            self.assertTrue(saved.is_file())

            # Reload and verify secrets aren't written to config.json, while settings persist
            raw_json = json.loads(saved.read_text(encoding="utf-8"))
            self.assertNotIn("bot_token", raw_json)
            self.assertEqual(raw_json["owner_id"], 987654321)

            cfg2 = bot_config.load_config(config_dir=cfg_dir, environ={})
            self.assertEqual(cfg2.owner_id, 987654321)
            self.assertEqual(cfg2.alerts.low_battery_pct, 20)
            self.assertEqual(cfg2.effective_pc_token(cfg2.pcs[0]), "tok_abc")

    def test_zero_friction_owner_pairing_flow(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_dir = Path(tmp)
            cfg = bot_config.load_config(config_dir=cfg_dir, environ={})
            auth = bot_config.OwnerAuthManager(cfg)

            self.assertFalse(auth.is_paired)
            self.assertIsNotNone(auth.pair_code)
            self.assertEqual(len(auth.pair_code), 6)
            self.assertTrue(auth.pair_code.isdigit())
            self.assertTrue(cfg.pair_code_path.is_file())
            self.assertEqual(cfg.pair_code_path.read_text(encoding="utf-8").strip(), auth.pair_code)

            # Wrong code rejected
            ok, _ = auth.try_pair(11111, "000000" if auth.pair_code != "000000" else "999999")
            self.assertFalse(ok)
            self.assertFalse(auth.is_paired)
            self.assertFalse(auth.is_owner(11111))

            # Correct code locks owner permanently and removes pair_code.txt
            ok, msg = auth.try_pair(424242, auth.pair_code)
            self.assertTrue(ok, msg)
            self.assertTrue(auth.is_paired)
            self.assertTrue(auth.is_owner(424242))
            self.assertFalse(auth.is_owner(999999))
            self.assertFalse(cfg.pair_code_path.exists())

            # Second user cannot hijack pairing
            ok2, _ = auth.try_pair(999999, "123456")
            self.assertFalse(ok2)
            self.assertTrue(auth.is_owner(424242))

            # Reloaded config preserves permanent owner_id without generating new pair_code
            reloaded = bot_config.load_config(config_dir=cfg_dir, environ={})
            auth2 = bot_config.OwnerAuthManager(reloaded)
            self.assertTrue(auth2.is_paired)
            self.assertIsNone(auth2.pair_code)
            self.assertTrue(auth2.is_owner(424242))


# ---------------------------------------------------------------------------
# 2. Command Catalog & Help Completeness Tests
# ---------------------------------------------------------------------------

class CommandCatalogTests(unittest.TestCase):
    REQUIRED_COMMANDS = {
        "start",
        "status",
        "tablet",
        "screen",
        "vpn",
        "services",
        "pc",
        "tv",
        "net",
        "run",
        "open",
        "get",
        "alerts",
        "help",
    }

    def test_all_required_commands_present_and_valid(self):
        errors = commands.validate_command_catalog()
        self.assertEqual(errors, [])
        self.assertLessEqual(len(commands.BOT_COMMANDS), 20)
        names = {c.command for c in commands.BOT_COMMANDS}
        self.assertTrue(self.REQUIRED_COMMANDS.issubset(names))
        wave5_cmds = {"term", "clip", "scenes", "tv_text", "tv_cast"}
        self.assertTrue(wave5_cmds.issubset(names))
        self.assertIn("cam", names)
        self.assertEqual(len(commands.BOT_COMMANDS), 20)

    def test_help_text_documents_every_registered_command(self):
        help_text = commands.build_help_text()
        for spec in commands.BOT_COMMANDS:
            self.assertIn(f"/{spec.command}", help_text)
        self.assertIn("/cmd", help_text)


# ---------------------------------------------------------------------------
# 3. Callbacks & Russian UI Keyboard Tests (strict <= 64-byte callback_data)
# ---------------------------------------------------------------------------

class CallbackAndUITests(unittest.TestCase):
    def test_callback_pack_unpack_and_length_limit(self):
        packed = callbacks.pack_cb("p", "act", "mypc", "media_play_pause")
        self.assertLessEqual(len(packed.encode("utf-8")), 64)
        scope, action, args = callbacks.unpack_cb(packed)
        self.assertEqual(scope, "p")
        self.assertEqual(action, "act")
        self.assertEqual(args, ("mypc", "media_play_pause"))

        with self.assertRaises(ValueError):
            callbacks.pack_cb("x", "y", "a:b")
        with self.assertRaises(ValueError):
            callbacks.pack_cb("x", "y", "z" * 65)
        with self.assertRaises(ValueError):
            callbacks.unpack_cb("malformed")

    def test_render_bar_and_html_escape(self):
        self.assertEqual(ui.render_bar(0, 10), "░░░░░░░░░░ 0%")
        self.assertEqual(ui.render_bar(50, 10), "█████░░░░░ 50%")
        self.assertEqual(ui.render_bar(100, 10), "██████████ 100%")
        self.assertIn("—", ui.render_bar(None, 8))
        self.assertEqual(ui.esc("<tag>&'"), "&lt;tag&gt;&amp;&#x27;")

    def test_every_ui_panel_buttons_fit_64_bytes_and_are_valid(self):
        cfg = bot_config.BotConfig()
        sample_tablet = {
            "battery": {"capacity": 68, "status": "Discharging", "temp_c": 31.5, "power_w": -4.2},
            "brightness_pct": 60,
            "volume_pct": 45,
            "muted": False,
            "cpu_pct": 14.0,
            "cpu_temp_c": 41.2,
            "ram_used_mb": 2100,
            "ram_total_mb": 7800,
            "ram_pct": 26.9,
            "disk_free_gb": 84.3,
            "disk_total_gb": 115.0,
            "uptime_sec": 91234,
            "wifi_ip": "192.168.1.50",
            "vpn_short": "Вкл (nl-ams)",
        }
        sample_services = [
            {"key": s.key, "label": s.label, "active": (i % 2 == 0), "state": "active" if i % 2 == 0 else "inactive", "critical": s.critical}
            for i, s in enumerate(tablet.MANAGED_SERVICES)
        ]
        sample_pc_status = FakePCExecutor().get_status()
        sample_devices = [
            {"ip": "192.168.1.1", "mac": "aa:bb:cc:dd:ee:01", "state": "ONLINE", "label": "Роутер (Home Router)"},
            {"ip": "192.168.1.100", "mac": "00:11:22:33:44:55", "state": "REACHABLE", "label": "My PC (Windows)"},
        ]

        panels = [
            ui.build_main_dashboard(sample_tablet),
            ui.build_status_summary(sample_tablet, [(cfg.pcs[0], True, sample_pc_status)], True, False, cfg),
            ui.build_tablet_panel(sample_tablet),
            ui.build_vpn_panel(["nl-ams-very-long-vpn-profile-name-01", "us-nyc"], "active: nl-ams", "nl-ams-very-long-vpn-profile-name-01"),
            ui.build_services_panel(sample_services),
            ui.build_pc_list_panel([(cfg.pcs[0], True)]),
            ui.build_pc_detail_panel(cfg.pcs[0], True, sample_pc_status),
            ui.build_tv_selector_panel(cfg, True, True),
            ui.build_samsung_tv_panel(cfg, True),
            ui.build_android_tv_panel(cfg, True),
            ui.build_network_panel({"ssid": "Home5G", "ip": "192.168.1.50", "signal_dbm": -52}, "Выкл", 12.4, sample_devices),
            ui.build_alerts_panel(cfg.alerts),
            ui.build_confirm_dialog("Переход в One UI", "Опасно", "toneui", callbacks.pack_cb("t", "open")),
            ui.build_clipboard_panel(True, "Привет с планшета", cfg.pcs[0].name, True, "Текст с ПК (Windows)"),
            ui.build_scenes_panel(report_lines=["🎬 Сценарий «Кино»:", "1. ✅ Samsung TV уже включён."]),
            ui.build_terminal_panel("/home/pipa/projects", command="ls -la", rc=0, output="total 4\nREADME.md"),
            ui.build_camera_panel(torch_on=False),
            ui.build_camera_panel(torch_on=True, notice="Фонарик включён."),
        ]

        total_buttons = 0
        for text, rows in panels:
            self.assertTrue(len(text) > 10)
            self.assertLessEqual(len(text), 4096)
            for row in rows:
                for btn in row:
                    total_buttons += 1
                    self.assertTrue(btn.text.strip())
                    raw = btn.callback_data.encode("utf-8")
                    self.assertLessEqual(len(raw), 64, f"Button {btn.text!r} callback_data > 64 bytes: {btn.callback_data!r}")
                    callbacks.unpack_cb(btn.callback_data)
        self.assertGreater(total_buttons, 80)

        # Verify Wave 5 & Wave 6 specific buttons exist on main dashboard, Samsung TV, and Camera panel
        main_cbs = {btn.callback_data for row in panels[0][1] for btn in row}
        self.assertIn("sc:open", main_cbs)
        self.assertIn("cl:open", main_cbs)
        self.assertIn("tm:open", main_cbs)
        self.assertIn("m:cam", main_cbs)

        cam_off_text, cam_off_rows = panels[-2]
        cam_on_text, cam_on_rows = panels[-1]
        cam_cbs = {btn.callback_data for row in cam_off_rows for btn in row}
        for expected_cb in (
            "cam:snap:back:0:off",
            "cam:snap:front:0:off",
            "cam:snap:back:0:on",
            "cam:snap:back:35:off",
            "cam:snap:back:70:off",
            "cam:torch:toggle",
            "cam:open",
            "m:home",
        ):
            self.assertIn(expected_cb, cam_cbs)
        self.assertIn("ВЫКЛ", cam_off_text)
        self.assertIn("ВКЛ", cam_on_text)
        self.assertTrue(any("ВЫКЛ" in btn.text for row in cam_off_rows for btn in row))
        self.assertTrue(any("ВКЛ" in btn.text for row in cam_on_rows for btn in row))

        samsung_cbs = {btn.callback_data for row in panels[8][1] for btn in row}
        self.assertIn("s:text", samsung_cbs)
        self.assertIn("s:cast", samsung_cbs)
        self.assertIn("s:cstop", samsung_cbs)

        cast_file_cb = callbacks.pack_cb("s", "castf", "0123456789ab")
        self.assertLessEqual(len(cast_file_cb.encode("utf-8")), 64)


# ---------------------------------------------------------------------------
# 4. Wake-on-LAN, Samsung TV & Android TV Driver Tests
# ---------------------------------------------------------------------------

class DeviceDriversTests(unittest.TestCase):
    def test_wol_magic_packet_construction(self):
        pkt_colon = pc_driver.build_wol_packet("00:11:22:33:44:55")
        pkt_dash = pc_driver.build_wol_packet("00-11-22-33-44-55")
        self.assertEqual(len(pkt_colon), 102)
        self.assertEqual(pkt_colon, pkt_dash)
        self.assertEqual(pkt_colon[:6], b"\xff" * 6)
        self.assertEqual(pkt_colon[6:], bytes.fromhex("001122334455") * 16)

        with self.assertRaises(ValueError):
            pc_driver.build_wol_packet("not-a-mac")

    def test_samsung_tv_ws_url_payload_and_token_persistence(self):
        url_wss = samsung_tv.build_ws_url("192.168.1.150", 8002, "PipaBot", token="12345678")
        self.assertTrue(url_wss.startswith("wss://192.168.1.150:8002/api/v2/channels/samsung.remote.control?name="))
        self.assertIn("name=UGlwYUJvdA%3D%3D", url_wss)
        self.assertIn("&token=12345678", url_wss)

        url_ws = samsung_tv.build_ws_url("192.168.1.150", 8001, "PipaBot")
        self.assertTrue(url_ws.startswith("ws://192.168.1.150:8001/"))
        self.assertNotIn("token=", url_ws)

        payload = samsung_tv.build_key_payload("vol_up")
        self.assertEqual(payload["method"], "ms.remote.control")
        self.assertEqual(payload["params"]["Cmd"], "Click")
        self.assertEqual(payload["params"]["DataOfCmd"], "KEY_VOLUP")
        self.assertEqual(payload["params"]["TypeOfRemote"], "SendRemoteKey")

        with self.assertRaises(ValueError):
            samsung_tv.build_key_payload("invalid_key_xyz")

        yt_payload = samsung_tv.build_app_launch_payload("youtube")
        self.assertEqual(yt_payload["method"], "ms.channel.emit")
        self.assertEqual(yt_payload["params"]["data"]["appId"], "111299001912")

        hello = json.dumps({"event": "ms.channel.connect", "data": {"token": "99887766"}})
        self.assertEqual(samsung_tv.extract_token_from_message(hello), "99887766")

        with tempfile.TemporaryDirectory() as tmp:
            drv = samsung_tv.SamsungTVDriver(token_path=Path(tmp) / "samsung_token.txt")
            self.assertIsNone(drv.load_token())
            drv.save_token("99887766")
            self.assertEqual(drv.load_token(), "99887766")

    def test_android_tv_adb_command_building(self):
        self.assertEqual(
            android_tv.build_connect_cmd("192.168.1.160", 5555),
            ["adb", "connect", "192.168.1.160:5555"],
        )
        self.assertEqual(
            android_tv.build_keyevent_cmd("192.168.1.160", "home"),
            ["adb", "-s", "192.168.1.160:5555", "shell", "input", "keyevent", "3"],
        )
        self.assertEqual(
            android_tv.build_keyevent_cmd("192.168.1.160", "play_pause"),
            ["adb", "-s", "192.168.1.160:5555", "shell", "input", "keyevent", "85"],
        )
        text_cmd = android_tv.build_text_cmd("192.168.1.160", "hello world & test;")
        self.assertEqual(text_cmd[-1], r"hello%sworld%s\&%stest\;")

        app_cmd = android_tv.build_app_launch_cmd("192.168.1.160", "kinopoisk")
        self.assertIn("ru.kinopoisk.tv", app_cmd)
        self.assertIn("monkey", app_cmd)

        with self.assertRaises(ValueError):
            android_tv.build_app_launch_cmd("192.168.1.160", "bad;rm -rf /")

        self.assertEqual(
            android_tv.build_screencap_cmd("192.168.1.160"),
            ["adb", "-s", "192.168.1.160:5555", "exec-out", "screencap", "-p"],
        )

    def test_samsung_tv_text_input_and_dlna_soap_builders(self):
        sample_text = "Привет, Samsung! 🎬"
        txt_payload = samsung_tv.build_text_input_payload(sample_text)
        self.assertEqual(txt_payload["method"], "ms.remote.control")
        self.assertEqual(txt_payload["params"]["DataOfCmd"], "base64")
        self.assertEqual(txt_payload["params"]["TypeOfRemote"], "SendInputString")
        decoded = base64.b64decode(txt_payload["params"]["Cmd"]).decode("utf-8")
        self.assertEqual(decoded, sample_text)

        end_payload = samsung_tv.build_text_end_payload()
        self.assertEqual(end_payload["method"], "ms.remote.control")
        self.assertEqual(end_payload["params"]["TypeOfRemote"], "SendInputEnd")

        # UPnP AVTransport SetAVTransportURI SOAP XML validation & XML escaping
        media_url = "http://192.168.1.50:18766/media/tok123/movie.mp4?a=1&b=2"
        set_uri_xml = samsung_tv.build_avtransport_set_uri_soap(
            media_url,
            title='Фильм "Тест" & <HD>',
            mime_type="video/mp4",
        )
        root = ET.fromstring(set_uri_xml)
        cur_uri = root.find(".//CurrentURI")
        cur_meta = root.find(".//CurrentURIMetaData")
        self.assertIsNotNone(cur_uri)
        self.assertEqual(cur_uri.text, media_url)
        self.assertIsNotNone(cur_meta)
        didl = ET.fromstring(cur_meta.text)
        self.assertIn("object.item.videoItem.movie", cur_meta.text)
        self.assertIn("http-get:*:video/mp4:*", cur_meta.text)
        self.assertIsNotNone(didl)

        # Audio and image UPnP class inference
        self.assertEqual(
            samsung_tv.guess_media_mime_and_upnp_class("http://host/track.mp3"),
            ("audio/mpeg", "object.item.audioItem.musicTrack"),
        )
        self.assertEqual(
            samsung_tv.guess_media_mime_and_upnp_class("http://host/pic.png"),
            ("image/png", "object.item.imageItem.photo"),
        )

        # Non-HTTP URL rejected
        with self.assertRaises(ValueError):
            samsung_tv.build_avtransport_set_uri_soap("file:///etc/passwd")

        # Play / Stop / Pause SOAP envelopes are valid XML
        for soap_xml, tag in (
            (samsung_tv.build_avtransport_play_soap(), "Play"),
            (samsung_tv.build_avtransport_stop_soap(), "Stop"),
            (samsung_tv.build_avtransport_pause_soap(), "Pause"),
        ):
            parsed = ET.fromstring(soap_xml)
            self.assertIn(tag, ET.tostring(parsed, encoding="unicode"))


class SamsungDLNAAndMediaServerAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_samsung_tv_dlna_cast_live_soap_http(self):
        received: List[tuple[str, str, str]] = []

        class MockDLNAHandler(BaseHTTPRequestHandler):
            def log_message(self, fmt: str, *args: Any) -> None:
                return

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length") or "0")
                body = self.rfile.read(length).decode("utf-8", errors="replace")
                received.append((self.path, self.headers.get("SOAPACTION", ""), body))
                resp = b'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body/></s:Envelope>'
                self.send_response(200)
                self.send_header("Content-Type", "text/xml; charset=utf-8")
                self.send_header("Content-Length", str(len(resp)))
                self.end_headers()
                self.wfile.write(resp)

        srv = HTTPServer(("127.0.0.1", 0), MockDLNAHandler)
        port = srv.server_address[1]
        th = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        th.start()
        try:
            drv = samsung_tv.SamsungTVDriver(host="127.0.0.1", dlna_port=port)
            ok, msg = await drv.cast_url("http://192.168.1.50:18766/media/abc/test.mp4", title="Test Clip")
            self.assertTrue(ok, msg)
            ok_p, _ = await drv.cast_pause()
            self.assertTrue(ok_p)
            ok_s, _ = await drv.cast_stop()
            self.assertTrue(ok_s)

            self.assertEqual(len(received), 4)
            self.assertEqual([r[0] for r in received], ["/upnp/control/AVTransport1"] * 4)
            self.assertIn("#SetAVTransportURI\"", received[0][1])
            self.assertIn("#Play\"", received[1][1])
            self.assertIn("#Pause\"", received[2][1])
            self.assertIn("#Stop\"", received[3][1])
            for _, _, xml_body in received:
                ET.fromstring(xml_body)
        finally:
            srv.shutdown()
            srv.server_close()
            th.join(timeout=3)

    async def test_local_media_server_serving_range_traversal_and_cap(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            f1 = tmp_dir / "sample_video.mp4"
            payload1 = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            f1.write_bytes(payload1)
            f2 = tmp_dir / "audio.mp3"
            f2.write_bytes(b"ID3audio-bytes")
            f3 = tmp_dir / "photo.jpg"
            f3.write_bytes(b"\xff\xd8\xff\xe0jpeg-bytes")

            srv = media_server.LocalMediaServer(
                advertise_host="127.0.0.1",
                bind_host="127.0.0.1",
                port=0,
                max_entries=2,
            )
            port = await srv.start()
            self.assertTrue(srv.is_running)
            self.assertGreater(port, 0)
            try:
                tok1, url1, mime1 = srv.register_file(f1)
                self.assertEqual(mime1, "video/mp4")
                parsed = urlparse(url1)
                self.assertTrue(parsed.path.startswith(f"/media/{tok1}/"))

                def _req(method: str, path: str, headers: Optional[Dict[str, str]] = None):
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                    try:
                        conn.request(method, path, headers=headers or {})
                        resp = conn.getresponse()
                        body = resp.read()
                        return resp.status, {k.lower(): v for k, v in resp.getheaders()}, body
                    finally:
                        conn.close()

                # 1. Full GET
                st, hdrs, body = await asyncio.to_thread(_req, "GET", parsed.path)
                self.assertEqual(st, 200)
                self.assertEqual(body, payload1)
                self.assertEqual(hdrs.get("accept-ranges"), "bytes")
                self.assertIn("video/mp4", hdrs.get("content-type") or "")

                # 2. HEAD
                st_h, hdrs_h, body_h = await asyncio.to_thread(_req, "HEAD", parsed.path)
                self.assertEqual(st_h, 200)
                self.assertEqual(body_h, b"")
                self.assertEqual(int(hdrs_h.get("content-length") or "0"), len(payload1))

                # 3. Byte Range (206 Partial Content)
                st_r, hdrs_r, r_body = await asyncio.to_thread(_req, "GET", parsed.path, {"Range": "bytes=4-11"})
                self.assertEqual(st_r, 206)
                self.assertEqual(r_body, payload1[4:12])
                self.assertEqual(hdrs_r.get("content-range"), f"bytes 4-11/{len(payload1)}")

                # 4. Unsatisfiable Range (416)
                st_416, _, _ = await asyncio.to_thread(_req, "GET", parsed.path, {"Range": "bytes=9999-10000"})
                self.assertEqual(st_416, 416)

                # 5. Path traversal rejected (403)
                for evil_path in (
                    f"/media/{tok1}/..%2fetc%2fpasswd",
                    "/media/../etc/passwd",
                    f"/media/{tok1}/..%5csecret",
                ):
                    st_bad, _, _ = await asyncio.to_thread(_req, "GET", evil_path)
                    self.assertIn(st_bad, (400, 403), f"Expected traversal block for {evil_path}")

                # 6. Unknown token -> 404
                st_404, _, _ = await asyncio.to_thread(_req, "GET", "/media/deadbeefdeadbeef/nope.mp4")
                self.assertEqual(st_404, 404)

                # 7. Max entries cap evicts oldest (tok1 evicted after f2 + f3)
                srv.register_file(f2)
                srv.register_file(f3)
                self.assertEqual(srv.entry_count(), 2)
                st_evicted, _, _ = await asyncio.to_thread(_req, "GET", parsed.path)
                self.assertEqual(st_evicted, 404)
            finally:
                await srv.stop()
                self.assertFalse(srv.is_running)


# ---------------------------------------------------------------------------
# 5. Tablet Telemetry, VPN, Network & Alert Watcher Tests
# ---------------------------------------------------------------------------

class TabletNetworkAndAlertsTests(unittest.IsolatedAsyncioTestCase):
    def test_tablet_parsers(self):
        batt = tablet.parse_battery_info(
            {
                "capacity": "74",
                "status": "Discharging",
                "temp": "335",
                "current_now": "-1250000",
                "voltage_now": "4000000",
                "night_charging": "1",
            }
        )
        self.assertEqual(batt["capacity"], 74)
        self.assertEqual(batt["temp_c"], 33.5)
        self.assertEqual(batt["power_w"], -5.0)
        self.assertTrue(batt["night_charging"])

        mem = tablet.parse_meminfo("MemTotal:       8000000 kB\nMemAvailable:   6000000 kB\n")
        self.assertEqual(mem["ram_pct"], 25.0)

        t1 = tablet.parse_cpu_times("cpu  100 0 100 800 0 0 0 0\n")
        t2 = tablet.parse_cpu_times("cpu  150 0 150 900 0 0 0 0\n")
        self.assertEqual(tablet.compute_cpu_percent(t1, t2), 50.0)

        self.assertEqual(tablet.parse_uptime("12345.67 89012.34\n"), 12345.67)
        self.assertEqual(
            tablet.parse_thermal_zones([("battery", "31000"), ("cpu-0-0-usr", "58400"), ("cpu-1-0-usr", "61200")]),
            61.2,
        )

        profiles = tablet.parse_vpn_list("  - nl-ams\n* de-fra\nuk-lon\n  - nl-ams\n; bad cmd\n")
        self.assertEqual(profiles, ["nl-ams", "de-fra", "uk-lon"])
        self.assertTrue(tablet.validate_vpn_profile("nl-ams_01"))
        self.assertFalse(tablet.validate_vpn_profile("bad;id"))
        self.assertFalse(tablet.validate_vpn_profile("../etc"))

        short, active = tablet.parse_vpn_short_status("active: de-fra (wg0)", profiles)
        self.assertEqual(active, "de-fra")
        self.assertIn("de-fra", short)

        self.assertEqual(tablet.clamp_brightness_raw(0, 4095), 0)
        self.assertEqual(tablet.clamp_brightness_raw(100, 4095), 4095)
        self.assertEqual(tablet.clamp_brightness_raw(50, 4095), 2048)

        self.assertEqual(tablet.parse_busctl_int("i 65"), 65)
        self.assertIs(tablet.parse_busctl_bool("b true"), True)
        self.assertEqual(tablet.parse_pactl_volume("Volume: front-left: 45875 /  70% / -9.29 dB"), 70)
        self.assertIs(tablet.parse_pactl_mute("Mute: yes"), True)

    async def test_tablet_brightness_sysfs_read_write_and_screen_toggle(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp) / "4-0011"
            bdir.mkdir()
            (bdir / "max_brightness").write_text("4095\n", encoding="utf-8")
            (bdir / "brightness").write_text("2048\n", encoding="utf-8")

            drv = tablet.TabletDriver(backlight_path=bdir)
            pct, raw, max_raw = drv.read_brightness()
            self.assertEqual((pct, raw, max_raw), (50, 2048, 4095))

            ok, _ = await drv.set_brightness_pct(30)
            self.assertTrue(ok)
            self.assertEqual(int((bdir / "brightness").read_text(encoding="utf-8").strip()), 1228)

            ok_off, _ = await drv.screen_off()
            self.assertTrue(ok_off)
            self.assertEqual((bdir / "brightness").read_text(encoding="utf-8").strip(), "0")

            ok_on, _ = await drv.screen_on()
            self.assertTrue(ok_on)
            self.assertEqual(int((bdir / "brightness").read_text(encoding="utf-8").strip()), 1228)

    def test_network_parsers_and_annotation(self):
        neigh_out = (
            "192.168.1.1 dev wlp1s0 lladdr aa:bb:cc:dd:ee:01 REACHABLE\n"
            "192.168.1.100 dev wlp1s0 lladdr 00:11:22:33:44:55 STALE\n"
            "192.168.1.99 dev wlp1s0 FAILED\n"
        )
        devs = network.parse_ip_neigh(neigh_out)
        self.assertEqual(len(devs), 2)
        annotated = network.annotate_devices(devs, bot_config.BotConfig())
        self.assertIn("Home Router", annotated[0]["label"])
        self.assertIn("My PC", annotated[1]["label"])

        iw = network.parse_iw_link("Connected to 00:11\n\tSSID: MyWiFi5G\n\tsignal: -54 dBm\n\ttx bitrate: 866.7 MBit/s\n")
        self.assertEqual(iw["ssid"], "MyWiFi5G")
        self.assertEqual(iw["signal_dbm"], -54)

        self.assertEqual(network.parse_ping_output("rtt min/avg/max/mdev = 10.2/14.6/18.1/2.1 ms"), 14.6)

    def test_alert_watcher_thresholds_and_cooldown(self):
        acfg = bot_config.AlertsConfig(low_battery_pct=15, max_cpu_temp_c=65.0, max_battery_temp_c=42.0, cooldown_sec=600)
        watcher = helpers.AlertWatcher(acfg)

        # Charging at 10% should NOT trigger low battery
        msgs = watcher.evaluate({"battery": {"capacity": 10, "status": "Charging", "temp_c": 35.0}, "cpu_temp_c": 50.0}, now=100.0)
        self.assertEqual(msgs, [])

        # Discharging at 12% + hot CPU + hot battery triggers all 3
        msgs = watcher.evaluate({"battery": {"capacity": 12, "status": "Discharging", "temp_c": 44.5}, "cpu_temp_c": 70.0}, now=200.0)
        self.assertEqual(len(msgs), 3)

        # Within cooldown -> no spam
        msgs_again = watcher.evaluate({"battery": {"capacity": 11, "status": "Discharging", "temp_c": 45.0}, "cpu_temp_c": 72.0}, now=300.0)
        self.assertEqual(msgs_again, [])

        # After cooldown -> alerts fire again
        msgs_later = watcher.evaluate({"battery": {"capacity": 10, "status": "Discharging", "temp_c": 45.0}, "cpu_temp_c": 72.0}, now=900.0)
        self.assertEqual(len(msgs_later), 3)

    def test_helpers_filename_and_open_parsing(self):
        self.assertEqual(helpers.sanitize_filename("../../evil/file?.txt"), "file_.txt")
        self.assertEqual(helpers.parse_open_command("pc https://ya.ru"), ("pc", "https://ya.ru"))
        self.assertEqual(helpers.parse_open_command("https://ya.ru"), ("tablet", "https://ya.ru"))
        self.assertEqual(helpers.parse_open_command("tv https://youtu.be/abc"), ("tv", "https://youtu.be/abc"))
        short, needs_file = helpers.truncate_shell_output("a" * 5000, limit=3800)
        self.assertTrue(needs_file)
        self.assertEqual(len(short), 3800)

    async def test_terminal_cwd_tracking_and_night_charging(self):
        marker = "__PIPA_CWD_test123__"
        wrapped = tablet.build_cwd_tracked_command("cd /var/log && ls", marker)
        self.assertIn("__pipa_rc=$?", wrapped)
        self.assertIn(marker, wrapped)
        self.assertIn("exit $__pipa_rc", wrapped)

        # Normal command output + new directory
        out1, cwd1 = tablet.parse_cwd_tracked_output(f"syslog\nauth.log\n\n{marker}/var/log\n", marker, "/home/pipa")
        self.assertEqual(out1, "syslog\nauth.log\n")
        self.assertEqual(cwd1, "/var/log")

        # Empty output (e.g. plain `cd /tmp`)
        out2, cwd2 = tablet.parse_cwd_tracked_output(f"\n{marker}/tmp\n", marker, "/home/pipa")
        self.assertEqual(out2, "")
        self.assertEqual(cwd2, "/tmp")

        # Output without trailing newline
        out3, cwd3 = tablet.parse_cwd_tracked_output(f"hello\n{marker}/home/pipa\n", marker, "/tmp")
        self.assertEqual(out3, "hello")
        self.assertEqual(cwd3, "/home/pipa")

        # Missing marker (e.g. user ran `exit 1`) keeps fallback cwd
        out4, cwd4 = tablet.parse_cwd_tracked_output("bye\n", marker, "/home/pipa/work")
        self.assertEqual(out4, "bye\n")
        self.assertEqual(cwd4, "/home/pipa/work")

        # Battery night_charging sysfs toggle
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp) / "battery"
            bdir.mkdir()
            nc_file = bdir / "night_charging"
            nc_file.write_text("0\n", encoding="utf-8")
            drv = tablet.TabletDriver(battery_path=bdir)
            ok_on, _ = await drv.set_night_charging(True)
            self.assertTrue(ok_on)
            self.assertEqual(nc_file.read_text(encoding="utf-8").strip(), "1")
            ok_off, _ = await drv.set_night_charging(False)
            self.assertTrue(ok_off)
            self.assertEqual(nc_file.read_text(encoding="utf-8").strip(), "0")


# ---------------------------------------------------------------------------
# 6. Windows PC Local Agent (pc_agent.py) + Live PCDriver Integration Tests
# ---------------------------------------------------------------------------

class PCAgentAndPCDriverTests(unittest.IsolatedAsyncioTestCase):
    def test_lan_ip_guard(self):
        for ok_ip in ("127.0.0.1", "::1", "192.168.1.50", "10.0.0.5", "172.16.10.2", "::ffff:192.168.1.50"):
            self.assertTrue(pc_agent.is_allowed_client_ip(ok_ip), f"Should allow {ok_ip}")
        for bad_ip in ("8.8.8.8", "1.1.1.1", "172.32.0.1", "100.64.0.1", "2001:4860:4860::8888", "", "not-an-ip"):
            self.assertFalse(pc_agent.is_allowed_client_ip(bad_ip), f"Should reject {bad_ip}")

    def test_token_auto_generation_and_reload(self):
        with tempfile.TemporaryDirectory() as tmp:
            tok_path = Path(tmp) / "token.txt"
            tok1, path1, created1 = pc_agent.ensure_token(tok_path, env_token="")
            self.assertTrue(created1)
            self.assertEqual(path1, tok_path)
            self.assertEqual(len(tok1), 64)

            tok2, _, created2 = pc_agent.ensure_token(tok_path, env_token="")
            self.assertFalse(created2)
            self.assertEqual(tok1, tok2)

    async def test_live_http_endpoints_auth_lan_guard_and_pc_driver(self):
        token = "test-secret-token-0123456789abcdef"
        fake_exec = FakePCExecutor()
        server = pc_agent.create_server(host="127.0.0.1", port=0, token=token, executor=fake_exec)
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        thread.start()
        try:
            # 1. Missing/wrong token -> 401
            bad_drv = pc_driver.PCDriver(host="127.0.0.1", port=port, token="wrong-token")
            self.assertFalse(await bad_drv.is_online())
            ok_st, _, err_st = await bad_drv.get_status()
            self.assertFalse(ok_st)
            self.assertIn("авторизации", err_st)

            # 2. Valid token -> health, status, screenshot, actions all work end-to-end
            good_drv = pc_driver.PCDriver(host="127.0.0.1", port=port, token=token, mac="00:11:22:33:44:55")
            self.assertTrue(await good_drv.is_online())

            ok, status, err = await good_drv.get_status()
            self.assertTrue(ok, err)
            self.assertEqual(status["hostname"], "mypc")
            self.assertEqual(status["cpu_pct"], 18.5)
            self.assertEqual(status["active_window"], "Visual Studio Code")

            png, caption = await good_drv.take_screenshot()
            self.assertEqual(png, FAKE_PNG)
            self.assertIn("Скриншот", caption)

            ok_act, _, data_act = await good_drv.action("vol_up")
            self.assertTrue(ok_act)
            self.assertEqual(data_act["action"], "vol_up")

            ok_url, _, _ = await good_drv.action("open_url", url="https://example.com")
            self.assertTrue(ok_url)

            # Invalid URL scheme rejected with 400
            ok_bad_url, _, _ = await good_drv.action("open_url", url="file:///C:/Windows/win.ini")
            self.assertFalse(ok_bad_url)

            # Unknown action rejected with 400
            ok_unk, _, _ = await good_drv.action("format_c_drive")
            self.assertFalse(ok_unk)

            # Mute alias accepted for Macro Scenes
            ok_mute, _, data_mute = await good_drv.action("mute")
            self.assertTrue(ok_mute)
            self.assertEqual(data_mute["action"], "mute")

            # 2b. Clipboard GET/POST endpoints (auth, UTF-8 Russian, shell metacharacters, empty, invalid)
            ok_bad_cl, _, _ = await bad_drv.get_clipboard()
            self.assertFalse(ok_bad_cl)
            ok_bad_set, _ = await bad_drv.set_clipboard("nope")
            self.assertFalse(ok_bad_set)

            ok_cl0, txt0, err0 = await good_drv.get_clipboard()
            self.assertTrue(ok_cl0, err0)
            self.assertEqual(txt0, "")

            tricky_text = "Привет, мир! $(whoami); `id` \"' & <>\nСтрока 2 🎬"
            ok_set, msg_set = await good_drv.set_clipboard(tricky_text)
            self.assertTrue(ok_set, msg_set)
            self.assertEqual(fake_exec.clipboard_text, tricky_text)

            ok_cl1, txt1, err1 = await good_drv.get_clipboard()
            self.assertTrue(ok_cl1, err1)
            self.assertEqual(txt1, tricky_text)

            # Empty string clipboard clear
            ok_empty, _ = await good_drv.set_clipboard("")
            self.assertTrue(ok_empty)
            ok_cl2, txt2, _ = await good_drv.get_clipboard()
            self.assertTrue(ok_cl2)
            self.assertEqual(txt2, "")

            # Invalid POST /clipboard JSON body -> 400
            conn_bad = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                body_bad = json.dumps({"text": 12345}).encode("utf-8")
                conn_bad.request(
                    "POST",
                    "/clipboard",
                    body=body_bad,
                    headers={"X-Pipa-Token": token, "Content-Type": "application/json", "Content-Length": str(len(body_bad))},
                )
                resp_bad = conn_bad.getresponse()
                resp_bad.read()
                self.assertEqual(resp_bad.status, 400)
            finally:
                conn_bad.close()

            # 3. Non-LAN IP validator rejection -> 403 Forbidden (health & clipboard)
            server.ip_validator = lambda _ip: False
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                conn.request("GET", "/health", headers={"X-Pipa-Token": token})
                resp = conn.getresponse()
                resp.read()
                self.assertEqual(resp.status, 403)

                conn.request("GET", "/clipboard", headers={"X-Pipa-Token": token})
                resp_cl = conn.getresponse()
                resp_cl.read()
                self.assertEqual(resp_cl.status, 403)

                body_ok = json.dumps({"text": "blocked"}).encode("utf-8")
                conn.request(
                    "POST",
                    "/clipboard",
                    body=body_ok,
                    headers={"X-Pipa-Token": token, "Content-Type": "application/json", "Content-Length": str(len(body_ok))},
                )
                resp_clp = conn.getresponse()
                resp_clp.read()
                self.assertEqual(resp_clp.status, 403)
            finally:
                conn.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

    def test_all_python_sources_have_valid_syntax(self):
        py_files = list(TG_BOT_DIR.rglob("*.py")) + list(PC_AGENT_DIR.rglob("*.py"))
        self.assertGreaterEqual(len(py_files), 12)
        for path in py_files:
            source = path.read_text(encoding="utf-8")
            ast.parse(source, filename=str(path))

    def test_linux_packaging_files_use_lf_line_endings(self):
        for rel in (
            "install.sh",
            "pipa-tg-bot.service",
            "requirements.txt",
            "bot.py",
            "commands.py",
            "ui.py",
            "drivers/tablet.py",
            "drivers/media_server.py",
        ):
            raw = (TG_BOT_DIR / rel).read_bytes()
            self.assertNotIn(b"\r\n", raw, f"{rel} must use LF line endings for Linux")


# ---------------------------------------------------------------------------
# 7. Wave 6 Track A: Native Camera App (naomi-camera) & /cam Bot Tests
# ---------------------------------------------------------------------------

def _check_balanced_qml_delimiters(source: str) -> None:
    """Verify balanced {}, (), [] in QML while skipping strings and JS comments."""
    stack: List[tuple[str, int]] = []
    pairs = {"}": "{", ")": "(", "]": "["}
    i = 0
    n = len(source)
    line = 1
    while i < n:
        ch = source[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n and source[i + 1] == "/":
            i += 2
            while i < n and source[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and source[i + 1] == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                if source[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n:
                if source[i] == "\\":
                    i += 2
                    continue
                if source[i] == "\n":
                    line += 1
                if source[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch in "{([":
            stack.append((ch, line))
        elif ch in "})]":
            if not stack or stack[-1][0] != pairs[ch]:
                raise AssertionError(f"Unbalanced '{ch}' at line {line} (stack={stack[-3:]})")
            stack.pop()
        i += 1
    if stack:
        raise AssertionError(f"Unclosed QML delimiters: {stack}")


class CameraAppAndBotTests(unittest.IsolatedAsyncioTestCase):
    def test_camera_cli_command_builders_and_validation(self):
        cmd_back = tablet.build_camera_snap_cmd("/tmp/shot.jpg", facing="back", zoom=35, flash="on", width=1920, height=1080)
        self.assertEqual(
            cmd_back,
            [
                tablet.CAMERA_BIN,
                "--snap",
                "back",
                "--out",
                "/tmp/shot.jpg",
                "--zoom",
                "35",
                "--flash",
                "on",
                "--width",
                "1920",
                "--height",
                "1080",
            ],
        )

        # Front camera forces flash off and clamps zoom
        cmd_front = tablet.build_camera_snap_cmd("/tmp/selfie.jpg", facing="front", zoom=250, flash="on")
        self.assertIn("front", cmd_front)
        self.assertEqual(cmd_front[cmd_front.index("--zoom") + 1], "99")
        self.assertEqual(cmd_front[cmd_front.index("--flash") + 1], "off")

        with self.assertRaises(ValueError):
            tablet.build_camera_snap_cmd("/tmp/bad.jpg", facing="side")
        with self.assertRaises(ValueError):
            tablet.build_camera_snap_cmd("/tmp/bad.jpg", facing="back", flash="strobe")

        self.assertEqual(
            tablet.build_camera_torch_cmd(600),
            [tablet.CAMERA_BIN, "--torch", "600"],
        )
        self.assertEqual(
            tablet.build_camera_torch_cmd(99999),
            [tablet.CAMERA_BIN, "--torch", "3600"],
        )

    async def test_tablet_driver_snap_camera_and_torch_pid_handling(self):
        with tempfile.TemporaryDirectory() as tmp:
            pid_file = Path(tmp) / "torch.pid"
            drv = tablet.TabletDriver(camera_bin="/fake/naomi-camera", torch_pid_path=pid_file)

            # Stale/dead PID file is cleaned up by is_torch_on()
            pid_file.write_text("99999999\n", encoding="utf-8")
            drv._is_pid_running = lambda _pid: False  # type: ignore[method-assign]
            self.assertFalse(drv.is_torch_on())
            self.assertFalse(pid_file.exists())

            # set_torch(False) cleanly returns ok even when already off
            ok_off, msg_off = await drv.set_torch(False)
            self.assertTrue(ok_off)
            self.assertIn("выключ", msg_off.lower())

            # snap_camera succeeds when CLI writes valid JPEG
            recorded_cmds: List[List[str]] = []

            async def fake_run_cmd(args, timeout=15.0, env=None, stdin_bytes=None):
                recorded_cmds.append(list(args))
                out_idx = args.index("--out") + 1
                out_p = Path(args[out_idx])
                out_p.write_bytes(tablet.JPEG_MAGIC + b"\xe0" + b"\x00" * 256)
                return 0, str(out_p) + "\n", ""

            drv._run_cmd = fake_run_cmd  # type: ignore[method-assign]
            ok_snap, shot_path = await drv.snap_camera(facing="back", zoom=35, flash="on")
            self.assertTrue(ok_snap, shot_path)
            p = Path(shot_path)
            self.assertTrue(p.is_file())
            p.unlink(missing_ok=True)
            self.assertEqual(recorded_cmds[0][:3], ["/fake/naomi-camera", "--snap", "back"])
            self.assertIn("35", recorded_cmds[0])
            self.assertIn("on", recorded_cmds[0])

            # snap_camera fails cleanly and cleans up invalid output file
            async def fake_run_fail(args, timeout=15.0, env=None, stdin_bytes=None):
                out_idx = args.index("--out") + 1
                Path(args[out_idx]).write_bytes(b"not-a-jpeg")
                return 1, "", "camera busy"

            drv._run_cmd = fake_run_fail  # type: ignore[method-assign]
            ok_fail, err_fail = await drv.snap_camera(facing="front")
            self.assertFalse(ok_fail)
            self.assertIn("camera busy", err_fail)

    def test_native_camera_app_sources_qml_and_wrapper(self):
        cam_dir = ROOT / "shell" / "apps" / "camera"
        required_files = [
            cam_dir / "CMakeLists.txt",
            cam_dir / "hybris_camera.h",
            cam_dir / "camera_engine.h",
            cam_dir / "camera_engine.cpp",
            cam_dir / "camera_viewfinder.h",
            cam_dir / "camera_viewfinder.cpp",
            cam_dir / "main.cpp",
            cam_dir / "qml" / "Main.qml",
        ]
        for path in required_files:
            self.assertTrue(path.is_file(), f"Missing camera source file: {path}")

        shell_cmake = (ROOT / "shell" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_subdirectory(apps/camera)", shell_cmake)

        # Verify QML syntax delimiters are balanced and expected UI elements are present
        qml_src = (cam_dir / "qml" / "Main.qml").read_text(encoding="utf-8")
        _check_balanced_qml_delimiters(qml_src)
        self.assertIn("CameraViewfinder", qml_src)
        self.assertIn("REC ", qml_src)
        self.assertIn("#d8b25a", qml_src)
        self.assertIn("#0e0f0c", qml_src)
        self.assertIn("#d9705f", qml_src)

        # Verify libcamera dlopen usage and ensure unexported symbol is never called
        hybris_src = (cam_dir / "hybris_camera.h").read_text(encoding="utf-8")
        self.assertIn('dlopen("libcamera.so.1"', hybris_src)
        for cpp_path in (cam_dir / "hybris_camera.h", cam_dir / "camera_engine.cpp", cam_dir / "main.cpp"):
            code_lines = [
                ln for ln in cpp_path.read_text(encoding="utf-8").splitlines()
                if not ln.lstrip().startswith("//")
            ]
            self.assertNotIn("android_camera_get_preview_format", "\n".join(code_lines))

        # Verify device/naomi-camera wrapper uses LF endings and handles headless + Wayland modes
        wrapper = ROOT / "device" / "naomi-camera"
        self.assertTrue(wrapper.is_file())
        raw_wrapper = wrapper.read_bytes()
        self.assertNotIn(b"\r\n", raw_wrapper, "device/naomi-camera must use LF line endings")
        wrapper_txt = raw_wrapper.decode("utf-8")
        self.assertTrue(wrapper_txt.startswith("#!/bin/sh\n"))
        self.assertIn("--snap", wrapper_txt)
        self.assertIn("--torch", wrapper_txt)
        self.assertIn("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=none", wrapper_txt)
        self.assertIn("WAYLAND_DISPLAY", wrapper_txt)


if __name__ == "__main__":
    unittest.main(verbosity=2)
