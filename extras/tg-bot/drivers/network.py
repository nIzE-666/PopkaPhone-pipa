"""Home LAN discovery, Wi-Fi telemetry, and internet ping diagnostics."""

from __future__ import annotations

import asyncio
import ipaddress
import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from ..config import BotConfig
except ImportError:
    from config import BotConfig


IP_NEIGH_RE = re.compile(
    r"^(?P<ip>\d+\.\d+\.\d+\.\d+)\s+dev\s+(?P<dev>\S+)"
    r"(?:\s+lladdr\s+(?P<mac>[0-9a-fA-F:]{17}))?"
    r".*?\s+(?P<state>REACHABLE|STALE|DELAY|PROBE|PERMANENT|FAILED|INCOMPLETE)\s*$"
)


def parse_ip_neigh(output: str) -> List[Dict[str, Any]]:
    """Parse `ip -4 neigh show` output into structured LAN neighbor entries."""
    devices: List[Dict[str, Any]] = []
    seen = set()
    for line in output.splitlines():
        m = IP_NEIGH_RE.match(line.strip())
        if not m:
            continue
        state = m.group("state")
        if state in ("FAILED", "INCOMPLETE"):
            continue
        ip = m.group("ip")
        if ip in seen:
            continue
        seen.add(ip)
        devices.append(
            {
                "ip": ip,
                "dev": m.group("dev"),
                "mac": (m.group("mac") or "").lower(),
                "state": state,
                "label": "",
            }
        )
    return devices


def parse_iw_link(output: str) -> Dict[str, Any]:
    """Parse `iw dev <iface> link` output for SSID, signal dBm, and TX bitrate."""
    info: Dict[str, Any] = {"ssid": None, "signal_dbm": None, "tx_bitrate": None}
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("SSID:"):
            info["ssid"] = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("signal:"):
            m = re.search(r"(-?\d+)\s*dBm", stripped)
            if m:
                info["signal_dbm"] = int(m.group(1))
        elif stripped.startswith("tx bitrate:"):
            info["tx_bitrate"] = stripped.split(":", 1)[1].strip()
    return info


def parse_ping_output(output: str) -> Optional[float]:
    """Extract average or single RTT in milliseconds from ping output."""
    m_avg = re.search(r"=\s*[\d.]+/([\d.]+)/[\d.]+", output or "")
    if m_avg:
        return round(float(m_avg.group(1)), 1)
    m_single = re.search(r"time[=<]([\d.]+)\s*ms", output or "", re.IGNORECASE)
    if m_single:
        return round(float(m_single.group(1)), 1)
    return None


def annotate_devices(devices: Sequence[Dict[str, Any]], config: BotConfig) -> List[Dict[str, Any]]:
    """Attach human-readable Russian labels to known LAN IPs/MACs and sort by IP."""
    ip_labels: Dict[str, str] = {
        config.network.router_ip: f"Роутер ({config.network.router_name})",
        config.network.tablet_ip: "Xiaomi Pad 6 (pipa)",
        config.samsung_tv.host: config.samsung_tv.name,
        config.android_tv.host: config.android_tv.name,
    }
    mac_labels: Dict[str, str] = {}
    if config.samsung_tv.mac:
        mac_labels[config.samsung_tv.mac.lower()] = config.samsung_tv.name
    for pc in config.pcs:
        if pc.host:
            ip_labels[pc.host] = pc.name
        if pc.mac:
            mac_labels[pc.mac.lower()] = pc.name

    enriched: List[Dict[str, Any]] = []
    for dev in devices:
        copy = dict(dev)
        ip = copy.get("ip", "")
        mac = (copy.get("mac") or "").lower()
        copy["label"] = copy.get("label") or ip_labels.get(ip) or mac_labels.get(mac) or ""
        enriched.append(copy)

    def _ip_key(item: Dict[str, Any]) -> Tuple[int, str]:
        try:
            return int(ipaddress.IPv4Address(item["ip"])), item["ip"]
        except ValueError:
            return (1 << 32), str(item.get("ip", ""))

    enriched.sort(key=_ip_key)
    return enriched


class NetworkDriver:
    """Async home network & Wi-Fi scanner running on the tablet."""

    def __init__(self, config: BotConfig) -> None:
        self.config = config

    async def _run(self, args: Sequence[str], timeout: float = 6.0) -> Tuple[int, str]:
        try:
            proc = await asyncio.create_subprocess_exec(
                *args,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
            try:
                out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.communicate()
                return 124, ""
            return proc.returncode if proc.returncode is not None else 1, out.decode("utf-8", errors="replace")
        except OSError:
            return 1, ""

    def _wifi_candidates(self) -> List[str]:
        candidates: List[str] = []
        net_dir = Path("/sys/class/net")
        if net_dir.is_dir():
            found: List[Tuple[int, str]] = []
            for p in sorted(net_dir.iterdir()):
                if p.name.startswith("p2p-"):
                    continue
                if (p / "wireless").is_dir():
                    state = ""
                    try:
                        state = (p / "operstate").read_text(encoding="utf-8", errors="replace").strip()
                    except OSError:
                        pass
                    found.append((0 if state == "up" else 1, p.name))
            found.sort()
            candidates.extend(name for _, name in found)
        for fallback in (self.config.network.wifi_interface, "wlan0", "wlp1s0"):
            if fallback and fallback not in candidates:
                candidates.append(fallback)
        return candidates

    async def get_wifi_info(self) -> Dict[str, Any]:
        candidates = self._wifi_candidates()
        iface = candidates[0]
        info: Dict[str, Any] = {"ssid": None, "signal_dbm": None, "tx_bitrate": None}
        for cand in candidates:
            rc_iw, out_iw = await self._run(["iw", "dev", cand, "link"], timeout=4.0)
            if rc_iw == 0:
                parsed = parse_iw_link(out_iw)
                if parsed.get("ssid"):
                    iface = cand
                    info = parsed
                    break
        rc_ip, out_ip = await self._run(["ip", "-4", "-o", "addr", "show", "dev", iface], timeout=4.0)
        ip_addr = None
        if rc_ip == 0:
            m = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)", out_ip)
            if m:
                ip_addr = m.group(1)
        info["ip"] = ip_addr or self.config.network.tablet_ip
        info["interface"] = iface
        return info

    async def ping_internet(self, host: str = "1.1.1.1") -> Optional[float]:
        rc, out = await self._run(["ping", "-c", "1", "-W", "2", host], timeout=4.0)
        if rc == 0:
            return parse_ping_output(out)
        rc2, out2 = await self._run(["ping", "-c", "1", "-W", "2", "8.8.8.8"], timeout=4.0)
        return parse_ping_output(out2) if rc2 == 0 else None

    async def _ping_one(self, ip: str, sem: asyncio.Semaphore) -> bool:
        async with sem:
            rc, _ = await self._run(["ping", "-c", "1", "-W", "1", ip], timeout=1.6)
            return rc == 0

    async def scan_lan(self, deep_sweep: bool = False) -> List[Dict[str, Any]]:
        """Discover active devices in the local subnet."""
        prefix = self.config.network.subnet_prefix
        sem = asyncio.Semaphore(48)

        # Always ping key known devices so ARP table is fresh
        known_ips = {
            self.config.network.router_ip,
            self.config.samsung_tv.host,
            self.config.android_tv.host,
            *(pc.host for pc in self.config.pcs if pc.host),
        }
        targets = set(known_ips)
        if deep_sweep and prefix.endswith("."):
            targets.update(f"{prefix}{i}" for i in range(1, 255))

        alive_set = set()
        results = await asyncio.gather(*(self._ping_one(ip, sem) for ip in targets))
        for ip, ok in zip(targets, results):
            if ok:
                alive_set.add(ip)

        rc, out = await self._run(["ip", "-4", "neigh", "show"], timeout=4.0)
        devices = parse_ip_neigh(out) if rc == 0 else []
        by_ip = {d["ip"]: d for d in devices}

        # Mark freshly pinged devices as ONLINE and include self
        for ip in alive_set:
            if ip in by_ip:
                by_ip[ip]["state"] = "ONLINE"
            else:
                by_ip[ip] = {"ip": ip, "dev": self.config.network.wifi_interface, "mac": "", "state": "ONLINE", "label": ""}

        self_ip = self.config.network.tablet_ip
        if self_ip and self_ip not in by_ip:
            by_ip[self_ip] = {
                "ip": self_ip,
                "dev": self.config.network.wifi_interface,
                "mac": "",
                "state": "ONLINE",
                "label": "",
            }

        return annotate_devices(list(by_ip.values()), self.config)
