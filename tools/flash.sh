#!/bin/bash
# НаомиФон — прошивка Google Pixel 5 (redfin) с Mac.
#
#   tools/flash.sh status          что сейчас на телефоне (ADB или fastboot)
#   tools/flash.sh bootloader      перезагрузить телефон в загрузчик (fastboot)
#   tools/flash.sh stock           заводской Android 13 TQ3A.230901.001 — СТИРАЕТ ВСЁ
#   tools/flash.sh ubuntu DIR      Ubuntu Touch из DIR (boot.img vendor_boot.img dtbo.img ubuntu.img
#                                  и SHA256SUMS) поверх заводского — СТИРАЕТ userdata
#
# Порядок установки с нуля: stock, затем ubuntu. Описание — docs/INSTALL.md.
# Работает только с одним подключённым телефоном, только с redfin и только с открытым загрузчиком.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FACTORY_ZIP=$ROOT/images/factory/redfin-tq3a.230901.001-factory-308061bf.zip
FACTORY_SHA=308061bf6961ade40cae773395e01e5ca05fc8ebb554d4c56c42057b1022ec2e
FACTORY_DIR=$ROOT/images/factory/redfin-tq3a.230901.001
BOOTLOADER_IMG=bootloader-redfin-r3-0.5-9825705.img
BOOTLOADER_VER=r3-0.5-9825705
RADIO_IMG=radio-redfin-g7250-00258-230518-b-10157620.img
RADIO_VER=g7250-00258-230518-b-10157620
SYSTEM_IMAGE_ZIP=image-redfin-tq3a.230901.001.zip

say()  { printf '\033[1;33m» %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
ok()   { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }

lower() { tr '[:upper:]' '[:lower:]'; }

# Значение `fastboot getvar NAME` (fastboot пишет его в stderr).
getvar() {
  fastboot getvar "$1" 2>&1 | sed -n "s/^$1: *//p" | head -1 | tr -d '\r'
}

fastboot_serial() { fastboot devices 2>/dev/null | awk 'NF {print $1}' | head -1; }
adb_serial()      { adb devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1}' | head -1; }

one_device() {
  local n
  n=$( (fastboot devices 2>/dev/null; adb devices 2>/dev/null | awk 'NR>1 && NF') | awk 'NF' | wc -l | tr -d ' ')
  [ "$n" -le 1 ] || die "подключено несколько устройств ($n) — оставь только Pixel 5"
}

# Ждать fastboot; $1 — нужен userspace (yes = fastbootd, no = загрузчик, any).
wait_fastboot() {
  local want=${1:-any} i us
  for i in $(seq 1 120); do
    if [ -n "$(fastboot_serial)" ]; then
      us=$(getvar is-userspace)
      [ -z "$us" ] && us=no
      if [ "$want" = any ] || [ "$us" = "$want" ]; then return 0; fi
    fi
    sleep 1
  done
  die "телефон не появился в fastboot (ждали 120 с, нужен userspace=$want)"
}

check_redfin() {
  local product unlocked
  product=$(getvar product)
  unlocked=$(getvar unlocked)
  [ "$product" = redfin ] || die "это не Pixel 5: product=$product"
  [ "$unlocked" = yes ]   || die "загрузчик закрыт (unlocked=$unlocked)"
  ok "redfin, загрузчик открыт, слот $(getvar current-slot)"
}

to_bootloader() {
  one_device
  if [ -n "$(fastboot_serial)" ]; then
    [ "$(getvar product)" = redfin ] || die "загрузчик не принадлежит redfin"
    if [ "$(getvar is-userspace)" = yes ]; then fastboot reboot-bootloader; wait_fastboot no; fi
    return 0
  fi
  local serial product
  serial=$(adb_serial)
  [ -n "$serial" ] || die "телефон не виден ни в ADB, ни в fastboot. Выключи его и зажми питание + громкость вниз"
  product=$(adb -s "$serial" shell getprop ro.product.device 2>/dev/null | tr -d '\r' || true)
  [ "$product" = redfin ] || die "ADB не подтвердил redfin"
  say "ADB → перезагрузка в загрузчик"
  # Ubuntu Touch: adb reboot без root не срабатывает («Permission denied») — тогда через sudo; с паролем на sudo
  # (до device/dev-access.sh) — кнопками или самому: adb shell -t sudo reboot bootloader
  adb -s "$serial" reboot bootloader 2>&1 | grep -qi 'denied' && adb -s "$serial" shell sudo -n reboot bootloader 2>/dev/null || true
  wait_fastboot no
}

cmd_status() {
  one_device
  if [ -n "$(fastboot_serial)" ]; then
    echo "режим:        fastboot ($( [ "$(getvar is-userspace)" = yes ] && echo fastbootd || echo загрузчик))"
    for v in product unlocked current-slot version-bootloader version-baseband serialno; do
      printf '%-13s %s\n' "$v:" "$(getvar $v)"
    done
  elif [ -n "$(adb_serial)" ]; then
    echo "режим:        ADB ($(adb_serial))"
    adb shell 'uname -sr; grep PRETTY_NAME /etc/os-release 2>/dev/null; getprop ro.build.fingerprint 2>/dev/null' || true
  else
    echo "телефон не подключён"
  fi
}

cmd_stock() {
  [ -f "$FACTORY_ZIP" ] || die "нет $FACTORY_ZIP"
  say "проверка контрольной суммы заводского образа"
  echo "$FACTORY_SHA  $FACTORY_ZIP" | shasum -a 256 -c - >/dev/null || die "SHA-256 заводского образа не совпал"
  ok "заводской образ цел"
  if [ ! -f "$FACTORY_DIR/$SYSTEM_IMAGE_ZIP" ]; then
    (cd "$(dirname "$FACTORY_ZIP")" && unzip -o -q "$FACTORY_ZIP")
  fi

  to_bootloader
  check_redfin
  fastboot set_active a

  if [ "$(getvar version-bootloader | lower)" = "$BOOTLOADER_VER" ]; then
    ok "загрузчик уже $BOOTLOADER_VER — не трогаем"
  else
    say "прошивка загрузчика $BOOTLOADER_VER"
    fastboot flash bootloader "$FACTORY_DIR/$BOOTLOADER_IMG"
    fastboot reboot-bootloader; sleep 5; wait_fastboot no
  fi
  if [ "$(getvar version-baseband | lower)" = "$RADIO_VER" ]; then
    ok "модем уже $RADIO_VER — не трогаем"
  else
    say "прошивка модема $RADIO_VER"
    fastboot flash radio "$FACTORY_DIR/$RADIO_IMG"
    fastboot reboot-bootloader; sleep 5; wait_fastboot no
  fi

  say "заводской Android 13 со стиранием данных (5–10 минут)"
  fastboot -w --skip-reboot update "$FACTORY_DIR/$SYSTEM_IMAGE_ZIP"
  ok "заводской Android 13 записан"
}

cmd_ubuntu() {
  local dir=${1:-}
  [ -n "$dir" ] && [ -d "$dir" ] || die "укажи каталог с образами Ubuntu Touch"
  dir=$(cd "$dir" && pwd)
  for f in boot.img vendor_boot.img dtbo.img ubuntu.img SHA256SUMS; do
    [ -f "$dir/$f" ] || die "нет $dir/$f"
  done
  say "проверка контрольных сумм образов Ubuntu Touch"
  (cd "$dir" && shasum -a 256 -c SHA256SUMS) || die "контрольные суммы не совпали"

  one_device
  [ -n "$(fastboot_serial)" ] || to_bootloader
  check_redfin
  fastboot set_active a

  if [ "$(getvar is-userspace)" != yes ]; then
    say "переход в fastbootd (логические разделы)"
    fastboot reboot fastboot
    wait_fastboot yes
  fi
  for p in product_a system_ext_a; do
    if fastboot getvar "partition-size:$p" 2>&1 | grep -q "^partition-size:$p: *0x"; then
      fastboot delete-logical-partition "$p"
    else
      ok "$p уже удалён"
    fi
  done
  say "запись ubuntu.img в system_a"
  fastboot -S 256M flash system "$dir/ubuntu.img"
  fastboot erase userdata

  say "загрузчик: boot, vendor_boot, dtbo"
  fastboot reboot-bootloader
  wait_fastboot no
  fastboot flash boot "$dir/boot.img"
  fastboot flash vendor_boot "$dir/vendor_boot.img"
  fastboot flash dtbo "$dir/dtbo.img"
  fastboot set_active a
  ok "Ubuntu Touch записан. Перезагрузка — первый запуск идёт несколько минут"
  fastboot reboot
}

case "${1:-}" in
  status)     cmd_status ;;
  bootloader) to_bootloader; check_redfin ;;
  stock)      cmd_stock ;;
  ubuntu)     shift; cmd_ubuntu "${1:-}" ;;
  *)          sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
