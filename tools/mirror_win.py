#!/usr/bin/env python3
"""Launch NaomiPhone Mirror over ADB or Wi-Fi without a shell wrapper."""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MIRROR = ROOT / "mirror"
STREAM_PORT = 8478  # docs/STREAM.md, shell/stream/stream_protocol.h
PRODUCT = "NaomiPhone Mirror"


class LauncherError(Exception):
    pass


def port_number(value):
    try:
        port = int(value)
    except ValueError:
        port = 0
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("порт должен быть числом от 1 до 65535")
    return port


def host_name(value):
    try:
        return str(ipaddress.ip_address(value))
    except ValueError:
        if re.fullmatch(r"[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9.])?", value):
            return value
        raise argparse.ArgumentTypeError("укажите IP-адрес или DNS-имя устройства")


def saved_host():
    try:
        info = json.loads((ROOT / "private" / "device.json").read_text(encoding="utf-8-sig"))
        if isinstance(info, dict) and isinstance(info.get("wifi_ip"), str):
            return host_name(info["wifi_ip"])
    except (OSError, ValueError, argparse.ArgumentTypeError):
        pass
    return None


def adb_executable():
    on_path = shutil.which("adb")
    if on_path:
        return on_path
    default_win = Path(r"C:\adb\adb.exe")
    if sys.platform == "win32" and default_win.is_file():
        return str(default_win)
    raise LauncherError("adb не найден. Добавьте Android Platform Tools в PATH")


def adb(adb_path, *args):
    try:
        result = subprocess.run(
            [adb_path, *args], check=True, capture_output=True,
            encoding="utf-8", errors="replace", timeout=15,
        )
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise LauncherError(detail or str(error)) from error
    except (OSError, subprocess.TimeoutExpired) as error:
        raise LauncherError(str(error)) from error
    return result.stdout.strip()


def usb_serial(adb_path, requested):
    rows = (line.split() for line in adb(adb_path, "devices").splitlines())
    devices = {
        row[0]: row[1] for row in rows if len(row) >= 2 and row[0] not in ("List", "*")
        and not row[0].startswith("emulator-") and ":" not in row[0]
        and not row[0].endswith("._adb-tls-connect._tcp")
    }
    if requested is not None:
        if devices.get(requested) != "device":
            state = devices.get(requested, "нет в списке")
            raise LauncherError(f"USB-устройство {requested} недоступно в ADB ({state})")
        return requested
    ready = [serial for serial, state in devices.items() if state == "device"]
    if len(ready) > 1:
        raise LauncherError("несколько ADB-устройств. Укажите --adb SERIAL")
    if not ready:
        raise LauncherError("ADB не видит готовое USB-устройство")
    return ready[0]


def launch_command(exe):
    if exe is not None:
        if not exe.is_file():
            raise LauncherError(f"приложение не найдено: {exe}")
        return [str(exe.resolve())]
    if (MIRROR / "node_modules" / "electron" / "package.json").is_file():
        # Electron 45 can download its binary lazily on the first require.
        # Resolve it before reserving an ADB port and wait on the native app itself.
        result = subprocess.run(
            ["node", "-p", "require('electron')"], cwd=MIRROR, check=False,
            stdout=subprocess.PIPE, encoding="utf-8", errors="replace",
        )
        lines = result.stdout.splitlines()
        if result.returncode == 0 and lines and Path(lines[-1]).is_file():
            return [lines[-1], str(MIRROR)]
        raise LauncherError("не удалось подготовить Electron. Проверьте npm install в папке mirror")
    packaged = MIRROR / "dist" / f"{PRODUCT}-win32-x64" / f"{PRODUCT}.exe"
    if sys.platform == "win32" and packaged.is_file():
        return [str(packaged)]
    raise LauncherError("Electron не найден. Выполните npm install в папке mirror")


def run(args):
    command = launch_command(args.exe)
    host = None if args.via == "usb" else (args.host or saved_host())
    local_port = None
    try:
        if args.via != "wifi":
            try:
                executable = adb_executable()
                serial = usb_serial(executable, args.adb)
                output = adb(executable, "-s", serial, "forward", "tcp:0", f"tcp:{args.port}")
                try:
                    local_port = port_number(output)
                except argparse.ArgumentTypeError as error:
                    raise LauncherError("adb forward не вернул номер локального порта") from error
            except LauncherError as error:
                if args.via == "usb":
                    raise
                if not host:
                    raise LauncherError(f"{error}. Для Wi-Fi укажите --host IP") from error
                print(f"USB — {error}. Подключаюсь по Wi-Fi к {host}", file=sys.stderr)
        if local_port is None and not host:
            raise LauncherError("укажите --host IP или сохраните wifi_ip в private/device.json")

        env = os.environ.copy()
        for key in ("NAOMI_MIRROR_HOST", "NAOMI_MIRROR_USB_PORT", "ELECTRON_RUN_AS_NODE"):
            env.pop(key, None)
        env.update(
            NAOMI_MIRROR_ROOT=str(ROOT),
            NAOMI_MIRROR_VIA=args.via if local_port is not None else "wifi",
            NAOMI_MIRROR_PORT=str(args.port),
        )
        if host:
            env["NAOMI_MIRROR_HOST"] = host
        if local_port is not None:
            env["NAOMI_MIRROR_USB_PORT"] = str(local_port)
        return subprocess.run(command, cwd=MIRROR, env=env, check=False).returncode
    finally:
        if local_port is not None:
            try:
                adb(executable, "-s", serial, "forward", "--remove", f"tcp:{local_port}")
            except LauncherError as error:
                print(f"Не удалось убрать проброс ADB tcp:{local_port} — {error}", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description="NaomiPhone Mirror через USB (ADB) или Wi-Fi (SSH).")
    parser.add_argument("--via", choices=("auto", "usb", "wifi"), default="auto",
                        help="auto сначала пробует USB, затем Wi-Fi")
    parser.add_argument("--adb", metavar="SERIAL", help="серийный номер USB-устройства из ADB")
    parser.add_argument("--host", type=host_name, metavar="IP", help="IP или DNS-имя для SSH по Wi-Fi")
    parser.add_argument("--port", type=port_number, default=STREAM_PORT, help="порт трансляции на устройстве (8478)")
    parser.add_argument("--exe", type=Path, metavar="PATH", help="путь к собранному приложению Windows")
    args = parser.parse_args(argv)
    try:
        return run(args)
    except (LauncherError, OSError) as error:
        print(f"NaomiPhone Mirror — {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
