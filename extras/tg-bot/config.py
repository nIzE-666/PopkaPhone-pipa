"""Configuration, secret loading, and zero-friction owner pairing for pipa-tg-bot."""

from __future__ import annotations

import json
import os
import secrets
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


DEFAULT_CONFIG_DIR = Path.home() / ".config" / "pipa-bot"
DEFAULT_DOWNLOADS_DIR = Path.home() / "Downloads" / "tg-bot"


@dataclass
class PCConfig:
    id: str
    name: str
    host: str
    mac: str = ""
    port: int = 18765
    broadcast: str = "192.168.1.255"
    token: str = ""

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "PCConfig":
        return cls(
            id=str(data.get("id", "pc")),
            name=str(data.get("name", data.get("id", "PC"))),
            host=str(data.get("host", "")),
            mac=str(data.get("mac", "")),
            port=int(data.get("port", 18765)),
            broadcast=str(data.get("broadcast", "192.168.1.255")),
            token=str(data.get("token", "")),
        )


@dataclass
class SamsungTVConfig:
    name: str = "Samsung TV"
    host: str = "192.168.1.150"
    port: int = 8002
    fallback_port: int = 8001
    dlna_port: int = 9197
    mac: str = ""
    broadcast: str = "192.168.1.255"
    app_name: str = "PipaBot"
    token_file: str = ""

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "SamsungTVConfig":
        return cls(
            name=str(data.get("name", "Samsung TV")),
            host=str(data.get("host", "192.168.1.150")),
            port=int(data.get("port", 8002)),
            fallback_port=int(data.get("fallback_port", 8001)),
            dlna_port=int(data.get("dlna_port", 9197)),
            mac=str(data.get("mac", "")),
            broadcast=str(data.get("broadcast", "192.168.1.255")),
            app_name=str(data.get("app_name", "PipaBot")),
            token_file=str(data.get("token_file", "")),
        )


@dataclass
class AndroidTVConfig:
    name: str = "Android TV"
    host: str = "192.168.1.160"
    adb_port: int = 5555
    cast_port: int = 8008
    remote_port: int = 6466

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "AndroidTVConfig":
        return cls(
            name=str(data.get("name", "Android TV")),
            host=str(data.get("host", "192.168.1.160")),
            adb_port=int(data.get("adb_port", 5555)),
            cast_port=int(data.get("cast_port", 8008)),
            remote_port=int(data.get("remote_port", 6466)),
        )


@dataclass
class NetworkConfig:
    router_ip: str = "192.168.1.1"
    router_name: str = "Home Router"
    subnet_prefix: str = "192.168.1."
    wifi_interface: str = "wlp1s0"
    tablet_ip: str = "192.168.1.50"

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "NetworkConfig":
        return cls(
            router_ip=str(data.get("router_ip", "192.168.1.1")),
            router_name=str(data.get("router_name", "Home Router")),
            subnet_prefix=str(data.get("subnet_prefix", "192.168.1.")),
            wifi_interface=str(data.get("wifi_interface", "wlp1s0")),
            tablet_ip=str(data.get("tablet_ip", "192.168.1.50")),
        )


@dataclass
class AlertsConfig:
    enabled: bool = True
    low_battery_enabled: bool = True
    low_battery_pct: int = 15
    overheat_enabled: bool = True
    max_cpu_temp_c: float = 65.0
    max_battery_temp_c: float = 42.0
    startup_notify: bool = True
    poll_interval_sec: int = 60
    cooldown_sec: int = 1800

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "AlertsConfig":
        return cls(
            enabled=bool(data.get("enabled", True)),
            low_battery_enabled=bool(data.get("low_battery_enabled", True)),
            low_battery_pct=int(data.get("low_battery_pct", 15)),
            overheat_enabled=bool(data.get("overheat_enabled", True)),
            max_cpu_temp_c=float(data.get("max_cpu_temp_c", 65.0)),
            max_battery_temp_c=float(data.get("max_battery_temp_c", 42.0)),
            startup_notify=bool(data.get("startup_notify", True)),
            poll_interval_sec=max(15, int(data.get("poll_interval_sec", 60))),
            cooldown_sec=max(60, int(data.get("cooldown_sec", 1800))),
        )


def default_pcs() -> List[PCConfig]:
    return [
        PCConfig(
            id="mypc",
            name="My PC (Windows)",
            host="192.168.1.100",
            mac="00:11:22:33:44:55",
            port=18765,
            broadcast="192.168.1.255",
        )
    ]


@dataclass
class BotConfig:
    bot_token: str = ""
    owner_id: Optional[int] = None
    pc_agent_token: str = ""
    telegram_proxy: str = ""
    downloads_dir: str = str(DEFAULT_DOWNLOADS_DIR)
    media_server_port: int = 18766
    pcs: List[PCConfig] = field(default_factory=default_pcs)
    samsung_tv: SamsungTVConfig = field(default_factory=SamsungTVConfig)
    android_tv: AndroidTVConfig = field(default_factory=AndroidTVConfig)
    network: NetworkConfig = field(default_factory=NetworkConfig)
    alerts: AlertsConfig = field(default_factory=AlertsConfig)
    config_dir: Path = field(default=DEFAULT_CONFIG_DIR, repr=False, compare=False)

    @property
    def config_path(self) -> Path:
        return self.config_dir / "config.json"

    @property
    def env_path(self) -> Path:
        return self.config_dir / "bot.env"

    @property
    def pair_code_path(self) -> Path:
        return self.config_dir / "pair_code.txt"

    @property
    def samsung_token_path(self) -> Path:
        if self.samsung_tv.token_file:
            return Path(self.samsung_tv.token_file).expanduser()
        return self.config_dir / "samsung_token.txt"

    def get_pc(self, pc_id: str) -> Optional[PCConfig]:
        for pc in self.pcs:
            if pc.id == pc_id:
                return pc
        return None

    def effective_pc_token(self, pc: PCConfig) -> str:
        return pc.token or self.pc_agent_token

    def to_persist_dict(self) -> Dict[str, Any]:
        """Serialize non-secret configuration for config.json."""
        return {
            "owner_id": self.owner_id,
            "downloads_dir": self.downloads_dir,
            "media_server_port": self.media_server_port,
            "pcs": [
                {
                    "id": pc.id,
                    "name": pc.name,
                    "host": pc.host,
                    "mac": pc.mac,
                    "port": pc.port,
                    "broadcast": pc.broadcast,
                    **({"token": pc.token} if pc.token else {}),
                }
                for pc in self.pcs
            ],
            "samsung_tv": asdict(self.samsung_tv),
            "android_tv": asdict(self.android_tv),
            "network": asdict(self.network),
            "alerts": asdict(self.alerts),
        }

    def save(self) -> Path:
        """Atomically persist config.json."""
        self.config_dir.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(self.to_persist_dict(), ensure_ascii=False, indent=2) + "\n"
        fd, tmp_name = tempfile.mkstemp(prefix=".config.", suffix=".tmp", dir=str(self.config_dir))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(payload)
            os.replace(tmp_name, self.config_path)
        except Exception:
            if os.path.exists(tmp_name):
                try:
                    os.unlink(tmp_name)
                except OSError:
                    pass
            raise
        return self.config_path


def parse_env_file(path: Path) -> Dict[str, str]:
    """Parse KEY=VALUE lines from an environment file."""
    result: Dict[str, str] = {}
    if not path.is_file():
        return result
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
            val = val[1:-1]
        if key:
            result[key] = val
    return result


def _parse_optional_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    s = str(raw).strip()
    if not s:
        return None
    try:
        val = int(s)
        return val if val > 0 else None
    except ValueError:
        return None


def load_config(
    config_dir: Optional[Path] = None,
    environ: Optional[Dict[str, str]] = None,
) -> BotConfig:
    """Load BotConfig from config.json, bot.env, and process environment."""
    cfg_dir = Path(config_dir or os.environ.get("PIPA_BOT_CONFIG_DIR") or DEFAULT_CONFIG_DIR).expanduser()
    env_map = parse_env_file(cfg_dir / "bot.env")
    proc_env = os.environ if environ is None else environ

    def env_get(key: str) -> str:
        val = proc_env.get(key)
        if val is not None and str(val).strip() != "":
            return str(val).strip()
        return env_map.get(key, "").strip()

    raw_json: Dict[str, Any] = {}
    cfg_path = cfg_dir / "config.json"
    if cfg_path.is_file():
        try:
            loaded = json.loads(cfg_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                raw_json = loaded
        except (OSError, json.JSONDecodeError):
            raw_json = {}

    pcs_raw = raw_json.get("pcs")
    if isinstance(pcs_raw, list) and pcs_raw:
        pcs = [PCConfig.from_dict(item) for item in pcs_raw if isinstance(item, dict)]
    else:
        pcs = default_pcs()

    owner_id = _parse_optional_int(env_get("OWNER_ID")) or _parse_optional_int(raw_json.get("owner_id"))

    samsung_raw = raw_json.get("samsung_tv") if isinstance(raw_json.get("samsung_tv"), dict) else {}
    android_raw = raw_json.get("android_tv") if isinstance(raw_json.get("android_tv"), dict) else {}
    net_raw = raw_json.get("network") if isinstance(raw_json.get("network"), dict) else {}
    alerts_raw = raw_json.get("alerts") if isinstance(raw_json.get("alerts"), dict) else {}

    cfg = BotConfig(
        bot_token=env_get("BOT_TOKEN"),
        owner_id=owner_id,
        pc_agent_token=env_get("PC_AGENT_TOKEN"),
        telegram_proxy=env_get("TELEGRAM_PROXY") or str(raw_json.get("telegram_proxy") or "").strip(),
        downloads_dir=str(raw_json.get("downloads_dir") or DEFAULT_DOWNLOADS_DIR),
        media_server_port=int(raw_json.get("media_server_port") or 18766),
        pcs=pcs,
        samsung_tv=SamsungTVConfig.from_dict(samsung_raw),
        android_tv=AndroidTVConfig.from_dict(android_raw),
        network=NetworkConfig.from_dict(net_raw),
        alerts=AlertsConfig.from_dict(alerts_raw),
        config_dir=cfg_dir,
    )
    return cfg


class OwnerAuthManager:
    """Strict owner access guard with zero-friction 6-digit one-time pairing."""

    def __init__(self, config: BotConfig) -> None:
        self.config = config
        self.pair_code: Optional[str] = None
        if self.config.owner_id is None:
            self.pair_code = self._init_pair_code()

    def _init_pair_code(self) -> str:
        code = f"{secrets.randbelow(1_000_000):06d}"
        self.config.config_dir.mkdir(parents=True, exist_ok=True)
        path = self.config.pair_code_path
        path.write_text(code + "\n", encoding="utf-8")
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
        return code

    @property
    def is_paired(self) -> bool:
        return self.config.owner_id is not None

    def is_owner(self, user_id: Optional[int]) -> bool:
        if user_id is None or self.config.owner_id is None:
            return False
        return int(user_id) == int(self.config.owner_id)

    def try_pair(self, user_id: int, submitted_code: str) -> tuple[bool, str]:
        """Attempt to pair the first owner using the 6-digit code."""
        if self.is_paired:
            if self.is_owner(user_id):
                return True, "Вы уже привязаны как владелец этого бота."
            return False, "🔒 Бот уже привязан к владельцу. Повторная привязка запрещена."

        cleaned = submitted_code.strip()
        if not self.pair_code or not cleaned or not secrets.compare_digest(cleaned, self.pair_code):
            return False, "❌ Неверный код привязки. Проверьте код в журнале службы или ~/.config/pipa-bot/pair_code.txt."

        self.config.owner_id = int(user_id)
        self.config.save()
        self.pair_code = None
        try:
            if self.config.pair_code_path.exists():
                self.config.pair_code_path.unlink()
        except OSError:
            pass
        return True, f"✅ Привязка выполнена! Вы назначены единственным владельцем (ID: {user_id})."
