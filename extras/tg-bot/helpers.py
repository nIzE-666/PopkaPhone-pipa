"""Pure helper utilities (alert evaluation, filename sanitization, command parsing)."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    from .config import AlertsConfig
except ImportError:
    from config import AlertsConfig


SAFE_FILENAME_RE = re.compile(r"[^a-zA-Z0-9а-яА-ЯёЁ._ -]+")


def sanitize_filename(name: str, fallback: str = "file.bin") -> str:
    """Sanitize untrusted Telegram filenames against path traversal and odd chars."""
    base = Path(name or "").name.strip()
    base = SAFE_FILENAME_RE.sub("_", base).strip(". ")
    if not base:
        base = fallback
    return base[:160]


def unique_target_path(directory: Path, filename: str) -> Path:
    """Return a collision-free Path inside directory for filename."""
    clean = sanitize_filename(filename)
    candidate = directory / clean
    if not candidate.exists():
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    for idx in range(1, 1000):
        alt = directory / f"{stem}_{idx}{suffix}"
        if not alt.exists():
            return alt
    return directory / f"{stem}_dup{suffix}"


def parse_open_command(arg_text: str) -> Tuple[str, str]:
    """Parse `/open [pc|tablet|tv] <url>` into (target, url). Defaults to 'tablet'."""
    raw = (arg_text or "").strip()
    if not raw:
        return "tablet", ""
    parts = raw.split(None, 1)
    first = parts[0].lower()
    if len(parts) == 2 and first in ("pc", "пк", "mypc"):
        return "pc", parts[1].strip()
    if len(parts) == 2 and first in ("tablet", "планшет", "pipa"):
        return "tablet", parts[1].strip()
    if len(parts) == 2 and first in ("tv", "android", "androidtv", "тв"):
        return "tv", parts[1].strip()
    return "tablet", raw


def truncate_shell_output(text: str, limit: int = 3800) -> Tuple[str, bool]:
    """Return (text_or_empty, needs_file) depending on Telegram 4096-char limit."""
    if len(text) <= limit:
        return text, False
    return text[:limit], True


class AlertWatcher:
    """Evaluates tablet telemetry against AlertsConfig thresholds with cooldown."""

    def __init__(self, config: AlertsConfig) -> None:
        self.config = config
        self._last_sent: Dict[str, float] = {}

    def _can_send(self, key: str, now: float) -> bool:
        prev = self._last_sent.get(key)
        if prev is None:
            return True
        return (now - prev) >= self.config.cooldown_sec

    def evaluate(self, tablet_status: Dict[str, Any], now: float) -> List[str]:
        """Return Russian alert messages triggered by current tablet telemetry."""
        if not self.config.enabled:
            return []

        alerts: List[str] = []
        batt = tablet_status.get("battery") or {}
        cap = batt.get("capacity")
        bstatus = str(batt.get("status") or "").lower()
        btemp = batt.get("temp_c")
        cpu_temp = tablet_status.get("cpu_temp_c")

        # Low battery only while discharging (not charging / full)
        is_charging = ("charging" in bstatus and "dis" not in bstatus) or "full" in bstatus
        if (
            self.config.low_battery_enabled
            and cap is not None
            and not is_charging
            and int(cap) < self.config.low_battery_pct
            and self._can_send("low_battery", now)
        ):
            self._last_sent["low_battery"] = now
            alerts.append(
                f"🪫 <b>Низкий заряд планшета!</b>\n"
                f"Осталось <b>{cap}%</b> (порог: {self.config.low_battery_pct}%). Пора подключить зарядку."
            )

        if (
            self.config.overheat_enabled
            and cpu_temp is not None
            and float(cpu_temp) > self.config.max_cpu_temp_c
            and self._can_send("cpu_temp", now)
        ):
            self._last_sent["cpu_temp"] = now
            alerts.append(
                f"🔥 <b>Перегрев CPU планшета!</b>\n"
                f"Температура CPU: <b>{float(cpu_temp):.1f}°C</b> (порог: {self.config.max_cpu_temp_c:.0f}°C)."
            )

        if (
            self.config.overheat_enabled
            and btemp is not None
            and float(btemp) > self.config.max_battery_temp_c
            and self._can_send("batt_temp", now)
        ):
            self._last_sent["batt_temp"] = now
            alerts.append(
                f"🌡 <b>Перегрев батареи планшета!</b>\n"
                f"Температура батареи: <b>{float(btemp):.1f}°C</b> (порог: {self.config.max_battery_temp_c:.0f}°C)."
            )

        return alerts
