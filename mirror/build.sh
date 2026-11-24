#!/bin/bash
# NaomiPhone Mirror.app — окно трансляции экрана телефона (docs/STREAM.md) приложением Mac, со своим значком
# (make_icon.py), в /Applications и ссылкой на рабочем столе. Electron и упаковщик — свои (mirror/node_modules): один раз
# `cd mirror && npm install` (скачивает Electron, ≈ 100 МБ). В приложении — только окно: ключ SSH и адрес Wi-Fi берутся
# из репозитория (home.json — путь к нему; поэтому приложение собирают у себя, готовым его не раздать).
#
#   mirror/build.sh        → /Applications/NaomiPhone Mirror.app и ссылка ~/Desktop/NaomiPhone Mirror
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
NAME="NaomiPhone Mirror"
ID=com.popka.naomiphone.mirror
DEST="${DEST:-/Applications}"
if [ -z "${PY:-}" ]; then
  for p in /opt/homebrew/opt/python@3.14/bin/python3.14 "$(command -v python3)"; do
    [ -x "$p" ] && { PY="$p"; break; }
  done
fi
PACKAGER="node_modules/.bin/electron-packager"
ELECTRON_VERSION="$(sed -n 's/.*"version": "\(.*\)".*/\1/p' node_modules/electron/package.json | head -1)"
[ -x "$PACKAGER" ] || { echo "нет упаковщика: сначала cd mirror && npm install" >&2; exit 1; }

[ -f icon.icns ] || "$PY" make_icon.py
printf '{ "root": "%s" }\n' "$ROOT" > home.json
trap 'rm -f home.json' EXIT

# Локальная сеть: телефон для Mac — её устройство (и по кабелю, и по Wi-Fi); macOS пускает к нему приложение (и ssh,
# который оно запускает), только когда пользователь разрешил, и спрашивает один раз
"$PACKAGER" . "$NAME" --platform=darwin --arch="$(uname -m | sed 's/x86_64/x64/')" --electron-version="$ELECTRON_VERSION" \
  --icon=icon.icns --out=dist --overwrite --app-bundle-id="$ID" \
  --app-category-type=public.app-category.developer-tools --darwin-dark-mode-support \
  --usage-description.LocalNetwork="NaomiPhone Mirror shows your phone's screen and controls it over the local network (the cable or Wi-Fi)." \
  --ignore='^/dist' --ignore='^/node_modules' --ignore='^/tests' --ignore='^/make_icon\.py' \
  --ignore='^/build\.sh$' --ignore='^/build-win\.ps1$' --ignore='^/icon\.(icns|ico|png)$' >/dev/null
BUILT="$(ls -d "dist/$NAME-darwin-"*/"$NAME.app" | head -1)"

# Свои UUID исполняемых файлов: у приложений на одном Electron они одинаковые, и macOS по UUID путает, чьё разрешение
# на локальную сеть (так одно приложение получало разрешение другого). UUID — из id приложения
# и имени файла: свой и тот же при каждой сборке (разрешение пользователя остаётся)
"$PY" - "$BUILT" "$ID" <<'EOF'
import glob, os, struct, sys, uuid
app, bundle = sys.argv[1], sys.argv[2]
exes = glob.glob(os.path.join(app, "Contents/MacOS/*")) + glob.glob(os.path.join(app, "Contents/Frameworks/*.app/Contents/MacOS/*"))
for exe in exes:
    with open(exe, "r+b") as f:
        data = bytearray(f.read())
        if data[:4] != b"\xcf\xfa\xed\xfe":
            sys.exit(f"{exe}: not a thin 64-bit Mach-O")
        ncmds, at = struct.unpack_from("<I", data, 16)[0], 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, at)
            if cmd == 0x1B:   # LC_UUID
                data[at + 8:at + 24] = uuid.uuid5(uuid.NAMESPACE_URL, bundle + "/" + os.path.basename(exe)).bytes
                break
            at += size
        else:
            sys.exit(f"{exe}: no LC_UUID")
        f.seek(0)
        f.write(data)
EOF

# подпись для этого Mac (без учётной записи Apple): собранное здесь здесь и запускается
codesign --force --deep --sign - "$BUILT" 2>/dev/null

# в /Applications: прежнее NaomiPhone Mirror.app заменяется, только если оно наше
OLD="$DEST/$NAME.app"
if [ -d "$OLD" ]; then
  [ "$(defaults read "$OLD/Contents/Info" CFBundleIdentifier 2>/dev/null)" = "$ID" ] || { echo "$OLD — не наше, оставлено как есть" >&2; exit 1; }
  rm -rf "$OLD"
fi
ditto "$BUILT" "$OLD"
touch "$OLD"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$OLD" || true
# и ссылка на рабочем столе (Finder показывает значок приложения); там что-то другое с этим именем — не трогаем
LINK="$HOME/Desktop/$NAME"
if [ -L "$LINK" ] || [ ! -e "$LINK" ]; then ln -sfn "$OLD" "$LINK"; else echo "$LINK — что-то другое, ссылки нет" >&2; fi
echo "$OLD — $(du -sh "$OLD" | cut -f1)"
