#!/usr/bin/env python3
"""Проверить pipa или передать и собрать PopkaPhone в Ubuntu Touch 24.04 или 26.04."""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUARD = r'''
set -eu
die() { printf '%s\n' "$*" >&2; exit 1; }

identities=$(
    if command -v getprop >/dev/null 2>&1; then
        for name in ro.product.device ro.product.vendor.device ro.product.odm.device; do
            getprop "$name" 2>/dev/null || true
        done
    fi
    if [ -f /etc/deviceinfo ] && [ -r /etc/deviceinfo ]; then
        sed -nE 's/^[[:space:]]*(export[[:space:]]+)?(deviceinfo_codename|DeviceCodename|DeviceName|Name)[[:space:]]*=[[:space:]]*//Ip' /etc/deviceinfo
    fi
    for model in /proc/device-tree/model /sys/firmware/devicetree/base/model; do
        if [ -r "$model" ]; then tr -d '\000' < "$model"; printf '\n'; fi
    done
    if [ -f /etc/system-image/channel.ini ] && [ -r /etc/system-image/channel.ini ]; then
        sed -nE 's/^[[:space:]]*device[[:space:]]*:[[:space:]]*([a-z0-9_-]+)[[:space:]]*([#;].*)?$/\1/Ip' /etc/system-image/channel.ini
    fi
)
printf '%s\n' "$identities" | grep -Eiq '(^|[^a-z0-9_-])pipa([^a-z0-9_-]|$)' || die 'Устройство pipa не подтверждено'
if printf '%s\n' "$identities" | grep -Eiq '(^|[^a-z0-9_-])redfin([^a-z0-9_-]|$)'; then
    die 'Обнаружен redfin'
fi
os_release=/etc/os-release
[ -r "$os_release" ] || os_release=/usr/lib/os-release
os_has() { grep -Eq "^$1=($2|\"$2\"|'$2')\$" "$os_release"; }
os_error='Нужен Ubuntu Touch 24.04 или 26.04 arm64'
[ -r "$os_release" ] && os_has ID ubuntu && [ "$(uname -m)" = aarch64 ] || die "$os_error"
if os_has VERSION_ID '26\.04'; then
    release=26.04
elif os_has VERSION_ID '24\.04'; then
    release=24.04
else
    die "$os_error"
fi
if ! os_has VARIANT_ID touch && ! os_has VARIANT Touch; then
    # На старых образах 24.04 признаком Ubuntu Touch служит Lomiri.
    [ "$release" = 24.04 ] && command -v lomiri >/dev/null 2>&1 || die "$os_error"
fi

slots=$(
    if command -v getprop >/dev/null 2>&1; then
        getprop ro.boot.slot_suffix 2>/dev/null || true
        getprop ro.boot.slot 2>/dev/null || true
    fi
    if [ -r /proc/cmdline ]; then
        tr ' ' '\n' < /proc/cmdline | sed -nE 's/^androidboot\.slot(_suffix)?=//p'
    fi
    if [ -r /proc/bootconfig ]; then
        sed -nE 's/^androidboot\.slot(_suffix)?[[:space:]]*=[[:space:]]*//p' /proc/bootconfig
    fi
)
slots=$(printf '%s\n' "$slots" | tr -d '"\r' | sed '/^[[:space:]]*$/d; s/[[:space:]]*$//')
[ -n "$slots" ] || die 'Активный слот не подтверждён'
if printf '%s\n' "$slots" | grep -Eqv '^_?b$'; then
    die 'Допустим только подтверждённый активный слот _b'
fi
printf 'pipa · Ubuntu Touch %s arm64 · слот _b\n' "$release"
'''


def target(value):
    # Серийный номер ADB и адрес или алиас SSH передаются аргументами, без локальной оболочки.
    if not re.fullmatch(r"[a-zA-Z0-9_.:\[\]%][a-zA-Z0-9_.:\[\]%-]*", value):
        raise argparse.ArgumentTypeError("укажите серийный номер ADB, адрес или алиас SSH")
    return value


def ssh_command(host, script):
    return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
            "-l", "phablet", host, "sh -c " + shlex.quote(script)]


def stage_script(revision, jobs=None):
    script = GUARD + rf'''
[ "$(id -un)" = phablet ] || die 'Войдите по SSH как phablet'
umask 077
base=/home/phablet/naomi
mkdir -p "$base/src"
source="$base/src/pipa-{revision}"
temp=$(mktemp -d "$base/src/.pipa-XXXXXX")
trap 'rm -rf -- "$temp"' EXIT
tar -xf - -C "$temp"
if [ -e "$source" ]; then
    [ ! -L "$source" ] && diff -qr "$temp" "$source" || die 'Каталог этой версии уже существует с другими файлами'
else
    mv -T -- "$temp" "$source"
fi
printf 'Исходники: %s\n' "$source"
'''
    if jobs is not None:
        script += rf'''
build="$base/build-pipa-{revision}"
[ -f /userdata/naomi/devroot/etc/os-release ] || die 'Сначала подготовьте devroot по docs/PIPA.md'
(
    flock 9
    sudo -n sh "$source/device/devroot.sh" user \
        "cmake -S $source/shell -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build $build --parallel {jobs}"
) 9>"$base/.pipa-build.lock"
printf 'Сборка: %s\n' "$build"
'''
    return script


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="проверить модель, систему и активный слот")
    transport = check.add_mutually_exclusive_group(required=True)
    transport.add_argument("--adb", type=target, metavar="SERIAL")
    transport.add_argument("--ssh", type=target, metavar="HOST")
    stage = commands.add_parser("stage", help="передать исходники текущего коммита")
    stage.add_argument("--ssh", type=target, required=True, metavar="HOST")
    build = commands.add_parser("build", help="передать исходники и собрать их в готовом devroot")
    build.add_argument("--ssh", type=target, required=True, metavar="HOST")
    build.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    args = parser.parse_args()

    try:
        if args.command == "check":
            command = (["adb", "-s", args.adb, "shell", "-T", "sh -c " + shlex.quote(GUARD)]
                       if args.adb else ssh_command(args.ssh, GUARD))
            subprocess.run(command, check=True, timeout=25)
        else:
            revision = subprocess.check_output(["git", "rev-parse", "--verify", "HEAD"], cwd=ROOT, text=True).strip()
            if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", revision):
                raise ValueError("не удалось определить коммит Git")
            archive = subprocess.check_output(
                ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
                 "archive", "--format=tar", revision, "shell", "device"], cwd=ROOT,
            )
            script = stage_script(revision, args.jobs if args.command == "build" else None)
            subprocess.run(ssh_command(args.ssh, script), input=archive, check=True)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        # Исключение CalledProcessError включает весь удалённый сценарий. Достаточно статуса и его stderr.
        if isinstance(error, subprocess.CalledProcessError):
            print(f"pipa: команда завершилась с кодом {error.returncode}", file=sys.stderr)
        elif isinstance(error, subprocess.TimeoutExpired):
            print("pipa: проверка не ответила за 25 секунд", file=sys.stderr)
        else:
            print(f"pipa: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
