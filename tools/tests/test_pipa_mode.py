"""Unit tests for device/pipa-mode (Home/Away/Auto travel security) and device/pipa-vpn (DNS-leak-free TUN)."""

from __future__ import annotations

import asyncio
import importlib.machinery
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch


ROOT = Path(__file__).resolve().parents[2]
TG_BOT_DIR = ROOT / "extras" / "tg-bot"
if str(TG_BOT_DIR) not in sys.path:
    sys.path.insert(0, str(TG_BOT_DIR))

from drivers.tablet import TabletDriver  # noqa: E402


def _load_script(name: str, rel_path: str):
    path = ROOT / rel_path
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    assert spec is not None
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


pipa_mode = _load_script("pipa_mode_script", "device/pipa-mode")
pipa_vpn = _load_script("pipa_vpn_script", "device/pipa-vpn")


class PipaModeTests(unittest.TestCase):
    def test_default_config_is_sanitized(self):
        self.assertEqual(pipa_mode.DEFAULT_CFG["home_ssids"], [])
        self.assertEqual(pipa_mode.DEFAULT_CFG["home_subnets"], ["192.168.1.0/24"])
        self.assertEqual(pipa_mode.DEFAULT_CFG["default_vpn_profile"], "")

    def test_resolve_mode_manual_overrides_ssid(self):
        self.assertEqual(pipa_mode.resolve_mode("home", "AirportFreeWiFi", ["MyHome"]), ("home", "вручную"))
        self.assertEqual(pipa_mode.resolve_mode("away", "MyHome", ["MyHome"]), ("away", "вручную"))

    def test_resolve_mode_auto_matches_home_ssids_only(self):
        eff, reason = pipa_mode.resolve_mode("auto", "MyHome5G", ["MyHome5G", "MyHome2G"])
        self.assertEqual(eff, "home")
        self.assertIn("MyHome5G", reason)

        eff, reason = pipa_mode.resolve_mode("auto", "CafeGuest", ["MyHome5G"])
        self.assertEqual(eff, "away")
        self.assertIn("CafeGuest", reason)

        eff, reason = pipa_mode.resolve_mode("auto", "", ["MyHome5G"])
        self.assertEqual(eff, "away")
        self.assertIn("нет Wi-Fi", reason)

    def test_load_cfg_and_mode_file_fallbacks(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_file = Path(tmp) / "config.json"
            cfg_file.write_text(
                json.dumps({"home_ssids": ["OfficeWiFi"], "home_subnets": ["10.0.0.0/24"]}),
                encoding="utf-8",
            )
            cfg = pipa_mode.load_cfg(str(cfg_file))
            self.assertEqual(cfg["home_ssids"], ["OfficeWiFi"])
            self.assertEqual(cfg["home_subnets"], ["10.0.0.0/24"])
            self.assertTrue(cfg["away_auto_vpn"])

            mode_file = Path(tmp) / "mode"
            self.assertEqual(pipa_mode.read_configured_mode(str(mode_file)), "auto")
            mode_file.write_text("away\n", encoding="utf-8")
            self.assertEqual(pipa_mode.read_configured_mode(str(mode_file)), "away")
            mode_file.write_text("invalid\n", encoding="utf-8")
            self.assertEqual(pipa_mode.read_configured_mode(str(mode_file)), "auto")

    def test_firewall_description_formats_dynamic_subnets(self):
        self.assertEqual(pipa_mode.format_firewall_description(False, "home", ["10.1.0.0/24"]), "выкл")
        desc = pipa_mode.format_firewall_description(True, "home", ["10.1.0.0/24", "192.168.10.0/24"])
        self.assertIn("10.1.0.0/24, 192.168.10.0/24", desc)
        away_desc = pipa_mode.format_firewall_description(True, "away", ["10.1.0.0/24"])
        self.assertIn("все порты Wi-Fi закрыты", away_desc)
        self.assertNotIn("10.1.0.0/24", away_desc)


class PipaVpnTests(unittest.TestCase):
    def test_get_server_domains_skips_ip_literals_and_deduplicates(self):
        hosts = ["1.2.3.4", "vpn.example.com", "2001:db8::1", "vpn.example.com", "edge.example.org"]
        self.assertEqual(pipa_vpn.get_server_domains(hosts), ["vpn.example.com", "edge.example.org"])

    def test_build_tun_config_prevents_dns_leaks_and_preserves_ntp(self):
        cfg = pipa_vpn.build_tun_config(
            socks_port=10808,
            bypass_ips=["203.0.113.10"],
            server_domains=["vpn.example.com"],
            bootstrap_dns="192.168.1.1",
        )
        # DoH via proxy + LAN bootstrap only for VPN server domain
        dns_servers = {s["tag"]: s for s in cfg["dns"]["servers"]}
        self.assertEqual(dns_servers["remote"]["type"], "https")
        self.assertEqual(dns_servers["remote"]["detour"], "proxy")
        self.assertEqual(dns_servers["bootstrap"]["server"], "192.168.1.1")
        self.assertEqual(cfg["dns"]["final"], "remote")
        self.assertEqual(cfg["dns"]["rules"], [{"domain": ["vpn.example.com"], "server": "bootstrap"}])

        # Local DNS listener + TUN with RFC1918 & VPN server IP exclusions
        inbounds = {i["tag"]: i for i in cfg["inbounds"]}
        self.assertEqual(inbounds["dns-in"]["listen_port"], pipa_vpn.DNS_PORT)
        self.assertEqual(inbounds["tun-in"]["interface_name"], "popka-tun0")
        self.assertIn("192.168.0.0/16", inbounds["tun-in"]["route_exclude_address"])
        self.assertIn("203.0.113.10/32", inbounds["tun-in"]["route_exclude_address"])

        # NTP UDP/123 direct bypass + hijack-dns + private IP bypass
        route_rules = cfg["route"]["rules"]
        self.assertIn({"port": 123, "network": "udp", "outbound": "direct"}, route_rules)
        self.assertIn({"ip_is_private": True, "outbound": "direct"}, route_rules)
        self.assertIn({"action": "hijack-dns", "protocol": "dns"}, route_rules)
        self.assertEqual(cfg["route"]["final"], "proxy")


class TabletTravelModeDriverTests(unittest.TestCase):
    def test_get_and_set_travel_mode_parses_json_and_validates_input(self):
        drv = TabletDriver()
        sample = {"configured": "auto", "active": "away", "ssid": "CafeWiFi", "vpn": "Вкл (TUN)", "firewall": "активен"}
        with patch.object(drv, "_run_cmd", new=AsyncMock(return_value=(0, json.dumps(sample), ""))):
            info = asyncio.run(drv.get_travel_mode())
        self.assertEqual(info["active"], "away")
        self.assertEqual(info["ssid"], "CafeWiFi")

        ok, msg = asyncio.run(drv.set_travel_mode("bogus"))
        self.assertFalse(ok)
        self.assertIn("Неизвестный режим", msg)

        with patch.object(drv, "_run_cmd", new=AsyncMock(return_value=(0, "Режим: Дома (вручную)", ""))) as mocked:
            ok, msg = asyncio.run(drv.set_travel_mode("home"))
        self.assertTrue(ok)
        self.assertIn("Дома", msg)
        mocked.assert_awaited_once_with(["sudo", "-n", "/usr/local/bin/pipa-mode", "home"], timeout=25.0)


if __name__ == "__main__":
    unittest.main()
