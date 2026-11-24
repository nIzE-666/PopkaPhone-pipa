# Xiaomi Pad 6 (`pipa`)

Цель первого этапа — рабочий стол ПопкаФона на встроенном экране Xiaomi Pad 6 с Ubuntu Touch 26.04 LTS
(`resolute`) в слоте `_b`. Android One UI 8 загружается из `_a`. Сведения о конфигурации взяты из замеров владельца.
Сборка первой версии для arm64 прошла 120 из 120 шагов. Второй этап устраняет проблемы, замеченные при её запуске.

## Конфигурация планшета

| Узел | Измеренная конфигурация |
|---|---|
| Система | Ubuntu Touch 26.04 LTS, канал `utnext/arm64/android9plus/daily`, `device: pipa`. Halium 13, порт `thespider2/ubuntu-touch-xiaomi-pipa` v0.2.0, ядро 4.19 из `aymanrgab/linux-android-xiaomi-pipa` (`droidian`). Vendor Android 13 `OS2.0.13.0.UMZCNXM`, gbinder ApiLevel 33. |
| CPU | Snapdragon 870 (`SM8250`, `kona`). A55 `cpu0–3` 614 400–1 804 800 кГц, A77 `cpu4–6` 710 400–2 419 200 кГц, A77 `cpu7` 844 800–3 187 200 кГц. |
| GPU и память | Adreno 650 (`gralloc.kona.so`, `vulkan.adreno.so`), 6 ГБ RAM (5,4 ГиБ доступны Linux), 3,0 ГиБ zram. DRM `/dev/dri/card0`, `/dev/dri/renderD128`. |
| Панель | 11″ IPS LCD, физически 1800 × 2880, около 309 ppi (250 × 160 мм). Основная ориентация 2880 × 1800. DT `qcom,mdss_dsi_m82_36_02_0a_dual_dphy_video`. Яркость около 0,323–420 нит. |
| Частоты | Android DisplayManager показывает 120, 144, 90, 60, 50, 48 и 30 Гц под ID 1–7. HWC2 использует свои ID, на планшете активный ID 0 дал 120 Гц. |
| Яркость и питание | `/sys/class/backlight/panel0-backlight` (`max_brightness=4095`), также `4-0011`. В `/sys/class/power_supply` есть `battery`, `bms`, `usb`, `main`, `dc`, `bq2597x-standalone`, `wireless`, `pc_port`, `fuda`. |
| Температуры | Имена `cpu-0-*`, `cpu-1-*`, `gpuss-0-usr`, `video-usr`, `ddr-usr`, `cwlan-usr`, `aoss0/1-usr`. Номера `thermal_zone*` надо читать при загрузке. |
| Звук и видео | 4 динамика Aqstic WCD938x, `audio.primary.kona.so`, UCM2 `QUALCOMM-SM8250`, PulseAudio droid HAL. Узлы видео `/dev/video32`, `/dev/video33`, `msm_vidc` / `venus_enc`. |

В проверенной схеме `scripts/halium` из `boot_b` монтирует Android F2FS (`/dev/block/sda34`) на запись и переносит
монтирование в `/mnt/android-host-data`. Файл `/mnt/android-host-data/popka-linux/userdata.img` (55 ГиБ, ext4)
монтируется через loop в `/userdata`, где доступны 51,5 ГиБ, из них 42,8 ГиБ свободны. Внутри лежит
`/userdata/ubuntu.img` на 12 ГиБ (свободно 6,8 ГиБ в `/`), включён `.writable_image`. Образ v0.2.0 уже содержит
`android-rootfs.img`, `vendor.img` и `odm.img` в `/var/lib/lxc/android/`.

Порт задаёт `GRID_UNIT_PX=20`, `QTWEBKIT_DPR=2.0`, `NATIVE_ORIENTATION=landscape`, `FORM_FACTOR=tablet`,
`REPOWERD_BACKLIGHT_BACKEND=sysfs` и даёт `phablet` запись в `panel0-backlight/brightness`. Эти параметры рассчитаны
на штатный сеанс, плотность eglfs ПопкаФона задаёт свой профиль. [Порт Ubuntu Touch для pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa)

## Аудит исходной версии

Строки в таблицах относятся к коммиту `25c1d6bf23ef1ecd04ac1b4d55400dd0eb4c0e22`. Для просмотра используйте
`git show 25c1d6b:путь/к/файлу`.

| Участок | Привязка и её последствия |
|---|---|
| HWC2 | `shell/display/naomi_hwc.cpp:37–40,122–163,188–218` — основной дисплей 0, HWC2 через libhybris, пара конфигураций 0=60 и 1=90, старт на 90. `setActiveConfigWithConstraints` загружается из Android-библиотеки. На pipa измеренный HWC2 ID 0 даёт 120 Гц. |
| Размер и ориентация | `shell/display/naomi_hwc.cpp:164–185,188–196,292–324,454–554` — размер берётся из активного режима, ориентация кадра совпадает с ориентацией панели, запасной DPI 432, плотность 2,75. Уменьшенный кадр идёт слоем `DEVICE` с растяжением, полный кадр — `client target`. `shell/app/display_stats.cpp:22–49` и `shell/app/qml/SettingsControls.qml:69–98` ограничивают переключатели 720/1080 и 60/90. |
| Кадры и выключение | `shell/display/naomi_hwc.cpp:224–290,327–364,434–450,517–554` — подтверждение частоты по present fence, толчок через второй режим, выделение чёрного буфера для отказа Qualcomm, отключённый экран пропускает `present`. `shell/display/present_timer.cpp:21–36,88–126` требует `SYNC_IOC_FILE_INFO` и считает пропуски в периодах панели. Срабатывание OFF/ON и освобождение буферов надо повторить на Halium 13. |
| OLED и блокировка | `shell/app/power.cpp:22–32,60–69,143–187,306–375,571–593` — шкала с запасным максимумом 1023, яркость AOD, DOZE, FPC IRQ и команды Samsung `sec/tsp`. `shell/app/qml/AodScreen.qml:16–41,60–78` и `shell/app/qml/ShellView.qml:44–54,293–342,585–597` рисуют притушенный экран и двигают слои против выгорания. На IPS чёрный фон оставляет подсветку включённой, гашение должно использовать OFF. |
| Ввод | `device/install-shell.sh:121–128` — поправка последней строки `0.9995` только для `sec_touchscreen` и `naomi-virtual-touch`. `shell/stream/stream_input.cpp:90–104,216–275` — координаты зрителя переносятся в пиксели текущего окна или в логические координаты отдельного стола. `shell/app/qml/ShellView.qml:129–139,428–453,459–476` — физическое округление от DPR 2,75, экранная клавиатура и жест «домой» портретного лаунчера. Нормированные координаты pipa надо повернуть до пересчёта Qt в dp. |
| Лаунчер и проект | `shell/app/main.cpp:83–93,186–203,229–241` и `shell/app/qml/Main.qml:19–35` — единственное физическое окно занимает `ShellView`, стол 1280 × 720 при DPR 1,5 живёт в отдельном выводе. `device/default-project.json:3` задаёт `pixel5`, `shell/ui/Naomi/Screens/ScreenView.qml:102–110` учитывает верхний левый вырез. На pipa системный стол становится главной сценой. |
| Удалённый стол | `shell/app/desktop_screen.cpp:324–394,502–584,672–687,738–806` — FBO, отдельные такты, начальные 90 к/с, виртуальный вывод на `y=10000`. `shell/app/web_link.cpp:92–116` привязывает паузу и заморозку к зрителю. `shell/app/app_launcher.cpp:29–39,42–83,246–282` передаёт `QT_SCALE_FACTOR` и группирует дочерние процессы для выключения стола. Для физического стола жизненный цикл приложений задаёт сеанс на планшете. |
| Панель, окна, клавиатура | `shell/app/qml/desktop/DeskStyle.qml:27–63`, `shell/app/qml/desktop/WinButton.qml:13`, `shell/app/qml/desktop/DeskWindow.qml:33,93–128` — панель 40 dp, кнопки 30 dp, рамки для мыши. `shell/ui/Naomi/Ui/Keyboard.qml:33–69,73–117,580–605` — все вертикальные размеры пропорциональны ширине iPhone-макета и есть поправка точки касания Pixel. `shell/ui/Naomi/Ui/KeyboardPanel.qml:16–36,132–141` и `shell/input/keyboard_context.cpp:55–102,202–215` дают общий ввод RU/EN. Нужен отдельный размер панели и тот же протокол text-input. |
| Клиенты Wayland | `shell/app/android_apps.cpp:129–135,262–315` — Android определяется по `lxc.payload.waydroid`, видимость выводов фильтруется. `shell/app/qml/desktop/Desktop.qml:1160–1165` считает размер Android в физических пикселях и добавляет скрытую полосу. `shell/app/desktop_screen.cpp:379–390` держит для Wine вывод 1280 × 720. На pipa проверить масштаб Android и отдельный режим Wine при DPR 2. |
| Программы | `shell/apps/web/main.cpp:27–36,117–163` и `shell/apps/web/CMakeLists.txt:1–23` — Qt WebEngine системной версии с отдельным процессом и точным ABI приватных заголовков. `shell/apps/game/main.cpp:580–601,956–987` — фильтр, исключающий телефонный `uinput-fpc`, и выбор больших ядер по частоте. `shell/app/game_library.cpp:55–97` применяет тот же признак геймпада. |
| Файлы, терминал, просмотр | `shell/app/qml/desktop/Desktop.qml:200–251` — прогрев второго окна «Файлов». Модель `shell/files/files_model.cpp:98,139–156` следит за текущим каталогом. `shell/term/main.cpp:44–50,241–290,475–517` хранит ограниченный буфер PTY и меняет его размер по запросу. `shell/preview/preview_video.cpp:152–192,220–288` подгружает GStreamer и выбирает декодеры `amc`, `shell/preview/video_plane.cpp:38–76` рисует NV12. На pipa нужны проверки Mesa/libhybris и конкуренции за кодек, интерфейс этих модулей общий. |

| Участок системы | Привязка и её последствия |
|---|---|
| Батарея | `shell/naomid/battery.cpp:12–52,85–104` — UPower `DisplayDevice`, sysfs `battery` и `usb`, температуры `cpu-1-max-step` и `skin-therm`, ток в мкА, накопленный заряд и время по полям драйвера. `shell/naomid/charge.cpp:16–29,57–70,160–216,365–376` — узел `soc:google,charger`, защитные пороги, OCV Pixel и автоматическое выключение. На pipa этот алгоритм заряда отключён. |
| CPU и нагрев | `shell/naomid/perf.cpp:18–49,66–81,105–178` — SM7250 `policy0/6/7`, постоянные нижние частоты, `/dev/stune`, PM QoS и поправка `skin-therm-cpu/offset`. `shell/app/system_monitor.cpp:76–84,127–175` выбирает верхние кластеры через `policy6/7`, `shell/app/system_stats.cpp:68–91` уже перечисляет `policy*` по sysfs. У pipa представители кластеров 0/4/7, нижний порог температуры корпуса Google автоматически не переносится. |
| GPU и служебные интерфейсы | `shell/naomid/gpu.cpp:10–13,29–45,69–82` — таблица KGSL 625/500/400/275 МГц и запись `min_pwrlevel`. `shell/naomid/audio.cpp:18–24,307–338` — Pulse-сокет UID 32011 и `sink.primary_output`. `shell/naomid/wifi.cpp:81,416`, `shell/naomid/cable.cpp:17–33`, `shell/naomid/cell.cpp:12–31`, `shell/naomid/sensors.cpp:30–64` — интерфейсы `wlan0`/`usb0`, `/ril_0`, sensorfwd. Проверять по загруженному UT, запись частот Adreno 620 на pipa отключена. |
| Подготовка системы | `device/shell-prepare:28–59` ждёт конкретный vendor composer, меняет права на FPC, сенсор и симлинк кодера, останавливает `pixelstats_vendor`, ставит `noop`, меняет пороги VM и монтирует schedtune. `device/install-shell.sh:16–33,45–61,108–147,155–181,218–244` меняет журнал, `logind`, wlan1, udev, PulseAudio, Bluetooth, маскирует `repowerd` и управляет порогами Google при удалении. На pipa установщик регистрирует службы для ручного старта, использует права яркости порта и сохраняет его собственные настройки питания, памяти, Wi‑Fi и звука. |
| Сборка и дополнительные среды | `device/devroot.sh:14–25,30–63` копирует корень UT в `/userdata/naomi/devroot`, монтирует `/android` и `/home`, ставит Qt 6.10 и заголовки Android 30 для libhybris. Сверить версии Qt и провайдеров pkg-config с Halium 13. `device/lomiri.sh:11–28` маскирует `lightdm`. `device/android-setup.sh:13–41,78–108`, `device/wine.sh:22–36,74–108` и `device/cores.sh:11–17,38–49` привязаны к layout devroot, Waydroid и его binder, драйверам Turnip/Box64 и локальным путям. |
| Диагностика на устройстве | `device/hwcspike.py:106–114,170–197,214–242` и `device/safescreen.py:19–25` используют Pixel-путь HWC и свои конфигурации. `device/touchinject.py:15–46,119`, `device/taprec.py:24–25`, `device/touchrec.py:61` — 1080 × 2340, 180 Гц, имя сенсора. `device/battlog.sh:11–23,35`, `device/desksampler.py:20–22,117–121`, `device/gamesoak.py:26,178–180`, `device/blackbox.py:23–24`, `device/stress.py:17` — поля питания, имя зоны корпуса, `policy6`. Для первичной инвентаризации подходят динамические разделы `device/probe.py:77–174,274–284`. |
| Прошивка и USB | `tools/images:21–30` — заводские и портовые образы redfin. `tools/flash.sh:15–22,58–79,102–128,140–168` — прошивает redfin, принудительно выбирает `a`, пишет разделы без суффикса и очищает userdata. `device/naomi-usb:41–55,64–81` заменяет gadget UT на Google USB VID/PID с CDC-NCM и ADB. Для pipa предусмотрен отдельный путь через уже загруженный UT. |
| Инструменты ПК | `tools/naomi:87–90,117–149,178–255,442–456,499–520` — ADB-фильтр redfin, фиксированный туннель `40022`, USB `10.77.0.1`, Pi/mDNS и tar с macOS-специфичным окружением. `tools/naomi:700–709,877–894` и `tools/kbtaps:23,38–60` — запуск со своим `shell.env` и замеры под сенсор Pixel. `tools/deskbench:1268–1292` ждёт KGSL и `skin-therm`. `tools/mirror:6–9` собирает зрителя на Mac, `tools/pi-card:92–99,140–199` использует `diskutil`, а `tools/tablet:32–41` и `tools/tv:36–40` относятся к зрителям Pi, включая отдельный планшет. |

## Кадр, частота и касания

Профиль выбирается в `shell/device_profile.cpp`. Приоритет у `NAOMI_DEVICE=pipa|redfin`, затем у текстового файла
`/etc/deviceinfo`, модели DT, свойств `ro.product.*` и поля `device:` из `/etc/system-image/channel.ini`. В v0.2.0
`/etc/deviceinfo` — каталог конфигураций, поэтому проверка переходит к следующим источникам. `naomid --device`
возвращает идентификатор без запуска D-Bus, а установщик кладёт его в системный `device.env`.
Неизвестный профиль получает базовую геометрию из HWC с отключёнными настройками Pixel.

На планшете QTI SDM отказался от повёрнутого слоя `DEVICE`. При старом `NAOMI_ROTATION=0` были артефакты и через
примерно 40 с пропали USB и Wi‑Fi. У штатного Lomiri картинка на той же панели чистая. Причина зависания по этим
логам пока неизвестна.

В `shell/display/naomi_hwc.cpp` HWC получает полноэкранный слой `CLIENT` и `client target` размером 1800 × 2880.
Окно libhybris запрашивает три RGBA8888 буфера с `HW_COMPOSER`, `HW_RENDER` и `SW_READ_RARELY`. Последний флаг
запрашивает у QTI линейную раскладку. `validateDisplay` и при необходимости `acceptChanges` предшествуют
`setClientTarget` и `present`. В вызов `setClientTarget` подаётся `HAL_DATASPACE_UNKNOWN`. Копия acquire fence
уходит в HAL, оригинал остаётся для EGL при отброшенном кадре. Успешный present fence освобождает предыдущий
буфер и отдельно идёт в счётчик, который пропускает таймауты. Нулевая медиана возможна при единственном кадре
за секунду. [Буферы libhybris](https://github.com/libhybris/libhybris/blob/0cedf90/hybris/egl/platforms/hwcomposer/hwcomposer_window.cpp) · [Клиентский буфер libhybris](https://github.com/libhybris/libhybris/blob/0cedf90/compat/hwc2/hwc2_compatibility_layer.cpp)

`Desktop.qml` поворачивает стол внутри окна 900 × 1440 dp. Кнопка на панели меняет режим, а «Настройки → Экран
планшета» предлагают все четыре угла и автоповорот по датчику naomid. Стол занимает 1440 × 900 dp при 90° и 270°
и 900 × 1440 dp при 0° и 180°. Выбор сохраняется в `~/.config/naomi/shell.conf`, начальный угол по умолчанию — 90°.
Qt Quick переводит координаты касания, наведения, мыши и колеса в локальную геометрию. Если в `shell.env` остались
`QT_QPA_LIBINPUT_TOUCH_MATRIX` или `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` с поворотом, убрать этот поворот перед пробой.
`DesktopScreen` объявляет размер Wayland в ориентации стола и переводит обратно координаты удалённого указателя.

Подсветку на pipa регулирует `/sys/class/backlight/4-0011`. При нулевой яркости `shell-prepare` выставляет
половину аппаратного максимума и даёт оболочке права записи.

Первый запуск сохраняет частоту HAL. Драйвер читает `WIDTH`, `HEIGHT` и `VSYNC_PERIOD` у HWC2 ID 0–31 без смены
режима. «Настройки → Экран» предлагают найденные частоты нативного размера. При отсутствии символа `getAttribute`
остаётся текущий режим. `NAOMI_REFRESH=60` или `NAOMI_REFRESH=120` запрашивает найденный режим после первых кадров.
Период панели и интервалы present fence проверяются отдельно. У 120 Гц период 8,33 мс, у 144 Гц 6,94 мс, у 90 Гц
11,11 мс, у 60 Гц 16,67 мс. [Атрибуты и ID HWC2](https://github.com/LineageOS/android_frameworks_native/blob/lineage-18.1/services/surfaceflinger/DisplayHardware/HWC2.cpp#L692-L736)

## Стол на 11 дюймах

- `shell/app/main.cpp` и `shell/app/desktop_screen.cpp` встраивают `Desktop.qml` прямо в основной
  `QQuickWindow`. При DPR 2,0 получаются 1440 × 900 dp. Вывод Wayland имеет 2880 × 1800 пикселей, собственные клиенты
  получают масштаб 2,0 через `QT_SCALE_FACTOR`, Waydroid и Wine сохраняют свои правила расчёта буферов.
- Панель 60 dp, кнопки 44 dp, заголовки 54 dp. Захват края расширен до 18 dp. Окно выводится наверх по нажатию,
  перемещается за полосу и разворачивается кнопкой. Панель возвращает свёрнутые браузер, терминал и другие окна.
  Габарит 44 dp при DPR 2 — около 7 мм. Проверить попадание пальцем, длительные нажатия, колёсико и вторую кнопку мыши.
- RU/EN-клавиатура центрируется снизу до ширины 900 dp, высота около 304 dp. Ряды до 55 dp, переход языка — глобусом.
  Для поля в фокусе есть кнопка клавиатуры на панели. `shell/input/keyboard_context.cpp` закрывает панель при потере
  фокуса основного окна планшета. Для строк браузера, терминала и Wayland text-input отдельно проверить ввод и видимость
  редактируемого поля при открытой клавиатуре. Клавиатуры USB/BT идут через имеющийся evdev-путь RU/EN.
- Три RGBA8888 буфера 1800 × 2880 требуют около 59 МиБ до выравнивания. Нативный стол рисуется прямо в этот кадр.
  `NAOMI_RENDER` на планшете не меняет его размер. Предзагрузка запасного окна «Файлов» и страниц настроек выключена.
- Без доступного `wallpaper.jpg` виден тёмный градиент. При наличии `/usr/local/bin/vpn` в настройках есть VPN,
  список имён из `vpn list` (одно имя на строке), состояние из `vpn status` и подключение через `sudo -n`.
- Пункт «Перезагрузить в One UI» в меню N требует подтверждения. Служба `naomid` вызывает `bootctl` через контейнер
  Halium `android`, проверяет текущий слот `_b` и готовность `_a`, выбирает `_a` и сверяет выбор перед перезагрузкой.
  Помощник устанавливается вместе с `naomid` через `device/install-shell.sh install`. Причина отказа видна в диалоге
  и в журнале `naomid`. [Android Boot Control](https://source.android.com/docs/core/architecture/bootloader/updating).
- Возврат из One UI в Ubuntu Touch доступен через отдельное Android-приложение «В Ubuntu Touch». Установите
  `PopkaPhone-Boot-Switch.apk` из `tablet/build/boot-switch`, затем откройте значок приложения.
  Плитку «В Ubuntu Touch» также можно добавить через редактирование быстрых настроек One UI. Значок и плитка
  открывают диалог подтверждения. [Плитки быстрых настроек Android](https://developer.android.com/develop/ui/views/quicksettings-tiles).
- Для смены слота выдайте root-доступ (`su`) приложению «В Ubuntu Touch» в установленном менеджере root.
  На проверенной сборке One UI 8 со встроенным драйвером KernelSU Next 12851 подходит менеджер KernelSU Next
  1.1.1. Приложение проверяет модель pipa, текущий слот `_a`, готовность `_b` и состояние обновления Android.
  В One UI 8 без `bootctl` встроенный клиент вызывает Boot Control 1.1 и проверяет результат выбора слота.
  С Boot Control 1.2 сценарий также сверяет слот следующей загрузки. При отказе приложение показывает причину.
  Обе стороны используют сценарий `device/naomi-pipa-slot-switch`.
- `naomid` выбирает управляемый интерфейс Wi‑Fi в NetworkManager. На pipa это `wlp1s0`, `wlan1` и интерфейсы P2P пропускаются.
- Для замера на 5,4 ГиБ ориентир — от 0,8 ГиБ `MemAvailable` под постоянной нагрузкой, проверка PSS, zram,
  `vmstat` и OOM в журнале. Первый прогон — браузер с 3–5 вкладками, отдельно один клиент Waydroid, отдельно одна
  программа Wine/Box64. Затем повторить браузер с каждым из них. Отдельно сравнить 60/120 Гц по пропускам и нагреву.
  Среда сборки начинает с двух задач Ninja. Служба оставляет DVFS и пороги ядра pipa в ведении портовых драйверов.

## Venus на SM8250

`shell/stream/venus_encoder.cpp:16–30,68–131,136–211,225–283` в исходной версии открывает `/dev/video/venus_enc`,
задаёт `CAPTURE_MPLANE` H.264/HEVC, затем `OUTPUT_MPLANE` NV12. Используются `VIDIOC_S_FMT`, `VIDIOC_S_CTRL`,
`VIDIOC_REQBUFS`, `QBUF/DQBUF`, `STREAMON/OFF`, `DMA_BUF_IOCTL_SYNC`. В `USERPTR` передаются fd и смещение через
`planes[].reserved[0/1]`, а память выделяется через Android gralloc и libhybris в
`shell/stream/gralloc_buffer.cpp:10–94`. Параметры Qualcomm `0x00992035` (operating rate Q16), `0x00992038`
(low latency), `0x00992083` (bitrate savings) и потолок QP нужно сверять по `VIDIOC_QUERYCTRL`. Исходный код задаёт
для H.264 Constrained Baseline, уровень 5.1 и VBR. Трансляция экрана телефона начинает с ширины 720 и ждёт 90 к/с
по умолчанию (`shell/stream/stream_service.cpp:617–636,2864–2865`). [Реализация QBUF в ядре pipa](https://github.com/aymanrgab/linux-android-xiaomi-pipa/blob/droidian/techpack/video/msm/vidc/msm_vidc.c#L344-L389)

Исходные ограничения `shell/stream/venus_encoder.h:18–34` и `shell/stream/stream_service.cpp:2181–2192` привязаны
к SM7250: 1 958 400 макроблоков/с до запаса 2%, общий режим 1 530 000. В исходнике ядра `droidian`
для `kona` указан `qcom,max-hw-load=7 833 600`, предел HQ 8 160 макроблоков на кадр и 489 600/с.
Это ограничения драйвера по исходнику. Пропускную способность конкретной прошивки и совместное использование с
декодером нужно измерить. Для 2880 × 1800 при расчёте по 16 × 16 получается 20 340 на кадр, 1 220 400 при 60 к/с
и 2 440 800 при 120 к/с. Для 1280 × 720 при 30 к/с — 108 000. [Таблица Kona в драйвере pipa](https://github.com/aymanrgab/linux-android-xiaomi-pipa/blob/droidian/techpack/video/msm/vidc/msm_vidc_platform.c#L1190-L1238)

На планшете `/dev/video32` — декодер, `/dev/video33` — кодер. `QUERYCAP` и `ENUM_FMT` подтверждают сырой NV12
и выбранный H.264 или HEVC. `S_FMT` и `G_FMT` дают stride и `sizeimage` для выравнивания 512/512/256 (stride/Y/UV).
У двухплоскостного `OUTPUT_MPLANE` каждому кадру выделяется отдельный DMA-буфер extradata по размеру драйвера.
Код запрашивает минимум Kona, затем выделяет точное число буферов, возвращённое `REQBUFS`. Неподдерживаемые
настройки Qualcomm пропускаются после `QUERYCTRL` с сообщением в журнале. [Плоскости и очереди VENC](https://github.com/aymanrgab/linux-android-xiaomi-pipa/blob/droidian/techpack/video/msm/vidc/msm_venc.c#L1127-L1188) · [Передача DMA-буфера и смещения](https://github.com/aymanrgab/linux-android-xiaomi-pipa/blob/droidian/techpack/video/msm/vidc/msm_vidc.c#L403-L409)

После пробы кодера `NAOMI_PIPA_STREAM=1` включает вывод встроенного стола через существующий локальный порт
8478 и SSH-туннель Mirror. По умолчанию поток в альбомном режиме — 1280 × 800, до 60 к/с. Шейдер переводит
портретный буфер Qt в ориентацию стола при упаковке NV12. `naomi-venc-bench` и Mirror можно проверять по отдельности.

## Риски первого запуска

| Риск | Проверка и действие |
|---|---|
| Артефакты или зависание QTI SDM | Остановить владельца дисплея, проверить нативный `client target`, сохранить второй SSH и постоянный журнал. Сравнить чистую загрузку и передачу HAL после Lomiri. |
| Перевёрнутое изображение или двойной поворот сенсора | Четыре угла, диагональ, два пальца, наведение и колесо USB/BT-мыши. Сверить ориентацию DT и выбрать `90` или `270`. |
| Режим выбран в HAL, кадров приходит меньше | Сверить режим, период vsync и present fence при непрерывной прокрутке. Начать с 60, затем 90/120/144. Проверить 48/50/30 и ещё раз 60 после пробуждения. |
| Подсветка не возвращается, пропадают USB и Wi‑Fi | Для первой пробы задать `NAOMI_NO_SLEEP=1`, сверить `systemd-inhibit --list` и наличие `naomi-pipa` в `/sys/power/wake_lock`, если узел доступен. Потом проверить таймер, яркость и будящие события по очереди. |
| HAL/Qt/Wayland ABI расходятся с devroot | Сверить версии библиотек, владельца `/dev/input`, `/dev/dri`, группы unit и `wayland-naomi`. Сначала оконные «Файлы» и PTY, потом браузер, Android и Wine. |
| 6 ГБ, длительный нагрев, zram/ION | Измерить `MemAvailable`, PSI, swap-in/out, частоты и температуры при трёх отдельных профилях нагрузки. При свопинге и задержках взять 60 Гц и уменьшить число тяжёлых процессов. |
| Видно 0 °C или неверный знак тока | Сверить батарею с UPower и сырыми полями `battery`/`bms`/`usb`, подобрать датчик корпуса при наличии. Неиспользуемые поля Pixel показывают ноль при отсутствии файла. |

## Первый запуск на слоте `_b`

1. После загрузки UT проверить слот и ОС с ПК. На Windows использовать `py -3`, на Linux/macOS — `python3`.
   `--adb` принимает конкретный серийный номер для проверки, `--ssh` — адрес или настроенный алиас OpenSSH с ключом
   для `phablet`. `stage`/`build` повторяют проверку в той SSH-команде, которая пишет файлы. `redfin`, `_a`, неизвестная
   ОС и противоречивые значения слота отклоняются до записи. Архив содержит `shell/` и `device/` из `HEAD` с LF,
   в интерфейсе три команды `check`, `stage`, `build`.

   ```powershell
   py -3 tools/pipa.py check --ssh pipa-dev
   py -3 tools/pipa.py stage --ssh pipa-dev
   ```

2. В штатном сеансе UT записать исходные показания и убедиться, что UID phablet — 32011. Проверить группы
   `android_input`, `android_graphics`, `video`, `audio`, `input`, доступность `/android` и монтирование `/userdata`.
   Проверить работу экрана, касаний, заряда и звука в Lomiri до смены композитора.

   ```sh
   cat /usr/lib/os-release /proc/device-tree/model
   cat /etc/system-image/channel.ini
   cat /proc/cmdline
   getprop ro.boot.slot_suffix
   uname -m
   findmnt /
   findmnt /userdata
   findmnt /mnt/android-host-data
   df -h / /userdata /home/phablet
   id phablet
   getent group android_input android_graphics video audio input
   ls -l /sys/class/backlight /dev/dri /dev/video32 /dev/video33
   cat /sys/class/backlight/panel0-backlight/max_brightness
   for f in /sys/devices/system/cpu/cpufreq/policy*; do
       printf '\n%s\n' "$f"
       cat "$f/related_cpus" "$f/cpuinfo_min_freq" "$f/cpuinfo_max_freq"
   done
   ```

3. Подготовить devroot на UT, подставив полный путь `Исходники`, выведенный `stage`. Сверить `libhybris-dev`,
   `libhwc2`, `hwcomposer-egl`, `libhardware`, `qt6-base-private-dev`, Qt Wayland и WebEngine с установленным рантаймом.
   `build` использует `sudo -n` только для запуска компиляции в готовом devroot от `phablet`.

   `create` копирует настройки и ключи APT из образа, включая `ubuntu.sources` (deb822) и `ubports.list`
   в `/etc/apt/sources.list.d/` v0.2.0. `packages` использует зеркала образа. Для arm64 в `universe` Resolute
   на `archive.ubuntu.com` есть Qt 6.10.2, включая `qt6-base-private-dev`, `qt6-declarative-private-dev` и
   `qt6-wayland-private-dev`. Пакеты `libhybris-dev` и `android-headers-30` есть в архиве UBports `26.04-1.x`.
   [Пакеты Qt в Ubuntu](https://packages.ubuntu.com/source/resolute/arm64/misc/qt6-wayland) · [Архив UBports](https://repo.ubports.com/dists/26.04-1.x/)

   На планшете установлен `libqt6core6t64` версии `6.10.2+dfsg-14+0~20260605145647.2+ubports26.04.1~1.gbpc691c0`.
   Версия `libhybris` начинается с `0.1.0+git20250630+0cedf90-0ubports1` и содержит `+ubports26.04.1`.

   ```sh
   SRC=/home/phablet/naomi/src/pipa-ПОЛНЫЙ_SHA
   sudo sh "$SRC/device/devroot.sh" create
   sudo sh "$SRC/device/devroot.sh" packages
   sudo sh "$SRC/device/devroot.sh" user 'pkg-config --modversion libhwc2 hwcomposer-egl hybris-egl-platform libhardware'
   ```

   ```powershell
   py -3 tools/pipa.py build --ssh pipa-dev --jobs 2
   ```

4. Проверить собранные файлы и профиль. Если `~/naomi/build` свободен, привязать его к пути `Сборка` из вывода.
   Установить службы. Перед первым стартом открыть второе SSH-соединение, определить владельца HWC и остановить его.
   В примере владелец — `lightdm.service`. Для первой пробы вписать `NAOMI_NO_SLEEP=1` в `~/naomi/shell.env`.

   ```sh
   BUILD=/home/phablet/naomi/build-pipa-ПОЛНЫЙ_SHA
   "$BUILD/naomid/naomid" --device
   "$BUILD/naomid/naomid" --backlight
   ln -sT "$BUILD" /home/phablet/naomi/build
   sudo sh "$SRC/device/install-shell.sh" install
   sudo systemctl start naomid.service
   sudo systemctl stop lightdm.service
   sudo systemctl start naomi-shell.service
   journalctl -u naomi-shell -u naomid -u naomi-pipa-awake -b -n 180 --no-pager
   ```

   Служба выводит `repowerd` из сеанса через конфликт systemd. `naomi-pipa-awake` удерживает блокировки logind
   и доступный kernel wake lock до остановки
   оболочки. Это заменяет управление сном repowerd на время сеанса, включая работу от батареи. Портовая настройка
   `NoSuspendOnCharger` снова действует при запуске repowerd. [Блокировки logind](https://www.freedesktop.org/software/systemd/man/latest/systemd-inhibit.html)

   Если артефакты повторяются после смены композитора, остановить `naomi-shell` и Lomiri. Сравнить пробу после
   чистой загрузки и после `sudo /usr/local/lib/naomi/shell-prepare --restart-pipa-composer`. Команда проверяет имя
   QTI-сервиса, ждёт перехода в `stopped`, затем в `running`. Она запускается вручную между клиентами HWC. Отдельная
   проба `test_hwcomposer` или его HWC2-варианта из пакета `libhybris-test` также требует свободного HAL.
   `LD_PRELOAD=libtls-padding.so` у Lomiri помогает при ошибке загрузки статического TLS. Проверить наличие библиотеки
   и добавить её в `shell.env` для отдельного прогона при сообщении `cannot allocate memory in static TLS block`.
   При зависании сохранить предыдущую загрузку из постоянного журнала через `journalctl -b -1` и проверить `/sys/fs/pstore`.

5. Сверить `панель 1800x2880, кадр 1800x2880`, DPR 2, чистую картинку стола и оба канала связи в течение нескольких
   минут. Проверить найденные 60, 90, 120 и 144 Гц. Проверить четыре угла, клавиатуру RU/EN в браузере и PTY,
   наведение, перетаскивание и колесо. Затем убрать `NAOMI_NO_SLEEP`, проверить гашение по таймеру и пять циклов
   выключения подсветки и пробуждения на 60 и 120 Гц. При гашении подсветка идёт в 0, DSI остаётся в ON. Кнопка,
   касание, движение мыши и клавиши возвращают яркость. `NAOMI_NO_SLEEP=1` отключает таймер, ручное гашение доступно.
   Для восстановления штатного сеанса выполнить следующие команды.

   ```sh
   sudo systemctl stop naomi-shell.service naomid.service
   sudo systemctl start repowerd.service lightdm.service
   ```

6. После стабильного экрана проверить батарею, нагрев, Wi‑Fi, звук четырёх динамиков, BT-устройства и аппаратное видео.
   Сверить `nmcli -t -f DEVICE,TYPE,STATE dev` с состоянием в панели. Проверить обновление списка VPN, подключение,
   отключение и отмену перезагрузки. Для кодера выполнить `v4l2-ctl -d /dev/video33 -D --list-formats-out --list-formats
   --list-ctrls`, затем первый прогон `naomi-venc-bench --size 1280x720 --fps 30 --rate 30 --mbps 8 --maxqp 0 --frames 120`
   с EGL порта. Проверить HEVC, затем Mirror с `NAOMI_PIPA_STREAM=1`. Измерить нагрузку Waydroid/Box64, `MemAvailable`, `/proc/swaps` и
   `/sys/block/zram*/mm_stat`. До включения автозапуска повторить проверку на `redfin`: портретный лаунчер,
   60/90 Гц, OLED AOD, свайп с последнего ряда, удалённый стол и запуск старых клиентских приложений.

## Локальные проверки

Собрать APK для переключения из One UI можно на Windows с JDK и Android SDK. Установите платформу
`platforms;android-36` и `build-tools;36.0.0`, затем выполните из корня репозитория.

~~~powershell
py -3 .\tablet\boot-switch\build.py --sdk "C:\путь\к\Android\Sdk"
~~~

На macOS и Linux используйте `python3 tablet/boot-switch/build.py` с настроенным `ANDROID_HOME`.
Ключ подписи создаётся в `private/pipa-boot-switch` при первой сборке. Сохраняйте его для установки обновлений APK.

- `py -3 -B -m unittest discover -s tools/tests` проверяет точный `os-release` образа v0.2.0, оба признака Touch
  для 26.04, поддержку 24.04 с Lomiri и отказ для неподходящей системы, модели, архитектуры и слота. Для каталога
  `/etc/deviceinfo` проверяются модель DT и поле `device:`. Все ответы берутся из тестовых данных.
- Пройдены разбор 39 файлов Python, 3 JSON, 19 скриптов `sh`/Bash и Ruff для `tools/pipa.py` и `tools/tests/test_pipa.py`.
  Модели устройства, системы и слота проверены в 12 сценариях с подменой ответов. Вызовы ADB и SSH в тесте
  заменены заглушками. Архив из Windows содержит исходники и сценарии с LF при включённом `core.autocrlf`.
- Qt 6.10.2 разобрал 115 QML-файлов, в шаблоне `device/qml/camera.qml` предварительно подставлены значения.
  На offscreen проверены клавиатуры, четыре угла для касания, наведения, клика и колеса при 0°, 90° и 270°,
  действия VPN и подтверждение перезагрузки. Помощники в тесте заменены заглушками.
- На Linux проверен синтаксис 17 изменённых файлов C++ с заголовками Qt 6.10.2. Символы HWC2 сверены с публичной
  arm64-сборкой Halium 13. Заглушки проверили таймаут present fence, выбор управляемого Wi‑Fi, перезапуск composer
  и очереди Venus, включая две плоскости на Kona и возврат 6/7 буферов. Пройдены восемь тестов Mirror.
- Для второго этапа предстоит полная сборка в devroot arm64 и проверка HWC2, Wayland, реальных устройств ввода,
  работы USB/Wi‑Fi при погашенной подсветке и кодера на планшете по порядку выше.
