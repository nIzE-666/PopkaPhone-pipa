"""Local tablet (Xiaomi Pad 6 / pipa) telemetry and control driver."""

from __future__ import annotations

import asyncio
import json
import os
import re
import secrets
import signal
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


BACKLIGHT_PREFERRED = Path("/sys/class/backlight/4-0011")
BACKLIGHT_ROOT = Path("/sys/class/backlight")
BATTERY_PATH = Path("/sys/class/power_supply/battery")
THERMAL_ROOT = Path("/sys/class/thermal")
VPN_BIN = "/usr/local/bin/vpn"
BOOT_ANDROID_BIN = "/usr/local/bin/boot-android"
NAOMI_SHOT_PATH = Path("/tmp/naomi-shot.png")
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
JPEG_MAGIC = b"\xff\xd8\xff"
CAMERA_BIN = "/home/phablet/naomi/build/apps/camera/naomi-camera"
CAMERA_WRAPPER_BIN = "/usr/local/lib/naomi/naomi-camera"
TORCH_PID_PATH = Path("~/.config/pipa-bot/torch.pid")

VPN_PROFILE_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$")
VPN_LIST_LINE_RE = re.compile(r"^\s*(?:[-*•]\s+)?([a-zA-Z0-9][a-zA-Z0-9_.-]{0,63})\s*$")


@dataclass(frozen=True)
class ServiceSpec:
    key: str
    unit: str
    label: str
    user: bool = False
    critical: bool = False


MANAGED_SERVICES: Tuple[ServiceSpec, ...] = (
    ServiceSpec(key="waydroid", unit="waydroid-container.service", label="Waydroid", user=False, critical=False),
    ServiceSpec(key="dsh", unit="dsh-web.service", label="DeepSeek Web", user=True, critical=False),
    ServiceSpec(key="dc", unit="desktop-commander.service", label="Desktop Commander", user=True, critical=False),
    ServiceSpec(key="shell", unit="naomi-shell.service", label="naomi-shell", user=False, critical=True),
    ServiceSpec(key="naomid", unit="naomid.service", label="naomid", user=False, critical=True),
)

SERVICES_BY_KEY: Dict[str, ServiceSpec] = {s.key: s for s in MANAGED_SERVICES}


# ---------------------------------------------------------------------------
# Pure Parsers (platform-independent & unit-testable)
# ---------------------------------------------------------------------------

def _read_text_safe(path: Path) -> Optional[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def parse_battery_info(raw: Dict[str, Optional[str]]) -> Dict[str, Any]:
    """Parse sysfs battery values into normalized metrics."""
    info: Dict[str, Any] = {
        "capacity": None,
        "status": raw.get("status") or "Unknown",
        "temp_c": None,
        "current_ma": None,
        "voltage_v": None,
        "power_w": None,
        "night_charging": None,
    }
    if raw.get("capacity") is not None:
        try:
            info["capacity"] = max(0, min(100, int(raw["capacity"])))
        except ValueError:
            pass
    if raw.get("temp") is not None:
        try:
            # Linux power_supply temp is in tenths of degree Celsius (e.g. 315 -> 31.5 C)
            raw_t = float(raw["temp"])
            info["temp_c"] = round(raw_t / 10.0 if abs(raw_t) > 120 else raw_t, 1)
        except ValueError:
            pass

    curr_ua: Optional[float] = None
    volt_uv: Optional[float] = None
    if raw.get("current_now") is not None:
        try:
            curr_ua = float(raw["current_now"])
            info["current_ma"] = round(curr_ua / 1000.0, 1)
        except ValueError:
            pass
    if raw.get("voltage_now") is not None:
        try:
            volt_uv = float(raw["voltage_now"])
            info["voltage_v"] = round(volt_uv / 1_000_000.0, 2)
        except ValueError:
            pass
    if curr_ua is not None and volt_uv is not None:
        info["power_w"] = round((curr_ua * volt_uv) / 1e12, 2)

    if raw.get("night_charging") is not None:
        val = str(raw["night_charging"]).strip().lower()
        info["night_charging"] = val in ("1", "true", "yes", "on")

    return info


def parse_meminfo(text: str) -> Dict[str, Any]:
    """Parse /proc/meminfo into used/total MB and percentage."""
    kv: Dict[str, int] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, rest = line.split(":", 1)
        parts = rest.strip().split()
        if parts and parts[0].isdigit():
            kv[key.strip()] = int(parts[0])
    total_kb = kv.get("MemTotal", 0)
    avail_kb = kv.get("MemAvailable", kv.get("MemFree", 0))
    if total_kb <= 0:
        return {"ram_used_mb": None, "ram_total_mb": None, "ram_pct": None}
    used_kb = max(0, total_kb - avail_kb)
    return {
        "ram_used_mb": round(used_kb / 1024),
        "ram_total_mb": round(total_kb / 1024),
        "ram_pct": round((used_kb / total_kb) * 100.0, 1),
    }


def parse_cpu_times(proc_stat_text: str) -> Optional[Tuple[int, int]]:
    """Return (idle, total) ticks from the aggregate 'cpu ' line of /proc/stat."""
    for line in proc_stat_text.splitlines():
        if line.startswith("cpu "):
            parts = line.split()[1:]
            nums = [int(x) for x in parts if x.isdigit()]
            if len(nums) >= 4:
                idle = nums[3] + (nums[4] if len(nums) > 4 else 0)
                total = sum(nums)
                return idle, total
    return None


def compute_cpu_percent(prev: Optional[Tuple[int, int]], curr: Optional[Tuple[int, int]]) -> Optional[float]:
    if not prev or not curr:
        return None
    d_idle = curr[0] - prev[0]
    d_total = curr[1] - prev[1]
    if d_total <= 0:
        return 0.0
    busy = max(0.0, min(100.0, (1.0 - (d_idle / d_total)) * 100.0))
    return round(busy, 1)


def parse_uptime(proc_uptime_text: str) -> Optional[float]:
    parts = proc_uptime_text.strip().split()
    if not parts:
        return None
    try:
        return float(parts[0])
    except ValueError:
        return None


def parse_thermal_zones(zones: Sequence[Tuple[str, str]]) -> Optional[float]:
    """Select CPU/SoC temperature in Celsius from (zone_type, raw_temp) tuples."""
    cpu_temps: List[float] = []
    all_temps: List[float] = []
    for ztype, raw in zones:
        try:
            val = float(raw.strip())
        except (ValueError, AttributeError):
            continue
        temp_c = val / 1000.0 if abs(val) > 200 else val
        if not (-20.0 <= temp_c <= 130.0):
            continue
        all_temps.append(temp_c)
        zt = (ztype or "").lower()
        if any(k in zt for k in ("cpu", "soc", "apss", "kryo", "tsens", "silver", "gold", "prime")):
            cpu_temps.append(temp_c)
    pool = cpu_temps or all_temps
    return round(max(pool), 1) if pool else None


def parse_vpn_list(output: str) -> List[str]:
    """Parse /usr/local/bin/vpn list output matching naomi-shell SystemTools semantics."""
    profiles: List[str] = []
    seen = set()
    for line in output.splitlines():
        m = VPN_LIST_LINE_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name.lower() in ("available", "profiles", "vpn", "none"):
            continue
        if name not in seen:
            seen.add(name)
            profiles.append(name)
    return profiles


def validate_vpn_profile(profile: str) -> bool:
    return bool(VPN_PROFILE_RE.match(profile or ""))


def parse_vpn_short_status(status_output: str, profiles: Sequence[str] = ()) -> Tuple[str, Optional[str]]:
    """Summarize vpn status text into (short_label, active_profile_or_none)."""
    cleaned = (status_output or "").strip()
    if not cleaned:
        return "Выкл", None
    lower = cleaned.lower()
    if any(w in lower for w in ("inactive", "disconnected", "down", "off", "stopped", "не актив", "выкл")):
        # Make sure no active marker overrides
        if not any(w in lower for w in ("active:", "connected", "running", "up:")):
            return "Выкл", None
    for prof in profiles:
        if re.search(rf"(?<![a-zA-Z0-9_.-]){re.escape(prof)}(?![a-zA-Z0-9_.-])", cleaned):
            return f"Вкл ({prof})", prof
    first_line = cleaned.splitlines()[0].strip()
    if len(first_line) > 36:
        first_line = first_line[:35] + "…"
    return first_line or "Вкл", None


def clamp_brightness_raw(percent: int, max_brightness: int) -> int:
    pct = max(0, min(100, int(percent)))
    if max_brightness <= 0:
        return 0
    if pct == 0:
        return 0
    return max(1, min(max_brightness, int(round((pct / 100.0) * max_brightness))))


def parse_busctl_int(output: str) -> Optional[int]:
    """Parse 'i 45' or 'u 45' from `busctl get-property`."""
    parts = (output or "").strip().split()
    if len(parts) >= 2:
        try:
            return int(float(parts[-1]))
        except ValueError:
            return None
    return None


def parse_busctl_bool(output: str) -> Optional[bool]:
    parts = (output or "").strip().split()
    if len(parts) >= 2:
        val = parts[-1].lower()
        if val in ("true", "1"):
            return True
        if val in ("false", "0"):
            return False
    return None


def parse_pactl_volume(output: str) -> Optional[int]:
    m = re.search(r"(\d+)%", output or "")
    if not m:
        return None
    return max(0, min(150, int(m.group(1))))


def parse_pactl_mute(output: str) -> Optional[bool]:
    lower = (output or "").lower()
    if "yes" in lower:
        return True
    if "no" in lower:
        return False
    return None


def build_cwd_tracked_command(command: str, marker: str) -> str:
    """Wrap a bash command so exit code is preserved and resulting $PWD is emitted after marker."""
    return (
        f"{command}\n"
        "__pipa_rc=$?\n"
        f'printf "\\n{marker}%s\\n" "$PWD"\n'
        "exit $__pipa_rc\n"
    )


def parse_cwd_tracked_output(
    raw_output: str,
    marker: str,
    fallback_cwd: str,
) -> Tuple[str, str]:
    """Split tracked bash output into (clean_output, resolved_cwd)."""
    idx = raw_output.rfind(marker)
    if idx == -1:
        return raw_output, fallback_cwd
    before = raw_output[:idx]
    if before.endswith("\n"):
        before = before[:-1]
    after = raw_output[idx + len(marker) :]
    lines = after.splitlines()
    new_cwd = lines[0].strip() if lines else ""
    return before, (new_cwd or fallback_cwd)


def build_camera_snap_cmd(
    out_path: str | Path,
    facing: str = "back",
    zoom: int = 0,
    flash: str = "off",
    width: int = 1920,
    height: int = 1080,
    binary: str = CAMERA_BIN,
) -> List[str]:
    """Build sanitized CLI argv for headless `naomi-camera --snap` capture."""
    clean_facing = (facing or "back").strip().lower()
    if clean_facing not in ("back", "front"):
        raise ValueError(f"Unsupported camera facing: {facing!r}")
    clean_flash = (flash or "off").strip().lower()
    if clean_flash not in ("off", "auto", "on"):
        raise ValueError(f"Unsupported flash mode: {flash!r}")
    if clean_facing == "front":
        clean_flash = "off"
    clamped_zoom = max(0, min(99, int(zoom)))
    clamped_w = max(320, int(width))
    clamped_h = max(240, int(height))
    return [
        str(binary),
        "--snap",
        clean_facing,
        "--out",
        str(out_path),
        "--zoom",
        str(clamped_zoom),
        "--flash",
        clean_flash,
        "--width",
        str(clamped_w),
        "--height",
        str(clamped_h),
    ]


def build_camera_torch_cmd(seconds: int = 600, binary: str = CAMERA_BIN) -> List[str]:
    """Build sanitized CLI argv for headless `naomi-camera --torch <seconds>`."""
    clamped_sec = max(1, min(3600, int(seconds)))
    return [str(binary), "--torch", str(clamped_sec)]


# ---------------------------------------------------------------------------
# Async Tablet Driver
# ---------------------------------------------------------------------------

class TabletDriver:
    """Async control & telemetry driver for Xiaomi Pad 6 running PopkaPhone Linux."""

    def __init__(
        self,
        backlight_path: Optional[Path] = None,
        battery_path: Path = BATTERY_PATH,
        uid: int = 32011,
        wifi_interface: str = "wlp1s0",
        camera_bin: str = CAMERA_BIN,
        camera_wrapper_bin: str = CAMERA_WRAPPER_BIN,
        torch_pid_path: Path = TORCH_PID_PATH,
    ) -> None:
        self._backlight_override = backlight_path
        self.battery_path = battery_path
        self.uid = uid
        self.wifi_interface = wifi_interface
        self.camera_bin = camera_bin
        self.camera_wrapper_bin = camera_wrapper_bin
        self.torch_pid_path = torch_pid_path
        self._saved_brightness_pct: int = 50
        self._last_cpu_times: Optional[Tuple[int, int]] = None
        self._cached_vpn_profiles: List[str] = []

    def _session_env(self) -> Dict[str, str]:
        env = os.environ.copy()
        runtime_dir = env.get("XDG_RUNTIME_DIR") or f"/run/user/{self.uid}"
        env["XDG_RUNTIME_DIR"] = runtime_dir
        env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime_dir}/bus")
        return env

    def _wayland_env(self) -> Dict[str, str]:
        env = self._session_env()
        env.setdefault("WAYLAND_DISPLAY", "wayland-naomi")
        return env

    async def _run_cmd(
        self,
        args: Sequence[str],
        timeout: float = 15.0,
        env: Optional[Dict[str, str]] = None,
        stdin_bytes: Optional[bytes] = None,
    ) -> Tuple[int, str, str]:
        try:
            proc = await asyncio.create_subprocess_exec(
                *args,
                stdin=asyncio.subprocess.PIPE if stdin_bytes is not None else asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env=env,
            )
            try:
                out, err = await asyncio.wait_for(proc.communicate(stdin_bytes), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.communicate()
                return 124, "", f"Таймаут команды ({timeout:.0f} с)"
            return (
                proc.returncode if proc.returncode is not None else 1,
                out.decode("utf-8", errors="replace"),
                err.decode("utf-8", errors="replace"),
            )
        except FileNotFoundError:
            return 127, "", f"Команда не найдена: {args[0]}"
        except OSError as exc:
            return 1, "", str(exc)

    def _find_backlight_dir(self) -> Optional[Path]:
        if self._backlight_override and self._backlight_override.is_dir():
            return self._backlight_override
        if BACKLIGHT_PREFERRED.is_dir():
            return BACKLIGHT_PREFERRED
        if BACKLIGHT_ROOT.is_dir():
            candidates = sorted(p for p in BACKLIGHT_ROOT.iterdir() if p.is_dir())
            if candidates:
                return candidates[0]
        return None

    def read_battery(self) -> Dict[str, Any]:
        fields = ("capacity", "status", "temp", "current_now", "voltage_now", "night_charging")
        raw = {f: _read_text_safe(self.battery_path / f) for f in fields}
        return parse_battery_info(raw)

    def read_brightness(self) -> Tuple[Optional[int], Optional[int], Optional[int]]:
        """Return (pct, raw, max_raw)."""
        bdir = self._find_backlight_dir()
        if not bdir:
            return None, None, None
        max_raw_s = _read_text_safe(bdir / "max_brightness")
        cur_raw_s = _read_text_safe(bdir / "actual_brightness") or _read_text_safe(bdir / "brightness")
        if not max_raw_s or cur_raw_s is None:
            return None, None, None
        try:
            max_raw = int(max_raw_s)
            cur_raw = int(cur_raw_s)
        except ValueError:
            return None, None, None
        if max_raw <= 0:
            return None, cur_raw, max_raw
        pct = max(0, min(100, int(round((cur_raw / max_raw) * 100.0))))
        if pct > 0:
            self._saved_brightness_pct = pct
        return pct, cur_raw, max_raw

    async def set_brightness_pct(self, percent: int) -> Tuple[bool, str]:
        bdir = self._find_backlight_dir()
        if not bdir:
            return False, "Узел подсветки не найден."
        max_raw_s = _read_text_safe(bdir / "max_brightness")
        if not max_raw_s or not max_raw_s.isdigit():
            return False, "Не удалось прочитать max_brightness."
        max_raw = int(max_raw_s)
        pct = max(0, min(100, int(percent)))
        target_raw = clamp_brightness_raw(pct, max_raw)
        if pct > 0:
            self._saved_brightness_pct = pct
        target_file = bdir / "brightness"
        try:
            target_file.write_text(f"{target_raw}\n", encoding="utf-8")
            return True, f"Яркость установлена: {pct}% ({target_raw}/{max_raw})."
        except PermissionError:
            rc, _, err = await self._run_cmd(
                ["sudo", "-n", "tee", str(target_file)],
                stdin_bytes=f"{target_raw}\n".encode("ascii"),
            )
            if rc == 0:
                return True, f"Яркость установлена: {pct}% ({target_raw}/{max_raw})."
            return False, f"Ошибка записи яркости: {err.strip() or rc}"
        except OSError as exc:
            return False, f"Ошибка записи яркости: {exc}"

    async def adjust_brightness(self, delta_pct: int) -> Tuple[bool, str]:
        cur_pct, _, _ = self.read_brightness()
        base = cur_pct if cur_pct is not None else self._saved_brightness_pct
        return await self.set_brightness_pct(max(5, min(100, base + delta_pct)))

    async def screen_off(self) -> Tuple[bool, str]:
        cur_pct, _, _ = self.read_brightness()
        if cur_pct and cur_pct > 0:
            self._saved_brightness_pct = cur_pct
        ok, msg = await self.set_brightness_pct(0)
        return ok, "Экран погашен (яркость 0%)." if ok else msg

    async def screen_on(self) -> Tuple[bool, str]:
        target = self._saved_brightness_pct if self._saved_brightness_pct > 0 else 50
        ok, msg = await self.set_brightness_pct(target)
        return ok, f"Экран включён (яркость {target}%)." if ok else msg

    async def get_volume(self) -> Tuple[Optional[int], Optional[bool]]:
        rc_v, out_v, _ = await self._run_cmd(
            ["busctl", "get-property", "org.naomi.Phone", "/org/naomi/Phone/Audio", "org.naomi.Phone.Audio", "Volume"],
            timeout=5.0,
        )
        rc_m, out_m, _ = await self._run_cmd(
            ["busctl", "get-property", "org.naomi.Phone", "/org/naomi/Phone/Audio", "org.naomi.Phone.Audio", "Muted"],
            timeout=5.0,
        )
        vol = parse_busctl_int(out_v) if rc_v == 0 else None
        muted = parse_busctl_bool(out_m) if rc_m == 0 else None
        if vol is not None:
            return vol, muted

        # Fallback to pactl
        env = self._session_env()
        rc1, o1, _ = await self._run_cmd(["pactl", "get-sink-volume", "@DEFAULT_SINK@"], timeout=5.0, env=env)
        rc2, o2, _ = await self._run_cmd(["pactl", "get-sink-mute", "@DEFAULT_SINK@"], timeout=5.0, env=env)
        return (
            parse_pactl_volume(o1) if rc1 == 0 else None,
            parse_pactl_mute(o2) if rc2 == 0 else None,
        )

    async def volume_step(self, direction: str) -> Tuple[bool, str]:
        method = "VolumeUp" if direction == "up" else "VolumeDown"
        rc, _, _ = await self._run_cmd(
            ["busctl", "call", "org.naomi.Phone", "/org/naomi/Phone/Audio", "org.naomi.Phone.Audio", method],
            timeout=5.0,
        )
        if rc != 0:
            delta = "+5%" if direction == "up" else "-5%"
            rc, _, err = await self._run_cmd(
                ["pactl", "set-sink-volume", "@DEFAULT_SINK@", delta],
                timeout=5.0,
                env=self._session_env(),
            )
            if rc != 0:
                return False, f"Не удалось изменить громкость: {err.strip()}"
        vol, muted = await self.get_volume()
        return True, f"Громкость: {'🔇' if muted else f'{vol}%'}"

    async def toggle_mute(self) -> Tuple[bool, str]:
        _, muted = await self.get_volume()
        target = not bool(muted)
        rc, _, _ = await self._run_cmd(
            [
                "busctl",
                "call",
                "org.naomi.Phone",
                "/org/naomi/Phone/Audio",
                "org.naomi.Phone.Audio",
                "SetMuted",
                "b",
                "true" if target else "false",
            ],
            timeout=5.0,
        )
        if rc != 0:
            rc, _, err = await self._run_cmd(
                ["pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"],
                timeout=5.0,
                env=self._session_env(),
            )
            if rc != 0:
                return False, f"Не удалось переключить звук: {err.strip()}"
        return True, "Звук выключен (Mute)." if target else "Звук включён."

    def read_cpu_temp(self) -> Optional[float]:
        if not THERMAL_ROOT.is_dir():
            return None
        zones: List[Tuple[str, str]] = []
        for z in sorted(THERMAL_ROOT.glob("thermal_zone*")):
            zt = _read_text_safe(z / "type") or z.name
            tv = _read_text_safe(z / "temp")
            if tv is not None:
                zones.append((zt, tv))
        return parse_thermal_zones(zones)

    async def read_cpu_percent(self) -> Optional[float]:
        stat1 = _read_text_safe(Path("/proc/stat"))
        t1 = parse_cpu_times(stat1) if stat1 else None
        if self._last_cpu_times is not None and t1 is not None and t1[1] > self._last_cpu_times[1]:
            pct = compute_cpu_percent(self._last_cpu_times, t1)
            self._last_cpu_times = t1
            return pct
        await asyncio.sleep(0.15)
        stat2 = _read_text_safe(Path("/proc/stat"))
        t2 = parse_cpu_times(stat2) if stat2 else None
        self._last_cpu_times = t2
        return compute_cpu_percent(t1, t2)

    def read_memory(self) -> Dict[str, Any]:
        txt = _read_text_safe(Path("/proc/meminfo"))
        return parse_meminfo(txt) if txt else {"ram_used_mb": None, "ram_total_mb": None, "ram_pct": None}

    def read_disk(self) -> Dict[str, Optional[float]]:
        try:
            st = os.statvfs("/")
            total = (st.f_blocks * st.f_frsize) / (1024**3)
            free = (st.f_bavail * st.f_frsize) / (1024**3)
            return {"disk_free_gb": round(free, 1), "disk_total_gb": round(total, 1)}
        except (AttributeError, OSError):
            return {"disk_free_gb": None, "disk_total_gb": None}

    def read_uptime(self) -> Optional[float]:
        txt = _read_text_safe(Path("/proc/uptime"))
        return parse_uptime(txt) if txt else None

    def _wifi_candidates(self) -> List[str]:
        candidates: List[str] = []
        net_dir = Path("/sys/class/net")
        if net_dir.is_dir():
            found: List[Tuple[int, str]] = []
            for p in sorted(net_dir.iterdir()):
                if p.name.startswith("p2p-"):
                    continue
                if (p / "wireless").is_dir():
                    state = _read_text_safe(p / "operstate") or ""
                    found.append((0 if state == "up" else 1, p.name))
            found.sort()
            candidates.extend(name for _, name in found)
        for fallback in (self.wifi_interface, "wlan0", "wlp1s0"):
            if fallback and fallback not in candidates:
                candidates.append(fallback)
        return candidates

    async def get_wifi_ip(self) -> Optional[str]:
        for iface in self._wifi_candidates():
            rc, out, _ = await self._run_cmd(["ip", "-4", "-o", "addr", "show", "dev", iface], timeout=5.0)
            if rc == 0:
                m = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)", out)
                if m:
                    return m.group(1)
        return None

    async def get_status(self) -> Dict[str, Any]:
        batt = self.read_battery()
        bri_pct, bri_raw, bri_max = self.read_brightness()
        mem = self.read_memory()
        disk = self.read_disk()
        uptime = self.read_uptime()
        cpu_temp = self.read_cpu_temp()

        (vol_pct, muted), cpu_pct, wifi_ip, (_, vpn_short, _) = await asyncio.gather(
            self.get_volume(),
            self.read_cpu_percent(),
            self.get_wifi_ip(),
            self.vpn_status(),
        )
        return {
            "battery": batt,
            "brightness_pct": bri_pct,
            "brightness_raw": bri_raw,
            "brightness_max": bri_max,
            "volume_pct": vol_pct,
            "muted": muted,
            "cpu_pct": cpu_pct if cpu_pct is not None else 0.0,
            "cpu_temp_c": cpu_temp,
            **mem,
            **disk,
            "uptime_sec": uptime,
            "wifi_ip": wifi_ip,
            "vpn_short": vpn_short,
        }

    # ------------------------------------------------------------------
    # Screenshots
    # ------------------------------------------------------------------

    async def take_screenshot(self, timeout: float = 6.0) -> Tuple[Optional[bytes], str]:
        """Capture screenshot from naomi-shell via SIGUSR2 -> /tmp/naomi-shot.png with fallbacks."""
        prev_mtime = NAOMI_SHOT_PATH.stat().st_mtime if NAOMI_SHOT_PATH.is_file() else 0.0
        triggered = False

        # Primary: SIGUSR2 directly to naomi-shell (dev_screenshot.cpp saves /tmp/naomi-shot.png atomically)
        rc, out, _ = await self._run_cmd(["pidof", "naomi-shell"], timeout=3.0)
        pids = [int(p) for p in out.split() if p.isdigit()] if rc == 0 else []
        for pid in pids:
            try:
                os.kill(pid, signal.SIGUSR2)
                triggered = True
            except PermissionError:
                rc_k, _, _ = await self._run_cmd(["sudo", "-n", "kill", "-USR2", str(pid)], timeout=3.0)
                if rc_k == 0:
                    triggered = True
            except OSError:
                pass

        if not triggered:
            rc_s, _, _ = await self._run_cmd(
                ["sudo", "-n", "systemctl", "kill", "-s", "SIGUSR2", "naomi-shell.service"],
                timeout=3.0,
            )
            triggered = rc_s == 0

        if triggered:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if NAOMI_SHOT_PATH.is_file():
                    try:
                        st = NAOMI_SHOT_PATH.stat()
                        if st.st_mtime > prev_mtime and st.st_size > 128:
                            data = NAOMI_SHOT_PATH.read_bytes()
                            if data.startswith(PNG_MAGIC):
                                return data, "Скриншот экрана планшета (naomi-shell)"
                    except OSError:
                        pass
                await asyncio.sleep(0.25)

        # Fallback 1: grim (Wayland)
        tmp_grim = Path("/tmp/pipa-bot-grim.png")
        env = self._session_env()
        env.setdefault("WAYLAND_DISPLAY", "wayland-naomi")
        rc_g, _, _ = await self._run_cmd(["grim", str(tmp_grim)], timeout=5.0, env=env)
        if rc_g == 0 and tmp_grim.is_file():
            try:
                data = tmp_grim.read_bytes()
                if data.startswith(PNG_MAGIC):
                    return data, "Скриншот экрана планшета (grim)"
            except OSError:
                pass

        # Fallback 2: newest file in ~/Screenshots
        shots_dir = Path.home() / "Screenshots"
        if shots_dir.is_dir():
            pngs = sorted(shots_dir.glob("*.png"), key=lambda p: p.stat().st_mtime, reverse=True)
            if pngs:
                try:
                    data = pngs[0].read_bytes()
                    if data.startswith(PNG_MAGIC):
                        return data, f"Последний сохранённый скриншот: {pngs[0].name}"
                except OSError:
                    pass

        return None, "Не удалось сделать скриншот (naomi-shell не ответил на SIGUSR2)."

    # ------------------------------------------------------------------
    # VPN (/usr/local/bin/vpn)
    # ------------------------------------------------------------------

    async def list_vpn_profiles(self) -> List[str]:
        rc, out, _ = await self._run_cmd([VPN_BIN, "list"], timeout=10.0)
        if rc == 0:
            self._cached_vpn_profiles = parse_vpn_list(out)
        return list(self._cached_vpn_profiles)

    async def vpn_status(self) -> Tuple[str, str, Optional[str]]:
        """Return (raw_status, short_status, active_profile)."""
        rc, out, err = await self._run_cmd([VPN_BIN, "status"], timeout=10.0)
        raw = out.strip() if rc == 0 else (out.strip() or err.strip() or "VPN недоступен")
        short, active = parse_vpn_short_status(raw, self._cached_vpn_profiles)
        return raw, short, active

    async def vpn_connect(self, profile: str) -> Tuple[bool, str]:
        if not validate_vpn_profile(profile):
            return False, f"Некорректное имя VPN-профиля: {profile}"
        rc, out, err = await self._run_cmd(["sudo", "-n", VPN_BIN, "on", profile], timeout=30.0)
        if rc != 0:
            # Resilient fallback if CLI expects 'up' instead of 'on'
            combined = (out + err).lower()
            if "usage" in combined or "unknown" in combined or "up" in combined:
                rc, out, err = await self._run_cmd(["sudo", "-n", VPN_BIN, "up", profile], timeout=30.0)
        if rc == 0:
            return True, f"VPN подключён: {profile}"
        detail = (err or out).strip()[:200]
        return False, f"Не удалось включить VPN '{profile}': {detail or f'код {rc}'}"

    async def vpn_disconnect(self) -> Tuple[bool, str]:
        rc, out, err = await self._run_cmd(["sudo", "-n", VPN_BIN, "off"], timeout=30.0)
        if rc != 0:
            combined = (out + err).lower()
            if "usage" in combined or "unknown" in combined or "down" in combined:
                rc, out, err = await self._run_cmd(["sudo", "-n", VPN_BIN, "down"], timeout=30.0)
        if rc == 0:
            return True, "VPN отключён."
        detail = (err or out).strip()[:200]
        return False, f"Не удалось отключить VPN: {detail or f'код {rc}'}"

    # ------------------------------------------------------------------
    # Systemd Services
    # ------------------------------------------------------------------

    async def list_services(self) -> List[Dict[str, Any]]:
        async def probe(spec: ServiceSpec) -> Dict[str, Any]:
            if spec.user:
                cmd = ["systemctl", "--user", "is-active", spec.unit]
                env = self._session_env()
            else:
                cmd = ["systemctl", "is-active", spec.unit]
                env = None
            rc, out, _ = await self._run_cmd(cmd, timeout=5.0, env=env)
            state = out.strip() or ("active" if rc == 0 else "inactive")
            return {
                "key": spec.key,
                "unit": spec.unit,
                "label": spec.label,
                "user": spec.user,
                "critical": spec.critical,
                "active": state == "active",
                "state": state,
            }

        return list(await asyncio.gather(*(probe(s) for s in MANAGED_SERVICES)))

    async def service_action(self, key: str, action: str) -> Tuple[bool, str]:
        if action not in ("start", "stop", "restart"):
            return False, f"Неизвестное действие службы: {action}"
        spec = SERVICES_BY_KEY.get(key)
        if not spec:
            return False, f"Неизвестная служба: {key}"
        if spec.user:
            cmd = ["systemctl", "--user", action, spec.unit]
            env = self._session_env()
        else:
            cmd = ["sudo", "-n", "systemctl", action, spec.unit]
            env = None
        rc, out, err = await self._run_cmd(cmd, timeout=25.0, env=env)
        if rc == 0:
            action_ru = {"start": "запущена", "stop": "остановлена", "restart": "перезапущена"}[action]
            return True, f"Служба {spec.label} {action_ru}."
        return False, f"Ошибка {action} {spec.label}: {(err or out).strip()[:200]}"

    # ------------------------------------------------------------------
    # Power, Dual-Boot One UI, Browser, Terminal
    # ------------------------------------------------------------------

    async def open_url(self, url: str) -> Tuple[bool, str]:
        cleaned = url.strip()
        if not cleaned.startswith(("http://", "https://")):
            return False, "Ссылка должна начинаться с http:// или https://"
        env = self._session_env()
        rc, _, err = await self._run_cmd(
            ["busctl", "--user", "call", "org.naomi.Web", "/web", "org.naomi.Web", "Open", "s", cleaned],
            timeout=8.0,
            env=env,
        )
        if rc == 0:
            return True, f"Открыто на планшете: {cleaned}"
        rc2, _, err2 = await self._run_cmd(["naomi-xdg-open", cleaned], timeout=8.0, env=env)
        if rc2 == 0:
            return True, f"Открыто на планшете: {cleaned}"
        return False, f"Не удалось открыть ссылку: {(err or err2).strip()[:200]}"

    async def reboot(self) -> Tuple[bool, str]:
        rc, _, _ = await self._run_cmd(
            ["busctl", "call", "org.naomi.Phone", "/org/naomi/Phone/System", "org.naomi.Phone.System", "Reboot"],
            timeout=10.0,
        )
        if rc != 0:
            rc, _, err = await self._run_cmd(["sudo", "-n", "systemctl", "reboot"], timeout=10.0)
            if rc != 0:
                return False, f"Не удалось перезагрузить планшет: {err.strip()}"
        return True, "Команда перезагрузки планшета отправлена."

    async def poweroff(self) -> Tuple[bool, str]:
        rc, _, _ = await self._run_cmd(
            ["busctl", "call", "org.naomi.Phone", "/org/naomi/Phone/System", "org.naomi.Phone.System", "PowerOff"],
            timeout=10.0,
        )
        if rc != 0:
            rc, _, err = await self._run_cmd(["sudo", "-n", "systemctl", "poweroff"], timeout=10.0)
            if rc != 0:
                return False, f"Не удалось выключить планшет: {err.strip()}"
        return True, "Команда выключения планшета отправлена."

    async def reboot_one_ui(self) -> Tuple[bool, str]:
        rc, _, err = await self._run_cmd(
            ["busctl", "call", "org.naomi.Phone", "/org/naomi/Phone/System", "org.naomi.Phone.System", "RebootOneUi"],
            timeout=30.0,
        )
        if rc != 0:
            rc, _, err2 = await self._run_cmd(["sudo", "-n", BOOT_ANDROID_BIN], timeout=30.0)
            if rc != 0:
                return False, f"Не удалось переключиться в One UI: {(err2 or err).strip()[:200]}"
        return True, "Переключение слота и перезагрузка в Android (One UI) запущены."

    async def run_shell(self, command: str, timeout: float = 30.0) -> Tuple[int, str]:
        """Run a shell command on the tablet and return (exit_code, combined_output)."""
        try:
            proc = await asyncio.create_subprocess_shell(
                command,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                env=self._session_env(),
            )
            try:
                out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.communicate()
                return 124, f"Таймаут выполнения ({timeout:.0f} с)"
            text = out.decode("utf-8", errors="replace")
            return proc.returncode if proc.returncode is not None else 1, text
        except OSError as exc:
            return 1, str(exc)

    async def run_terminal_command(
        self,
        command: str,
        cwd: Optional[str] = None,
        timeout: float = 30.0,
    ) -> Tuple[int, str, str]:
        """Run a bash command in `cwd`, track `cd` changes via marker, return (rc, output, new_cwd)."""
        base_path = Path(cwd or "~").expanduser()
        if not base_path.is_dir():
            base_path = Path.home()
        start_cwd = str(base_path)
        marker = f"__PIPA_CWD_{secrets.token_hex(6)}__"
        wrapped = build_cwd_tracked_command(command, marker)

        try:
            proc = await asyncio.create_subprocess_exec(
                "bash",
                "-c",
                wrapped,
                cwd=start_cwd,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                env=self._session_env(),
            )
            try:
                out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.communicate()
                return 124, f"Таймаут выполнения ({timeout:.0f} с)", start_cwd
            raw = out.decode("utf-8", errors="replace")
            rc = proc.returncode if proc.returncode is not None else 1
            clean_out, new_cwd = parse_cwd_tracked_output(raw, marker, start_cwd)
            return rc, clean_out, new_cwd
        except FileNotFoundError:
            return 127, "Команда bash не найдена в системе.", start_cwd
        except OSError as exc:
            return 1, str(exc), start_cwd

    # ------------------------------------------------------------------
    # Battery Limit (night_charging) & Wayland/X11 Clipboard
    # ------------------------------------------------------------------

    async def set_night_charging(self, enabled: bool) -> Tuple[bool, str]:
        """Enable or disable 80% battery charge limit via sysfs night_charging."""
        target_file = self.battery_path / "night_charging"
        val_str = "1\n" if enabled else "0\n"
        state_ru = "включён (лимит 80%)" if enabled else "отключён"
        if not target_file.exists():
            return False, f"Узел {target_file} недоступен на этом ядре."
        try:
            target_file.write_text(val_str, encoding="utf-8")
            return True, f"Режим ночной зарядки {state_ru}."
        except PermissionError:
            rc, _, err = await self._run_cmd(
                ["sudo", "-n", "tee", str(target_file)],
                stdin_bytes=val_str.encode("ascii"),
                timeout=6.0,
            )
            if rc == 0:
                return True, f"Режим ночной зарядки {state_ru}."
            return False, f"Не удалось изменить night_charging: {err.strip() or rc}"
        except OSError as exc:
            return False, f"Ошибка записи night_charging: {exc}"

    async def get_clipboard(self, timeout: float = 5.0) -> Tuple[bool, str]:
        """Read UTF-8 text from tablet clipboard (wl-paste primary, xclip/xsel fallbacks)."""
        env = self._wayland_env()
        attempts = (
            ["wl-paste", "--no-newline"],
            ["xclip", "-o", "-selection", "clipboard"],
            ["xsel", "--clipboard", "--output"],
        )
        last_err = ""
        for cmd in attempts:
            rc, out, err = await self._run_cmd(cmd, timeout=timeout, env=env)
            if rc == 0:
                return True, out
            err_lower = (err or "").lower()
            if cmd[0] == "wl-paste" and any(
                hint in err_lower for hint in ("nothing is copied", "no selection", "empty")
            ):
                return True, ""
            if cmd[0] == "xclip" and "target string not available" in err_lower:
                return True, ""
            if rc != 127:
                last_err = err.strip() or f"{cmd[0]} exited with {rc}"
            elif not last_err:
                last_err = err.strip()
        return False, f"Не удалось прочитать буфер планшета: {last_err or 'нет wl-paste/xclip/xsel'}"

    async def set_clipboard(self, text: str, timeout: float = 5.0) -> Tuple[bool, str]:
        """Write UTF-8 text to tablet clipboard via wl-copy (Wayland) or xclip/xsel fallback."""
        env = self._wayland_env()
        payload = text.encode("utf-8")

        if text == "":
            attempts: Sequence[Tuple[Sequence[str], Optional[bytes]]] = (
                (["wl-copy", "--clear"], None),
                (["xclip", "-i", "-selection", "clipboard"], b""),
                (["xsel", "--clipboard", "--input"], b""),
            )
        else:
            attempts = (
                (["wl-copy"], payload),
                (["xclip", "-i", "-selection", "clipboard"], payload),
                (["xsel", "--clipboard", "--input"], payload),
            )

        last_err = ""
        for cmd, stdin_data in attempts:
            try:
                # Redirect stdout/stderr to DEVNULL so background clipboard daemon forks do not hang pipes.
                proc = await asyncio.create_subprocess_exec(
                    *cmd,
                    stdin=asyncio.subprocess.PIPE if stdin_data is not None else asyncio.subprocess.DEVNULL,
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                    env=env,
                )
                try:
                    await asyncio.wait_for(proc.communicate(stdin_data), timeout=timeout)
                except asyncio.TimeoutError:
                    proc.kill()
                    await proc.communicate()
                    last_err = f"Таймаут {cmd[0]}"
                    continue
                if proc.returncode == 0:
                    return True, f"Скопировано в буфер планшета ({len(text)} симв.)."
                last_err = f"{cmd[0]} завершён с кодом {proc.returncode}"
            except FileNotFoundError:
                if not last_err:
                    last_err = f"Команда не найдена: {cmd[0]}"
                continue
            except OSError as exc:
                last_err = str(exc)
                continue

        return False, f"Не удалось записать в буфер планшета: {last_err or 'нет wl-copy/xclip/xsel'}"

    # ------------------------------------------------------------------
    # Camera (naomi-camera headless --snap / --torch & GUI launch)
    # ------------------------------------------------------------------

    def _camera_env(self) -> Dict[str, str]:
        env = self._session_env()
        env["HYBRIS_EGLPLATFORM"] = "null"
        env["EGL_PLATFORM"] = "null"
        return env

    def _torch_pid_file(self) -> Path:
        return Path(self.torch_pid_path).expanduser()

    def _read_torch_pid(self) -> Optional[int]:
        pid_file = self._torch_pid_file()
        raw = _read_text_safe(pid_file)
        if not raw:
            return None
        try:
            pid = int(raw.strip())
            return pid if pid > 1 else None
        except ValueError:
            return None

    def _is_pid_running(self, pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        except OSError:
            return False

        proc_status = Path(f"/proc/{pid}/status")
        if proc_status.is_file():
            txt = _read_text_safe(proc_status) or ""
            if "State:\tZ" in txt:
                return False
        return True

    def is_torch_on(self) -> bool:
        """Return True if managed `naomi-camera --torch` process is currently running."""
        pid = self._read_torch_pid()
        if pid is None:
            return False
        if self._is_pid_running(pid):
            return True
        try:
            self._torch_pid_file().unlink(missing_ok=True)
        except OSError:
            pass
        return False

    async def set_torch(self, on: bool, duration_sec: int = 600) -> Tuple[bool, str]:
        """Turn rear LED flashlight on/off via a managed `naomi-camera --torch` background process."""
        pid_file = self._torch_pid_file()

        if not on:
            pid = self._read_torch_pid()
            if pid is not None and self._is_pid_running(pid):
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass
                for _ in range(15):
                    if not self._is_pid_running(pid):
                        break
                    await asyncio.sleep(0.1)
                if self._is_pid_running(pid):
                    sigkill = getattr(signal, "SIGKILL", signal.SIGTERM)
                    try:
                        os.kill(pid, sigkill)
                    except OSError:
                        pass
            try:
                pid_file.unlink(missing_ok=True)
            except OSError:
                pass
            return True, "Фонарик выключен."

        if self.is_torch_on():
            return True, "Фонарик уже включён."

        cmd = build_camera_torch_cmd(seconds=duration_sec, binary=self.camera_bin)
        try:
            pid_file.parent.mkdir(parents=True, exist_ok=True)
            kwargs: Dict[str, Any] = {
                "stdin": asyncio.subprocess.DEVNULL,
                "stdout": asyncio.subprocess.DEVNULL,
                "stderr": asyncio.subprocess.PIPE,
                "env": self._camera_env(),
            }
            if os.name != "nt":
                kwargs["start_new_session"] = True
            proc = await asyncio.create_subprocess_exec(*cmd, **kwargs)
            pid_file.write_text(f"{proc.pid}\n", encoding="utf-8")
            await asyncio.sleep(0.3)
            if proc.returncode is not None:
                _, err_b = await proc.communicate()
                pid_file.unlink(missing_ok=True)
                err_txt = err_b.decode("utf-8", errors="replace").strip()[:200]
                return False, f"Не удалось включить фонарик: {err_txt or f'код {proc.returncode}'}"
            return True, "Фонарик включён."
        except FileNotFoundError:
            return False, f"Бинарник камеры не найден: {self.camera_bin}"
        except OSError as exc:
            return False, f"Ошибка запуска фонарика: {exc}"

    async def snap_camera(
        self,
        facing: str = "back",
        zoom: int = 0,
        flash: str = "off",
        width: int = 1920,
        height: int = 1080,
        timeout: float = 15.0,
    ) -> Tuple[bool, str]:
        """Capture a JPEG snapshot using headless `naomi-camera --snap` and return (ok, path_or_err)."""
        if self.is_torch_on():
            await self.set_torch(False)
            await asyncio.sleep(0.25)

        out_path = Path(tempfile.gettempdir()) / f"pipa-cam-{facing}-{secrets.token_hex(6)}.jpg"
        try:
            cmd = build_camera_snap_cmd(
                out_path=out_path,
                facing=facing,
                zoom=zoom,
                flash=flash,
                width=width,
                height=height,
                binary=self.camera_bin,
            )
        except ValueError as exc:
            return False, str(exc)

        rc, out, err = await self._run_cmd(cmd, timeout=timeout, env=self._camera_env())
        if rc == 0 and out_path.is_file():
            try:
                head = out_path.read_bytes()[:4]
                if head.startswith(JPEG_MAGIC) and out_path.stat().st_size > 128:
                    return True, str(out_path)
            except OSError:
                pass
        try:
            out_path.unlink(missing_ok=True)
        except OSError:
            pass
        detail = (err or out).strip()[:220]
        return False, f"Не удалось сделать снимок: {detail or f'код {rc}'}"

    async def launch_camera_app(self) -> Tuple[bool, str]:
        """Launch native PopkaPhone Camera UI detached on WAYLAND_DISPLAY=wayland-naomi."""
        if self.is_torch_on():
            await self.set_torch(False)
            await asyncio.sleep(0.2)

        target = self.camera_wrapper_bin if Path(self.camera_wrapper_bin).is_file() else self.camera_bin
        env = self._wayland_env()
        env["WAYLAND_DISPLAY"] = "wayland-naomi"
        env["QT_QPA_PLATFORM"] = "wayland"
        env["QT_WAYLAND_CLIENT_BUFFER_INTEGRATION"] = "none"
        env["QT_WAYLAND_DISABLE_WINDOWDECORATION"] = "1"
        env["HYBRIS_EGLPLATFORM"] = "null"
        env["EGL_PLATFORM"] = "null"
        try:
            kwargs: Dict[str, Any] = {
                "stdin": asyncio.subprocess.DEVNULL,
                "stdout": asyncio.subprocess.DEVNULL,
                "stderr": asyncio.subprocess.DEVNULL,
                "env": env,
            }
            if os.name != "nt":
                kwargs["start_new_session"] = True
            await asyncio.create_subprocess_exec(target, **kwargs)
            return True, "Приложение «Камера» запущено на экране планшета."
        except FileNotFoundError:
            return False, f"Приложение камеры не найдено: {target}"
        except OSError as exc:
            return False, f"Не удалось запустить Камеру: {exc}"

    # --- Home / Away / Auto Security Mode (/usr/local/bin/pipa-mode) ---
    async def get_travel_mode(self) -> Dict[str, Any]:
        rc, out, err = await self._run_cmd(["sudo", "-n", "/usr/local/bin/pipa-mode", "status", "--json"], timeout=6.0)
        if rc == 0 and out.strip():
            try:
                return json.loads(out)
            except Exception:
                pass
        return {"configured": "unknown", "active": "unknown", "ssid": "", "vpn": "", "firewall": err.strip() or "n/a"}

    async def set_travel_mode(self, mode: str) -> Tuple[bool, str]:
        if mode not in ("home", "away", "auto"):
            return False, f"Неизвестный режим: {mode}"
        rc, out, err = await self._run_cmd(["sudo", "-n", "/usr/local/bin/pipa-mode", mode], timeout=25.0)
        msg = (out or err or "").strip()
        return rc == 0, msg or f"Режим переключён: {mode}"
