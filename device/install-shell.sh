#!/bin/sh
# НаомиФон — установить оболочку в систему телефона (от root) или убрать.
#   sudo sh install-shell.sh install     службы naomid, naomi-shell и naomi-safe, автозапуск, repowerd — выкл.
#   sudo sh install-shell.sh uninstall   вернуть как было (кроме выключенного Lomiri — это lomiri.sh)
# Файлы берёт из того же каталога (их кладёт tools/naomi install).
# Сама оболочка — сборка в /home/phablet/naomi/build (tools/naomi build); службе нужен только путь.
# Служба железа naomid работает от root — её программа копируется из сборки в $LIB (из домашней папки
# root ничего не запускает).
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
LIB=/usr/local/lib/naomi
UNITS=/etc/systemd/system          # на Ubuntu Touch — в разделе данных, пишется без remount
BUILD=/home/phablet/naomi/build
BUS_POLICY=/etc/dbus-1/system.d/org.naomi.Phone.conf
TOUCH_RULE=/etc/udev/rules.d/90-naomi-touch.rules
PA_DROPIN=/etc/systemd/user/pulseaudio.service.d/90-naomi.conf
NM_WLAN1=/etc/NetworkManager/conf.d/90-naomi-wlan1.conf
BT_DROPIN=/etc/systemd/system/bluetooth.service.d/90-naomi.conf
# Пакеты, которых нет в системе, а оболочке-композитору нужны: Qt Wayland Compositor (библиотеки, QML,
# серверный плагин wayland-egl из qt6-wayland). Берутся .deb из кэша среды сборки (devroot.sh packages):
# той же версии, что в среде, без отдельного скачивания.
RUNTIME="libqt6waylandcompositor6 libqt6waylandcompositorxdgshell6 libqt6waylandcompositorpresentationtime6
  libqt6waylandcompositoriviapplication6 libqt6waylandcompositorwlshell6 libqt6wlshellintegration6
  qml6-module-qtwayland-compositor qt6-wayland"
DEBS=/userdata/naomi/devroot/var/cache/apt/archives

# Ставим только то, чего нет или другой версии: переустановка подменяет библиотеки под работающей
# оболочкой, их удалённые копии держат корень, и он не возвращается в «только чтение».
install_runtime() {
    files=""
    for p in $RUNTIME; do
        f=$(ls "$DEBS/${p}_"*_arm64.deb 2>/dev/null | tail -1)
        [ -n "$f" ] || { echo "нет $p в $DEBS — сначала devroot.sh packages" >&2; exit 1; }
        [ "$(dpkg-query -W -f='${Version}' "$p" 2>/dev/null)" = "$(dpkg-deb -f "$f" Version)" ] || files="$files $f"
    done
    [ -z "$files" ] || dpkg -i $files >/dev/null
    echo "рантайм композитора: $(echo $RUNTIME | wc -w) пакетов, поставлено сейчас $(echo $files | wc -w)"
}

rw() {
    case ",$(findmnt -n -o OPTIONS /)," in *,rw,*) return ;; esac
    mount -o remount,rw /
    trap 'sync; mount -o remount,ro / || echo "внимание: корень остался на запись" >&2' EXIT
}

case "${1:-}" in
install)
    [ -x "$BUILD/naomid/naomid" ] || { echo "нет $BUILD/naomid/naomid — сначала соберите оболочку" >&2; exit 1; }
    TARGET=$(timeout 5 runuser -u phablet -- "$BUILD/naomid/naomid" --device)
    case "$TARGET" in pipa|redfin) ;; *) echo "неизвестное устройство: $TARGET" >&2; exit 1 ;; esac
    rw
    if [ "$TARGET" = redfin ]; then
        # Журнал systemd — в памяти: на корне (3,4 ГБ, только для чтения) он дорос до 216 МБ и забил его
        # до 100% — писал, пока корень был открыт на запись.
        mkdir -p /etc/systemd/journald.conf.d
        printf '# НаомиФон: журнал в памяти (корень маленький и только для чтения)\n[Journal]\nStorage=volatile\nRuntimeMaxUse=64M\n' \
            > /etc/systemd/journald.conf.d/90-naomi.conf
        rm -rf /var/log/journal/*
        systemctl restart systemd-journald
    elif [ "$TARGET" = pipa ]; then
        # /var/log на pipa лежит на 52 ГБ /userdata: zz-90-naomi.conf перекрывает lxc-android-config.conf (500M).
        mkdir -p /etc/systemd/journald.conf.d
        rm -f /etc/systemd/journald.conf.d/pipa-size.conf /etc/systemd/journald.conf.d/90-naomi.conf
        printf '# НаомиФон (pipa): единый лимит журнала на /userdata\n[Journal]\nSystemMaxUse=100M\nRuntimeMaxUse=32M\n' \
            > /etc/systemd/journald.conf.d/zz-90-naomi.conf
        systemctl restart systemd-journald || true
    fi
    install_runtime
    mkdir -p "$LIB"
    install -m 755 "$SRC/shell-prepare" "$LIB/shell-prepare"
    install -m 755 "$SRC/naomi-telegram" "$LIB/naomi-telegram"
    install -m 755 "$SRC/naomi-deepseek" "$LIB/naomi-deepseek"
    [ -f "$SRC/naomi-camera" ] && install -m 755 "$SRC/naomi-camera" "$LIB/naomi-camera"
    install -m 755 "$BUILD/naomid/naomid" "$LIB/naomid"
    if [ "$TARGET" = pipa ]; then
        install -m 755 "$SRC/naomi-pipa-slot-switch" "$LIB/naomi-pipa-slot-switch"
    fi
    printf 'NAOMI_DEVICE=%s\n' "$TARGET" > "$LIB/device.env"

    # шина: имя org.naomi.Phone — только root, звать и слушать службу — только phablet и root
    cat > "$BUS_POLICY" <<'EOF'
<!-- НаомиФон — служба железа naomid (docs/NAOMID.md) -->
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy context="default">
    <deny send_destination="org.naomi.Phone"/>
    <deny receive_sender="org.naomi.Phone"/>
  </policy>
  <policy user="root">
    <allow own="org.naomi.Phone"/>
    <allow send_destination="org.naomi.Phone"/>
    <allow receive_sender="org.naomi.Phone"/>
  </policy>
  <policy user="phablet">
    <allow send_destination="org.naomi.Phone"/>
    <allow receive_sender="org.naomi.Phone"/>
  </policy>
</busconfig>
EOF
    systemctl reload dbus

    cat > "$UNITS/naomid.service" <<'EOF'
# НаомиФон — служба железа (docs/NAOMID.md)
[Unit]
Description=NaomiPhone hardware service
After=dbus.service NetworkManager.service ofono.service upower.service bluetooth.service
Before=naomi-shell.service

[Service]
Type=dbus
BusName=org.naomi.Phone
EnvironmentFile=/usr/local/lib/naomi/device.env
# звук — в сеансе phablet: службе нужен его ключ PulseAudio, причём открываемый на запись (иначе
# libpulse его не берёт), а дом ей — только читать. Копия ключа — в /run/naomid (до запуска, без ограничений)
RuntimeDirectory=naomid
RuntimeDirectoryMode=0700
ExecStartPre=-+/usr/bin/install -m 600 /home/phablet/.config/pulse/cookie /run/naomid/pulse-cookie
ExecStart=/usr/local/lib/naomi/naomid
Restart=on-failure
RestartSec=1
NoNewPrivileges=yes
ProtectHome=read-only
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
    if [ "$TARGET" = redfin ]; then
        mkdir -p /etc/systemd/logind.conf.d
        install -m 644 "$SRC/safescreen.py" "$SRC/hwcspike.py" "$SRC/blackbox.py" "$LIB/"
        # кнопку питания обрабатывает оболочка; иначе logind по ней выключает телефон
        printf '# НаомиФон: кнопку питания обрабатывает оболочка (naomi-shell), не logind\n[Login]\nHandlePowerKey=ignore\nHandlePowerKeyLongPress=ignore\n' \
            > /etc/systemd/logind.conf.d/90-naomi.conf
        # второй интерфейс Wi-Fi (wlan1) — без NetworkManager: тот, пока wlan1 ни к чему не подключён, сам запускал на нём
        # поиск сетей — радио на 3 с уходило с рабочего канала, и трансляция стола по Wi-Fi вставала на 50–130 мс по
        # нескольку раз подряд (docs/STREAM.md, «Сканы wlan1»). Сеть телефона — wlan0, wlan1 ничем не занят
        mkdir -p /etc/NetworkManager/conf.d
        printf '%s\n' '# НаомиФон: wlan1 не занят — без поиска сетей на нём (docs/STREAM.md, «Сканы wlan1»)' '[keyfile]' \
            'unmanaged-devices=interface-name:wlan1' > "$NM_WLAN1"
        nmcli general reload conf >/dev/null 2>&1 || true
        nmcli device set wlan1 managed no >/dev/null 2>&1 || true
        # последний ряд сенсора (y 2339) — внутрь окна оболочки: Qt переводит его в точку 850,55 при окне в 851 точку и
        # округляет до 851 — касание «ниже окна» выбрасывается целиком, и смах «домой» от самого края не срабатывал
        # (docs/SHELL.md, «Жест от самого края»). Сжатие по высоте на 0,05 % — внизу меньше точки панели.
        printf '%s\n' '# НаомиФон: последний ряд сенсора — внутрь окна оболочки (docs/SHELL.md, «Жест от самого края»)' \
            'ACTION=="add|change", KERNEL=="event*", ATTRS{name}=="sec_touchscreen|naomi-virtual-touch", ENV{LIBINPUT_CALIBRATION_MATRIX}="1 0 0 0 0.9995 0"' \
            > "$TOUCH_RULE"
        udevadm control --reload
        udevadm trigger --action=change --subsystem-match=input
        # Громкость динамика — в PulseAudio, а не в драйвере (hw_volume=false у pulseaudio-modules-droid): драйвер Qualcomm
        # громкость, присланную, пока поток спит, при его запуске не применяет («Playback 9 Volume» = 0) — динамик после
        # 5 с тишины играл на 100 %, а шторка показывала, например, 20 % (docs/NAOMID.md, «Громкость динамика»).
        # Действует со следующего запуска PulseAudio (перезагрузка или systemctl --user restart pulseaudio)
        # Клавиатуры и мыши Bluetooth: в ядре порта нет HIDP (CONFIG_BT_HIDP), BlueZ ведёт их сам через /dev/uhid, а тот —
        # system:system 0660 (так его ставит Android). bluetoothd — root без CAP_DAC_OVERRIDE и без групп: открыть не мог
        # и молча уходил на HIDP («ioctl_connadd(): Protocol not supported») — docs/DESKTOP.md, «Клавиатура Bluetooth».
        # Действует со следующего запуска bluetoothd (перезагрузка или systemctl restart bluetooth)
        mkdir -p "$(dirname "$BT_DROPIN")"
        printf '%s\n' '# НаомиФон: bluetoothd открывает /dev/uhid (system:system) — клавиатуры и мыши Bluetooth' \
            '[Service]' 'SupplementaryGroups=system' > "$BT_DROPIN"
        mkdir -p "$(dirname "$PA_DROPIN")"
        printf '%s\n' '# НаомиФон: громкость динамика — в PulseAudio, не в драйвере (docs/NAOMID.md, «Громкость динамика»)' \
            '[Service]' 'Environment=PULSE_MODULES_DROID_EXTRA_CARD_ARGS=hw_volume=false' > "$PA_DROPIN"
        runuser -u phablet -- env XDG_RUNTIME_DIR=/run/user/32011 systemctl --user daemon-reload || true
    fi

    cat > "$UNITS/naomi-shell.service" <<'EOF'
# НаомиФон — оболочка (docs/SHELL.md, «Служба»)
[Unit]
Description=NaomiPhone shell
After=lxc-android-config.service user@32011.service
Wants=lxc-android-config.service user@32011.service
Conflicts=naomi-safe.service
StartLimitIntervalSec=60
StartLimitBurst=4
OnFailure=naomi-safe.service

[Service]
Type=simple
Nice=-5
User=phablet
Group=phablet
SupplementaryGroups=android_input android_graphics video audio input
WorkingDirectory=/home/phablet/naomi
Environment=LANG=C.UTF-8 LC_ALL=C.UTF-8
Environment=EGL_PLATFORM=hwcomposer QT_QPA_PLATFORM=eglfs QT_QPA_EGLFS_INTEGRATION=eglfs_naomi
Environment=QT_PLUGIN_PATH=/home/phablet/naomi/build/plugins
Environment=XDG_RUNTIME_DIR=/run/user/32011 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
Environment=NAOMI_FRAMESTATS=1
EnvironmentFile=/usr/local/lib/naomi/device.env
EnvironmentFile=-/home/phablet/naomi/shell.env
ExecStartPre=+/usr/local/lib/naomi/shell-prepare
ExecStart=/home/phablet/naomi/build/app/naomi-shell
# Оболочка закрепляет в памяти свой код (shell/app/code_pin.h): под нехватку памяти он не уходит на диск
LimitMEMLOCK=infinity
# Группа процессов службы — оболочке: программы стола (браузер) живут в её подгруппе desk, и стол без зрителя
# замораживает их ядром (docs/DESKTOP.md, «Выключатель»). Перед остановкой — оттаять: замороженная программа SIGTERM
# не прочтёт, и остановка (и перезапуск после падения) ждала бы её до SIGKILL
Delegate=yes
ExecStop=-/bin/sh -c 'if [ -w /sys/fs/cgroup/system.slice/naomi-shell.service/desk/cgroup.freeze ]; then echo 0 > /sys/fs/cgroup/system.slice/naomi-shell.service/desk/cgroup.freeze; fi'
Restart=on-failure
RestartSec=1
# 75 — оболочка просит перезапуск (новая ширина кадра из «Настроек»): не падение
RestartForceExitStatus=75
SuccessExitStatus=75

[Install]
WantedBy=graphical.target
EOF
    if [ "$TARGET" = pipa ]; then
        sed -i '/^OnFailure=naomi-safe.service$/d' "$UNITS/naomi-shell.service"
        sed -i '/^\[Unit\]$/a After=naomi-pipa-awake.service\nConflicts=repowerd.service\nWants=naomi-pipa-awake.service' \
            "$UNITS/naomi-shell.service"
        cat > "$UNITS/naomi-pipa-awake.service" <<'EOF'
[Unit]
Description=Keep the Xiaomi Pad 6 awake during the NaomiPhone session
After=systemd-logind.service
PartOf=naomi-shell.service
StopWhenUnneeded=yes

[Service]
# Удерживаем также блокировку autosleep ядра Halium, если интерфейс доступен.
ExecStartPre=-/bin/sh -c 'if [ -w /sys/power/wake_lock ] && [ -w /sys/power/wake_unlock ]; then echo naomi-pipa > /sys/power/wake_lock; fi'
ExecStart=/usr/bin/systemd-inhibit --what=sleep:idle:handle-power-key --mode=block --who=NaomiPhone --why "Tablet display session" /usr/bin/sleep infinity
ExecStopPost=-/bin/sh -c 'if [ -w /sys/power/wake_unlock ]; then echo naomi-pipa > /sys/power/wake_unlock; fi'
EOF
        # Чистый сброс буферов userdata.img (loop0) и f2fs /mnt/android-host-data перед ребутом в One UI (_a).
        mkdir -p /usr/lib/systemd/system-shutdown
        cat > /usr/lib/systemd/system-shutdown/naomi-pipa-sync <<'EOF'
#!/bin/sh
/bin/sync 2>/dev/null || sync 2>/dev/null || true
mount -o remount,ro /userdata 2>/dev/null || true
mount -o remount,ro /mnt/android-host-data 2>/dev/null || true
/bin/sync 2>/dev/null || sync 2>/dev/null || true
exit 0
EOF
        chmod 755 /usr/lib/systemd/system-shutdown/naomi-pipa-sync
        # Маскируем конфликтующие службы Ubuntu Touch: keep-display-on-during-boot тянет repowerd
        # и держит graphical.target 300 секунд при выключенном lightdm; dpkg-db-backup падает на RO-корне.
        systemctl mask repowerd.service keep-display-on-during-boot.service >/dev/null 2>&1 || true
        systemctl stop motd-news.timer dpkg-db-backup.timer >/dev/null 2>&1 || true
        systemctl mask motd-news.timer dpkg-db-backup.timer dpkg-db-backup.service >/dev/null 2>&1 || true
        systemctl daemon-reload
        systemctl enable naomid.service naomi-shell.service >/dev/null 2>&1 || true
        systemctl restart naomid.service
        echo "pipa: установлено naomid ($(systemctl is-active naomid)), naomi-shell (автозапуск); repowerd/keep-display-on/dpkg-db-backup выключены"
        exit 0
    fi

    cat > "$UNITS/naomi-safe.service" <<'EOF'
# НаомиФон — безопасный режим: оболочка упала 4 раза за минуту. Экран-заглушка, SSH и ADB работают.
# Выйти: tools/naomi run (оболочка стартует и гасит заглушку).
[Unit]
Description=NaomiPhone safe mode screen
Conflicts=naomi-shell.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/lib/naomi/safescreen.py
EOF

    cat > "$UNITS/naomi-blackbox.service" <<'EOF'
# НаомиФон — «чёрный ящик» (docs/XRAY.md): раз в минуту состояние телефона — в /userdata/naomi/blackbox.
# Журнал systemd у нас в памяти; запись на диске переживает перезагрузку. Разбор — tools/naomi blackbox
[Unit]
Description=NaomiPhone black box recorder

[Service]
ExecStart=/usr/bin/python3 /usr/local/lib/naomi/blackbox.py
Restart=always
RestartSec=10
Nice=10

[Install]
WantedBy=multi-user.target
EOF

    systemctl mask repowerd.service >/dev/null
    # «новости дня» Ubuntu: раз в сутки ходит на сервер Canonical за текстом для входа в консоль — показывать его негде
    # (mask, а не disable: таймер включён ссылкой в /usr/lib — после перезагрузки disable с него слетал)
    systemctl stop motd-news.timer >/dev/null 2>&1 || true
    systemctl mask motd-news.timer >/dev/null 2>&1 || true
    systemctl daemon-reload
    systemctl kill -s HUP systemd-logind
    systemctl enable naomid.service naomi-shell.service naomi-blackbox.service >/dev/null 2>&1
    systemctl restart naomid.service naomi-blackbox.service
    echo "установлено: naomid ($(systemctl is-active naomid)), naomi-shell (автозапуск), naomi-safe; repowerd выключен"
    ;;
uninstall)
    rw
    if [ "$(cat "$LIB/device.env" 2>/dev/null || true)" = NAOMI_DEVICE=pipa ]; then
        systemctl disable --now naomi-shell.service naomid.service >/dev/null 2>&1 || true
        systemctl stop naomi-pipa-awake.service >/dev/null 2>&1 || true
        rm -f "$UNITS/naomi-shell.service" "$UNITS/naomid.service" "$UNITS/naomi-pipa-awake.service" \
              "$BUS_POLICY" /usr/lib/systemd/system-shutdown/naomi-pipa-sync \
              /etc/systemd/journald.conf.d/90-naomi.conf
        rm -rf "$LIB"
        systemctl unmask repowerd.service keep-display-on-during-boot.service motd-news.timer \
                         dpkg-db-backup.timer dpkg-db-backup.service >/dev/null 2>&1 || true
        systemctl daemon-reload
        systemctl reload dbus
        exit 0
    fi
    systemctl disable --now naomi-shell.service naomid.service naomi-blackbox.service >/dev/null 2>&1 || true
    systemctl stop naomi-safe.service 2>/dev/null || true
    # пороги заряда, что ставила naomid (docs/NAOMID.md, «Заряд»), — как с завода: без предела, защита ядра от жары
    # «вхолостую»
    CHG=/sys/devices/platform/soc/soc:google,charger
    echo 0 > "$CHG/charge_start_level" 2>/dev/null || true
    echo 100 > "$CHG/charge_stop_level" 2>/dev/null || true
    echo 1 > "$CHG/bd_temp_dry_run" 2>/dev/null || true
    rm -f "$UNITS/naomi-shell.service" "$UNITS/naomi-safe.service" "$UNITS/naomid.service" "$UNITS/naomi-blackbox.service" \
          /etc/systemd/logind.conf.d/90-naomi.conf /etc/systemd/journald.conf.d/90-naomi.conf "$BUS_POLICY" "$TOUCH_RULE" \
          "$PA_DROPIN" "$NM_WLAN1" "$BT_DROPIN"
    nmcli general reload conf >/dev/null 2>&1 || true
    nmcli device set wlan1 managed yes >/dev/null 2>&1 || true
    udevadm control --reload
    systemctl reload dbus
    rm -rf "$LIB"
    systemctl unmask repowerd.service >/dev/null
    systemctl unmask motd-news.timer >/dev/null 2>&1 || true
    systemctl daemon-reload
    systemctl kill -s HUP systemd-logind
    echo "убрано; repowerd вернётся при загрузке"
    ;;
*)
    echo "использование: $0 install|uninstall" >&2
    exit 2
    ;;
esac
