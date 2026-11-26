# Changelog

## v0.3.0-pipa — 2026-11-26

Deep power, thermal, display, and standby optimization release — verified live on Xiaomi Pad 6 (`pipa`, Snapdragon 870 / `SM8250`) and architected with portable device capabilities & topology auto-discovery for other Halium 13 / ARM64 Linux tablets.

### Kernel, SoC, Storage, GPU & Radio Efficiency (`device/shell-prepare`, `device/install-shell.sh`, `device/zz-pipa-security.conf`)
- **Fixed 100% MAX CPU pin on Halium 4.19 (`ondemand` vs `schedutil`):** on 4.19 QTI WALT kernels without `CONFIG_UCLAMP_TASK`, `schedutil` stayed permanently pegged at `1804.8 / 2419.2 / 3187.2 MHz` due to RT threaded IRQs. Auto-discovered `policy*` clusters now run tuned `ondemand` (`sampling_rate=20000`, `up_threshold=70`, `io_is_busy=1`, `sampling_down_factor=2`, hardware `cpuinfo_min_freq`) with `schedutil` fallback — verified live dropping idle frequencies to **`300 / 710.4 / 844.8 MHz`** with instant burst ramp-up.
- **UFS 3.1, GPU & Memory Bus Power Management:** enabled `auto_hibern8=5000` + `clkgate_enable=1` on `*.ufshc` (verified live `ufshc` entering `suspended` during idle), set Adreno 650 `kgsl-3d0/force_no_nap=0` (verified idling at **`150 MHz`**), restored stock SM8250 `bw_hwmon` caps (`14236` / `6881`), and tuned `mem_latency` (`polling_interval=20`) + WALT downmigration.
- **Eliminated Background Daemon & Radio Waste:** stopped 10 unused Android LXC daemons (`feature_enabler_client`, `wfd*`, `traced*`, `statsd`, `vendor.vppservice`, `neuralnetworks_hal_service`, `vendor.ipacm`), masked `nfcd.service` & `ofono.service` on Wi-Fi-only tablets, marked unused `wlan1` unmanaged in NetworkManager (`90-naomi-wlan1.conf`), and removed the conflicting `wifi.powersave=2` override in `zz-pipa-security.conf` (enabling `cnss` QCA6390 PCIe runtime suspend with Wake-on-Wireless).

### Display Pipeline, Compositor & Standby Breakthrough (`shell/display/`, `shell/app/`, `shell/apps/web/`)
- **True `HWC2_POWER_MODE_OFF` on Screen-Off (`shell/app/power.cpp`, `shell/device_profile.*`):** re-enabled real HWC2 panel power-off on `pipa` (overrideable via `NAOMI_DISPLAY_POWER_OFF=0`), cleanly turning off `card0-DSI-1` DPMS, shutting down `disp_cc_pll0`, and putting `ae00000.qcom,mdss_mdp` into **`suspended`** (previously 99.99% active while screen-off) with clean wake-resume verified live.
- **30 Hz Idle Refresh + UBWC + Hardware Night Filter CTM (`shell/display/naomi_hwc.cpp`, `shell/app/display_stats.cpp`):** lowered default tablet idle refresh to **`30 Hz`** with a dedicated `2.2 s` single-shot idle timer (`m_idleTimer`), enabled Qualcomm UBWC gralloc compression (`NAOMI_PIPA_UBWC`, `usage 0x10001a00`, visually verified pixel-perfect), and wired hardware `HWC2::Display::setColorTransform` 4×4 CTM for zero-GPU-overhead Night Filter (with QML fallback).
- **Fixed Screen-Off Cgroup Freezer & Background Loops (`shell/app/main.cpp`, `shell/app/qml/desktop/`):** fixed `viewer = (profile.tablet && power.displayOn()) || streamService.connected()` so desktop apps actually freeze when the tablet screen turns off; stopped `DeskBar` clock redraws, `SystemTools` polling, `PreviewVideo`, and `Waydroid` frame loops on screen-off; disabled hidden spare `FilesWindow`, 9-page Settings pre-warm, and permanent hidden `naomi-web` spare Chromium renderer (`NAOMI_WEB_NO_SPARE=1`) on tablets (reducing `naomi-shell` RSS to ~228 MB).

### Portable Tablet Profiles, Skin Thermal Governor & Deep Suspend (`shell/naomid/perf.*`, `SystemPage.qml`)
- **Multi-Subsystem Power Profiles (`Эконом 60 Гц` / `Баланс 120→30 Гц` / `Максимум 144 Гц`):** `naomid` (`org.naomi.Phone.Perf`) auto-discovers CPU clusters and `kgsl-3d0` levels and coordinates CPU frequency ceilings, `ondemand` thresholds, `input_boost`, GPU `max_pwrlevel`, and HWC2 refresh rate together.
- **Skin Thermal Governor (`quiet_therm` / `skin-therm`):** 5-second thermal monitor with 2 °C hysteresis (`39 / 42 / 45 °C` thresholds) smoothly caps Prime/Big/GPU clocks before the tablet body gets uncomfortably warm.
- **Dynamic `wake_lock` & Opt-In Deep Sleep (`mem`):** removed permanent kernel `wake_lock` from `naomi-pipa-awake.service` (keeping only `handle-power-key` inhibit) and handed dynamic wake-lock + opt-in battery `deep` (`mem`) suspend to `naomid`, with **«Режим сервера (не усыплять сеть)»** and **«Глубокий сон (deep)»** toggles in Settings → Система (automatically bypassed when plugged into USB/AC or during active SSH sessions).
- **Reduced `naomid` Wakeups (`shell/naomid/charge.cpp`, `shell/naomid/cell.cpp`):** eliminated the 30-second `iw` process fork storm in `charge.cpp` and short-circuited cellular polling when `!profile.hasCellular`.

### Wi-Fi Interface Auto-Detection Fix (`device/pipa-mode`, `device/95-pipa-mode`, `extras/tg-bot/`)
- Auto-detect active wireless interfaces (`wlan*` / `wlp*`, skipping `p2p-*`) and include `/usr/sbin` in `PATH` so `pipa-mode` firewall rules, NetworkManager dispatcher, and `tg-bot` work seamlessly whether Halium enumerates Wi-Fi as `wlan0` or `wlp1s0`.

---

## v0.2.0-pipa — 2026-11-25

System hardening, travel security mode, DNS-leak-free TUN VPN, and compositor/desktop fixes verified on Xiaomi Pad 6 (`pipa`).

### System Audit & Shell Hardening
- **Dual-boot & shutdown safety (`device/install-shell.sh`, `device/naomi-pipa-slot-switch`):** added `/usr/lib/systemd/system-shutdown/naomi-pipa-sync` to cleanly flush `userdata.img` (`loop0`) and `f2fs` `/mnt/android-host-data` before rebooting into Android (`_a`); hardened slot-switch D-Bus timeouts and `naomid` `wlp1s0` Wi-Fi detection.
- **Compositor, WebSurface & input (`shell/app/`):** fixed `WebSurface` redraw tracking so offscreen/unmapped surfaces stop burning GPU frames; fixed external IME handling in `app_launcher.cpp`; hardened `code_pin.cpp` against `ENOMEM` when pinning memory; expanded `.desktop` icon lookup paths (`~/.local/share/icons`, `Adwaita`, `suru`) and added `TryExec` + executable validation in `system_tools.cpp`.
- **Memory, storage & audio (`device/shell-prepare`, `device/install-shell.sh`, `device/91-pipa-audio-jack.rules`):** enforced 4 GB `zstd` ZRAM, unified persistent journald limit via `zz-90-naomi.conf` (`SystemMaxUse=100M`, overriding `lxc-android-config.conf`), masked failing/conflicting Ubuntu Touch units (`repowerd`, `keep-display-on-during-boot`, `dpkg-db-backup`), and added udev group `audio` permissions for the headset jack (`kona-mtp-snd-card*Jack*`).

### Security / Travel Mode & DNS-Leak-Free VPN
- **Home / Away / Auto Security Mode (`device/pipa-mode`, `device/pipa-firewall.service`, `device/95-pipa-mode`, `device/zz-pipa-security.conf`):**
  - Dedicated `iptables`/`ip6tables` chains (`PIPA_IN` / `PIPA_IN6`) loaded before `network-pre.target` — Home mode restricts SSH (`22`) and DLNA (`18766`) to configured `home_subnets` + USB; Away mode blocks all unsolicited inbound Wi-Fi traffic while preserving USB cable access.
  - NetworkManager dispatcher (`95-pipa-mode`) automatically switches modes on `wlp1s0` SSID changes, toggles Wi-Fi power-save, optionally starts a default VPN profile away from home, and sends optional Telegram notices.
  - Enabled Wi-Fi scan MAC randomization (`zz-pipa-security.conf`) to prevent probe-request tracking outside home.
- **DNS-Leak-Free TUN VPN (`device/pipa-vpn`):** sing-box TUN (`popka-tun0`) with DoH (`127.0.0.1:1053`) pushed to NetworkManager `dnsmasq` over D-Bus (`SetServersEx`), LAN DNS bootstrap restricted strictly to VPN server domains (preventing circular DNS deadlocks), UDP/123 NTP `direct` bypass for `systemd-timesyncd`, and IPv6 disabled on the tunnel interface.
- **Telegram Control Hub `/mode` (`extras/tg-bot/`):** added `/mode [home|away|auto]` command (21 registered commands), main dashboard quick button (`🎒 Режим Дома/Дорога`), and live interactive security panel.

### Quality & Tests
- Added **`tools/tests/test_pipa_mode.py`** covering travel-mode resolution, config fallbacks, dynamic firewall descriptions, `TabletDriver` travel-mode helpers, and `pipa-vpn` DoH/NTP/bootstrap TUN config generation (**99 Python & C++ harness unit tests** + **8 Node Mirror tests**).
- Extended GitHub Actions CI to syntax-check `device/95-pipa-mode`, `device/pipa-mode`, and `device/pipa-vpn`.

---

## v0.1.0-pipa — 2026-11-24

First public Xiaomi Pad 6 (`pipa`) release built on top of upstream [PopaSpinka/PopkaPhone](https://github.com/PopaSpinka/PopkaPhone) (`25c1d6b`, MIT) — **181 files changed (+20,811 / −1,063 lines)**.

### Xiaomi Pad 6 (`pipa`) Hardware & Dual-Boot Port
- **Non-destructive A/B dual-boot:** runs Ubuntu Touch 26.04 LTS (Halium 13) on slot `_b` while keeping stock/custom Android untouched on slot `_a`.
- **Bidirectional slot switching:** added `device/naomi-pipa-slot-switch` (Boot Control HAL 1.1/1.2 verification) wired into the `N` menu and a companion Android app + Quick Settings tile (`tablet/boot-switch/`).
- **Display & HWC2 (`shell/display/`, `shell/device_profile.cpp`):** native 11″ `2880×1800` (`1440×900 dp` at DPR 2.0) on-screen desktop via QTI SDM linear `CLIENT` target, dynamic `60/90/120/144 Hz` enumeration, adaptive `60↔120 Hz` refresh hysteresis, 4-way rotation + long-press auto-rotate lock, and sysfs backlight (`4-0011`).
- **Snapdragon 870 (`SM8250`) tuning (`device/shell-prepare`, `shell/naomid/`):** `schedutil` + input boost, unlocked C-states (`lpm_levels`), L3/LLCC/DDR/NPU devfreq, IRQ affinity, 4 GB `zstd` ZRAM, `read_ahead_kb=128`, PulseAudio 48 kHz (`avoid-resampling=yes`), NetworkManager `wlp1s0`, and hardware 80% battery charge limit (`night_charging`).
- **On-device build & safety CLI (`tools/pipa.py`, `device/devroot.sh`):** validates device model (`pipa`), Ubuntu Touch 26.04 (`arm64`), and slot `_b` before staging sources or compiling natively on-device.

### Desktop Ergonomics, Native Camera & Pro Settings
- **Native dual-camera app (`shell/apps/camera/` → `naomi-camera`):** direct `libhybris` `libcamera.so.1` backend using offscreen null-platform EGL Pbuffer (`HYBRIS_EGLPLATFORM=null`) + `cameraservice-trust-stored`; supports rear 13 MP (`4208×3120`) and front 8 MP (`3264×2448`) sensors, zoom, tap-to-focus, flash/torch, MP4 H.264 recording (`ffmpeg`), and headless `--snap` / `--torch` CLI.
- **Multitasking & tablet UX (`shell/app/qml/desktop/`):** 50/50 split-screen window snapping, Always-on-Top PiP pinning, Clipboard History popover (`06 БУФЕР`, 15 entries) + PC clipboard sync, quick screenshot, `03 LINUX` `.desktop` launcher with search, Waydroid 10-minute idle auto-stop, and Laptop auto-mode (physical keyboard & `card0-DP-1` detection).
- **Pro Settings (`InterfacePage.qml`, `SystemPage.qml`):** 24-hour / 12-hour clock toggle (24h default), seconds & date format, warm Night Filter overlay (`0–60%`), terminal font/extra-keys mode, live SM8250 CPU governor selector, thermal/ZRAM/disk telemetry, service toggles, timezone/NTP, journal cleanup, plus Mouse and VPN pages.
- **Terminal & browser upgrades:** touch extra-keys bar with haptics, pinch/touch gestures, `WAYLAND_DISPLAY=wayland-naomi` inheritance, Open-in-Terminal from Files, GPU rasterization and per-tab dark/light toggle in `naomi-web`.

### Windows 11 Mirror & Optional Companion Tools
- **Windows 11 Mirror (`mirror/`, `tools/mirror_win.py`):** Electron desktop viewer over USB (`adb forward`) and Wi-Fi (`ssh -W`) with WebCodecs H.264/HEVC Annex B decoding, touch/mouse/keyboard/clipboard forwarding, and portable x64 packaging (`mirror/build-win.ps1`).
- **Optional self-hosted companion stack:** 20-command Telegram Control Hub (`extras/tg-bot/`, `/term`, `/cam`, `/clip`, LAN DLNA server) and zero-dependency Windows LAN PC Agent (`extras/pc-agent/pc_agent.py`, RFC1918 guard + `X-Pipa-Token`).

### Quality, Testing & Open-Source Infrastructure
- Added **91 Python & C++ harness unit tests** (`tools/tests/`) and **8 Node Mirror tests** (`mirror/tests/`).
- Added GitHub Actions CI (`.github/workflows/ci.yml`), English & Russian documentation (`README.md`, `README.ru.md`, `docs/PIPA-INSTALL.en.md`), `ROADMAP.md`, `CONTRIBUTING.md`, Issue/PR templates, and verified MIT attribution (`LICENSE`, `THIRD-PARTY.md`).

---

## Upstream History (`PopaSpinka/PopkaPhone` — Pixel 5)

# История НаомиФона

## Дальше (в работе)

- Открытая выкладка (22 ноября 2026, решение пользователя: выложить как есть под именем PopkaPhone и на этом проект
  закрыть). Убраны части, которые работали только у автора: чат «Наоми», блокнот «Заметки», студия NaomiPhone, окно
  PopkaStudio и перенос входов браузера с НаомиОС; окно на Mac теперь со своим Electron. Из документов и кода убрано
  личное: адреса домашней сети, имя сети Wi-Fi, адреса Bluetooth, оператор SIM, часовой пояс, ссылки на личные
  страницы, названия сборок игр. Новое: лицензия MIT и список чужих компонентов (`LICENSE`, `THIRD-PARTY.md`),
  README и установка для людей (`docs/INSTALL.md`), `tools/images` (скачать и сверить образы), `tools/naomi setup`
  (всё после прошивки одной командой; с ним `devroot`, `lomiri`, `wine`), шрифты, словари и фильтры скачиваются
  сами со сверкой SHA-256, ключ SSH создаётся сам. `tools/publish` собирает чистую копию без истории и не даёт
  собрать её, пока сканер находит в файлах личное. Проход установки с нуля из чистой копии на стёртом телефоне
  (22 ноября) нашёл и закрыл пробелы: `flash.sh bootloader` из Ubuntu Touch, проверка интернета и часов перед
  установкой, провал установки пакетов больше не проходит молча, главный экран по умолчанию
  (`device/default-project.json` — без проекта студии на главном была одна строка «Настройки»), `tools/pi phone`
  (знакомство приставки с телефоном делалось руками), первый вход на Pi без готового `known_hosts`.
- Телевизор — четвёртый экран (просьба пользователя: «добавим четвёртый экран — телевизор… если даже выключен —
  включился, и если играл в PS5 — сам поверх запустил приложение… на кнопку пульта»). На телевизор TCL (Android TV)
  поставлено то же приложение «НаомиФон», что у планшета; в «Экранах» у него своя строка. Стол на нём идёт в 60
  кадров/с с задержкой 41–44 мс до вывода (у планшета 76–85), мышь и клавиатура приставки управляют им как обычно.
  Когда приложение не на экране, телевизор в «Экранах» значится «спит»: поставили на «стол» или «главный» — и он
  показывает стол: включённый — через 0,3 с, поверх YouTube или HDMI-входа; спящий — через 2–3 с (приставка будит
  его по сети, служба приложения включает экран). Поставили на «нет» — приложение уходит, на экране снова то, что
  было, а телевизор, который будили мы, засыпает обратно. Кнопка Apple TV на пульте открывает стол на телевизоре и
  убирает его. Пока стол на телевизоре, дом (НаомиОС) не гасит его как забытый. По дороге найдены два запрета
  прошивки TCL — на автозапуск служб и на сеть во сне; оба снимает одна команда (`tools/tv access on`, её запускает
  пользователь). Не проверено: игра на PS5 под столом, ночь сна, резкость глазами. Подробности — `docs/TV.md`.
- «Экраны»: на всех экранах «нет» — рабочий стол выключается сам (просьба пользователя; раньше он оставался
  «включён», хотя показывать его было некому). Выключатель потом возвращает стол на тот экран, с которого его сняли
  последним, а выбор любого экрана включает стол.
- Яркость планшета со стола (просьба пользователя): в окне «Экраны» под строкой планшета — шкала яркости и кнопка
  «АВТО» (планшет ведёт яркость сам, по своему датчику света). Работает и при выключенном столе, выбор помнится.
- Стол «включён», а ни один экран его не показывает (жалоба пользователя: «иногда просто отваливается»). Причина —
  в новом мультиэкране: при смене главного экрана стол на миг принимал форму прежнего, потом нужную, и от двух смен
  размера подряд видеокодер получал чужие буферы и отдавал пустые кадры без конца — поток трансляции на них
  зацикливался и переставал делать что-либо ещё. Теперь смена главного — одна смена размера, буферы кодеру — только
  под нынешний кадр, а сломавшийся кодер запускается заново. Тот же сценарий и 60 быстрых смен главного экрана
  подряд проходят без единого сбоя (`docs/SCREENS.md`).
- Экраны (просьба пользователя: «мультиэкранность: одна картинка, приёмников сколько хочу… выбрать, на какие экраны
  вывести… главный экран — его соотношение у всех… заставка с часами, как без дока на малинке… двойной тап на
  планшете — стол переезжает»). Рабочий стол теперь можно показывать сразу на мониторе приставки, в окне Mac и на
  планшете — или переносить между ними. В «Настройках» телефона и на столе (окно монитора, кнопка «Экраны») у каждого
  экрана три положения: заставка, стол, главный; главный задаёт форму стола — с планшетом главным стол 16:10 и
  занимает его экран целиком. Экран, на который стол не выведен, показывает заставку — обои и часы, одинаковые на
  мониторе, в окне Mac и на планшете. Жест на заставке (щелчок или клавиша на Mac, двойное касание планшета, клавиша
  у приставки) зовёт стол на этот экран. Клавиатура и мышь приставки управляют столом, на каком бы экране он ни был,
  её курсор виден и на Mac, и на планшете. Телефон сжимает картинку по-прежнему один раз: с тремя экранами сразу
  перетаскивание окна на мониторе — те же 125 кадров из 125, путь кадра +1 мс, мощность та же. На планшете —
  киоск: на экране только «НаомиФон», без шторки, «домой» и жестов системы; кнопка питания гасит экран и включает
  сразу в приложение (заодно найдено и исправлено: приложение падало каждый раз, когда гас экран). Подробности —
  `docs/SCREENS.md`.
- Платы ESP32 (просьба пользователя: PopkaStudio на столе телефона, плата — в приставке). Плата, вставленная в Pi,
  теперь видна на телефоне как своя: `/dev/ttyACM0` с её же именем и серийным номером, сброс и загрузчик проходят —
  Pi пробрасывает её по кабелю (USB/IP). В ядре телефона не было ни драйвера порта платы, ни проброса — три модуля
  собраны под его ядро на самом телефоне, каждая версия функции ядра сверена. Плата отвечает с телефона за 5 мс.
  Команда `tools/boards`, подробности — `docs/BOARDS.md`.
- PopkaStudio на столе (просьба пользователя): плитка POPKA в меню N открывает его редактор экранов для плат ESP32
  своим окном — как приложение, без вкладок браузера. Студия стоит на телефоне как есть, её же установщиком, и
  прошивает плату из приставки прямо с телефона: проект пользователя ушёл на круглые часы за 219 с (сборка на
  телефоне ≈ 2,5 мин, заливка 18 с). В окне работает всё, что пробовали: превью циферблатов, погода, печать
  латиницей и по-русски. Для таких окон браузер научился «режиму приложения» (`docs/WEB.md`).
- Load PopkaStudio вдвое быстрее, если окно студии не на виду (жалоба пользователя: «висло, очень долго грузило»).
  Разбор показал: тормозит не плата и не кабель, а нагрев — страница студии перерисовывает часы 60 раз в секунду
  без остановки, телефон в доке грелся до 48 °C и сбрасывал частоту ядер втрое; Load 116 с с окном против 64 с без
  него. Теперь свёрнутое окно студии стоит на паузе и не тратит процессор совсем (было ~1,5 ядра).
- Планшет вторым экраном (просьба пользователя: «приложение на планшет… управление остаётся с малинки… и с планшета —
  клики и хваты»). Приложение «НаомиФон» на Galaxy Tab S5e показывает стол вместе с монитором приставки: картинку ему
  пересылает Pi, телефон ничего не сжимает заново. Мышь, клавиатура и трекпад Pi работают как раньше, их курсор виден
  на планшете; пальцем — щелчок, двойной щелчок, перетаскивание окон, прокрутка и правый щелчок двумя пальцами. Связь
  шифрованная и только с сопряжённым планшетом. В движении — 60 кадров в секунду без потерь, от кадра на телефоне до
  экрана планшета ~80 мс (у монитора ~40). Монитор приставки — на 60 Гц (выбор пользователя: так ровно и на
  планшете). Подробности и что дальше — `docs/TABLET.md`.
- Разбор «сверху вниз» (просьба пользователя: взглянуть на всё со стороны, от ядра до приставки, и выжать максимум).
  Новый инструмент — «рентген» (`tools/xray`): видно каждый поток каждой программы — на каком ядре работал, сколько
  ждал и за кем. Первое же, что он показал: ядра телефона между звеньями кадра успевали уснуть, и каждое звено ждало,
  пока ядро проснётся. Теперь, пока стол смотрят, ядра не засыпают глубоко: кадр доходит до монитора на ~3–4 мс
  быстрее (27 → 23 мс от такта), первый кадр после паузы — на 5–6 мс, цена — около 0,1 Вт. Заодно исправлено: браузер
  больше не получает случайно тот же приоритет, что и сам стол; оболочка возвращает память после фото и видео (за
  десять прогонов разбухала с 228 до 666 МБ); после каждого переподключения приставки оставался висеть захват звука
  (ошибка утренней правки). План следующих этапов и проверок — `docs/AUDIT.md`.
- Этап 1 разбора — браузер. Видео и листание в окне браузера теперь на четверть легче для видеочипа телефона (на
  ролике 1080p60 он был занят на 75 %, стало 56 %): скруглённые углы страницы больше не требуют перерисовывать каждый
  кадр браузера дважды. Вид тот же — углы сглажены, и теперь не становятся прямыми, пока окно тянут за край. Три
  другие догадки (про ядра и приоритеты) замер не подтвердил — их не вносили. Для таких замеров прогон стола научился
  сам открывать нужную страницу и ролик и считать кадры браузера.
- Этапы 2 и 3 разбора. Приставка показывает кадр и курсор на ~3 мс свежее: она отдаёт их экрану ближе к моменту
  обновления монитора (за 2 мс вместо 4), а сеть по кабелю больше не придерживает мелкие пакеты (связь Pi → телефон
  1,7 → 0,5 мс). Итог дня по всему прогону стола: кадр в движении 27 → 21,6 мс, от кадра до экрана 43 → 34 мс,
  потерянных кадров 45 → 18. Проверено, что экран телефона и стол работают вместе без помех друг другу (телефон под
  пальцем держит 90 кадров в секунду, стол этого не замечает) — переделывать устройство оболочки не нужно.
  Исправлено: после каждого ролика в «Просмотре» в памяти оболочки оставались его остатки (~7 МБ и открытые файлы).
- Этапы 4 и 5 разбора. Телефон с неподвижным столом на приставке грелся зря: радио Wi-Fi не давали спать, хотя
  картинка шла по кабелю. Теперь радио спит, пока стол стоит: в покое телефон берёт 0,58 Вт вместо 0,90. Переход
  «кабель ↔ Wi-Fi без шва» при этом цел — проверено новым шагом прогона, который сам «вынимает» и «вставляет» кабель
  на ходу и в покое. По одному Wi-Fi стол работает почти как по кабелю (до экрана 33–39 мс против 30–34). Измерена
  цена постоянной копии видео по Wi-Fi ради перехода без шва: около 0,2 Вт в движении — оставлено как есть.
- Этапы 6 и 7 разбора. Нагрев: десять минут ролика 1080p60 в окне браузера — корпус 34 → 37,6 °C и почти встал,
  защита от перегрева не включилась ни разу, все кадры на месте (два дня назад такой же ролик доводил до 44 °C и
  срезанных частот). Свой регулятор тепла пока не нужен. Появился «чёрный ящик»: телефон сам раз в минуту записывает
  своё состояние на диск (память, потоки, перезапуски служб, температуру, мощность) — запись переживает перезагрузку,
  и по ней видно то, что портится часами: `tools/naomi blackbox`.
- Этап 8 разбора — фон и службы. Лишние службы Ubuntu Touch систему не грузят (ноль процессора, ~31 МБ) — оставлены.
  Настоящий шум был в контейнере Android: сборщик статистики Pixel для Google раз в секунду спрашивал службу, которой
  в телефоне нет, и будил ещё три системные службы — остановлен. Выключены «новости дня» Ubuntu (раз в сутки ходили
  на сервер Canonical). Всё сделанное за день проверено перезагрузкой телефона: поднимается само.

- Рабочий стол — разбор скорости (просьба пользователя: «прокликать всё на столе и чтобы на 75 Гц ничего не
  провисало»). Свой инструмент `tools/deskbench`: мышь приставки сама прокликивает все окна стола, а телефон и
  приставка пишут каждый кадр, нагрузку и кто держит поток интерфейса. Первое найденное и исправленное: приставка
  выбрасывала больше половины кадров при перетаскивании окон и держала лишние 30–50 мс задержки — теперь кадры не
  теряются, от кадра до экрана 32–60 мс вместо 55–82, щелчок отзывается заметно быстрее. Дальше — сам телефон:
  оболочка стола теперь работает на больших ядрах (кадр вдвое быстрее), растягивание «Файлов» больше не пересоздаёт
  шапку на каждом шаге, папки открываются вдвое быстрее, «Настройки» — за ~30 мс вместо 111, первый ролик в
  «Просмотре» больше не замораживает стол. Браузер: колесо и перетаскивание отзываются вдвое быстрее, растягивание
  окна — без рывков. Итог по всему прогону стола после перезагрузки: потерянных кадров в 9 раз меньше (384 → 43),
  от кадра до экрана 68 → 41 мс, щелчок отзывается за ~65 мс вместо ~97.
- «Терминал» на рабочем столе — свой, с нуля (просьба пользователя; вид окна выбран из десяти макетов — «Папка и
  программа»): плитка TERMINAL в меню N, TERM на панели, пока он открыт. Вкладки — как у браузера: «+», правой
  кнопкой — закрепить, переименовать, дублировать, закрыть; у вкладки значок папки, а когда в ней что-то работает —
  имя программы и бирюзовая точка. Режим дока — вкладки на панели, текст во весь стол. Вкладки и их программы
  переживают перезапуск оболочки; закрыли терминал или перезагрузили телефон — вкладки возвращаются с прежним текстом
  в тех же папках. Лёгкий: без дела стол не рисует ни кадра, служба вкладок — 0 процессора и 2–4 МБ памяти; `top` во
  вкладке — 5 кадров в секунду по 1,2 мс. Подробно — `docs/TERMINAL.md`.
- Звук стола в тишине больше не держит телефон проснувшимся: пока ничего не играет, захват звука для приставки стоит
  на паузе, и звуковая служба засыпает (было ~5 % ядра и 200 пробуждений в секунду постоянно). Заиграло — захват
  идёт сразу, начало звука не теряется.

- Интернет по кабелю работает и с выключенным Wi-Fi: приставка сама отвечает телефону, какой адрес у сайта (раньше
  это шло только через роутер по Wi-Fi, и без Wi-Fi сайты не открывались). Выдернули кабель — имена сразу находит
  роутер, без пауз. Заодно исправлено: Ubuntu Touch при каждом событии сети стирал маршрут через кабель, и интернет до
  минуты шёл по Wi-Fi.

- Окно «Интернет» на рабочем столе вместо окна Wi-Fi (вид выбран из макетов): значок у раскладки показывает, откуда
  интернет — три узла, когда по кабелю, веер — когда по Wi-Fi; в окне — выключатели кабеля и Wi-Fi значками,
  строка приставки («интернет», «без интернета», «выключен»), у Wi-Fi — «запасной», а «Сети рядом» свёрнуты и
  раскрываются щелчком. Выключили кабель — интернет идёт по Wi-Fi, картинка стола остаётся на кабеле.

- Интернет по кабелю: телефон в доке ходит в интернет через приставку (Pi в сети по Ethernet), а не по Wi-Fi. Выдернул
  кабель — картинка стола не пропадает и интернет сам переходит на Wi-Fi, вставил — снова кабель; у Pi пропал
  интернет — телефон заметит за минуту и вернётся на Wi-Fi. Проверено с настоящим YouTube: ролик ни разу не встал.
  Попутно: кабель у приставки теперь USB 3 — около гигабита, стол даже под полной нагрузкой кабеля не страдает.
  Исправлено: прогон стола, оборвавшись, оставлял «сторожа», который глушил каждый новый звук телефона.

- Значок пути на панели стола (кабель или без провода) больше не путается с Wi-Fi телефона: теперь это монитор на
  ножке, у его левого нижнего угла — вилка или волны, с одинаковым отступом от монитора. Вид выбран пользователем из
  макетов (сначала рамка экрана, потом — из десяти мониторов).

- Раскладка на рабочем столе (просьба пользователя): на панели теперь настоящая — EN или RU, а не вечное «EN»;
  щелчок по ней переключает, и она меняется от сочетаний клавиш у приставки — Alt+Shift, Ctrl+Пробел и, как в
  НаомиОС, Alt+Пробел.

- Wi-Fi на рабочем столе (просьба пользователя: «по подобию Bluetooth, чтобы можно было выбрать Wi-Fi и подключиться
  к сети»): значок у руны Bluetooth — дуги по силе сигнала, бирюзой на связи; щелчок — окно «03 WI-FI»: включить и
  выключить, свои сети и «СЕТИ РЯДОМ» с замками у закрытых; щелчок по сети — подключиться, по закрытой — поле пароля
  прямо под строкой (клавиатура приставки), по подключённой — отключить или забыть. Пароль не подошёл — сказано у
  самой сети, поле открывается снова. Заодно исправлено в службе Wi-Fi: о неудаче подключения она сообщала без имени
  сети — экран Wi-Fi телефона тоже не знал, к какой сети не подошёл пароль.

- Bluetooth на рабочем столе (просьба пользователя: «как на НаомиОС, один в один»): щелчок по руне Bluetooth на
  панели — окно «02 BLUETOOTH»: включить и выключить, лупа — поиск, свои устройства и «НАЙДЕНЫ РЯДОМ» с силой сигнала;
  щелчок по найденному — спарить и подключить, по своему — подключить или отключить; грань слева — состояние, справа —
  крутилка, код спаривания или заряд. Служба Bluetooth заодно научилась останавливать поиск, сообщать заряд,
  показывать «отключаю» и прятать вопрос о коде везде, когда на него ответили.

- Android-приложения больше не «висят на загрузке» насовсем (замечено пользователем после перезагрузки телефона:
  Google Play и приложения не открывались): изредка служба вывода Waydroid при первом окне встаёт, ожидая ответа
  оболочки, и за ней встаёт весь Android. Стол замечает окно без картинки (12 с) или Android, не загрузившийся за
  3 минуты, и сам перезапускает Android с теми же приложениями. Почему теряется ответ — ещё не найдено.

- Окна Android — на весь экран, как браузер в режиме панели (просьба пользователя): кнопка на полосе окна, окно —
  на всю область под панелью без полосы, а «в окно» и крестик переезжают на панель стола. Несколько таких окон — каждое
  на панели, кнопки — у верхнего. Крестик на панели — и у браузера в режиме панели: щелчок — свернуть, с Shift —
  закрыть. Заодно: развёрнутое двойным щелчком окно больше не съезжает влево (рамка ждёт, пока Android встанет).

- Окна Android больше не разваливаются (замечено пользователем: «Настройки» после растягивания и переноса —
  левая колонка отдельно, полоса отдельно): границы окна стол теперь берёт у самого Android, а не угадывает по
  картинкам — у «Настроек» шире 1200 пикселей это два окна в одной задаче. Перенос, растягивание через «две колонки /
  одна», «развернуть» и «вернуть» — окно цельное и встаёт, где отпустили. Для проверок без приставки — проверочная
  мышь стола (`tools/naomi mouse`).

- Окна Android тянутся за края и углы, как окна стола (замечено пользователем): у краёв — курсор ↔ ↕ ⤡ ⤢, тянет сам
  стол — плавно, полоса окна больше не вытягивается до края стола; когда отпустили, Android перекладывает приложение
  один раз (было: секунды, по шагу на каждое движение мыши).

- Окна Android — в общем стиле стола (просьба пользователя): углы скруглены, как у «Файлов» и браузера, с
  волосяной рамкой; двойной щелчок по полосе разворачивает окно во всю область под панелью и возвращает на прежнее
  место — приложение раскладывается заново (Subway Surfers во всю ширину — горизонтальный). Подписи приложений на
  панели — ровно по слову, черта под ними больше не шире подписи.

- Приложения Android — на панели стола, за FILE и WEB (просьба пользователя): черта снизу — окно на виду, вполсилы —
  свёрнуто; щелчок показывает и сворачивает, как FILE. У окна Android — полоса стола с одним крестиком: щелчок —
  свернуть, с Shift — закрыть приложение, как у браузера; кнопки Android («назад», «развернуть») убраны. Игра Subway
  Surfers больше не прячется под полосой: Android 13 кладёт полноэкранные игры под свою полосу, стол теперь показывает
  такую игру ниже полосы целиком, а нажатия попадают ровно туда, где их видно, — для любого такого приложения. Заодно:
  окно, спущенное за нижний край, больше не теряет это исправление и не уезжает вдвое дальше мыши, когда его тащат
  обратно; приложение под развёрнутым окном не пропадает с панели ([ANDROID.md](docs/ANDROID.md) «Полоса окна и
  панель»). Окно Android теперь тащит только стол — с первого нажатия, и неактивное тоже, без рывка в начале, — а
  когда отпустили, Android переносит задачу по команде (через naomid): перетаскивание окна игры больше не нажимает в
  игру (было: забег начинался, открывались окошки игры).

- Окна Android на столе двигаются гладко (просьба пользователя: «мерцает и 10 кадров, особенно с двумя окнами»): окно
  тащит сам стол, как «Файлы», — по шагу на каждый кадр, 70–75 кадров в секунду, без рывков назад и пропавших кадров
  (было 13–46 и окно прыгало); Android переносит окно один раз, когда отпустили. Пропало мерцание, когда на кадр
  исчезали все окна Android сразу (было 9 кадров из 1542 за 20 с с тремя окнами, стало 0). Окно, закрытое другим
  и снова открывшееся, встаёт на своё место, а не выскакивает наверх; у верхнего края полоса окна не теряется. Звук
  Android больше не пересоздаёт поток, когда телефон занят ([ANDROID.md](docs/ANDROID.md) «Перетаскивание и мерцание»).

- Игры Android на столе — разбор «подлагивает» (просьба пользователя), по замерам на телефоне: звук Android больше
  не крутит ядро впустую (после любого сбоя звука было 88–100 % одного ядра, часто главного, пока игра играет музыку;
  теперь 2–5 %), а застрявший звук Android сам лечится; кадры Android идут в такт монитору приставки — ровно по 13,3 мс вместо 11,
  11, 11, 11, 22 мс; игра в режиме «стол на полную» — на больших ядрах. Стресс-тест игры — `tools/naomi gamesoak`.
  Главная стена — нагрев: игра со столом берёт больше, чем телефон рассеивает без обдува ([ANDROID.md](docs/ANDROID.md)
  «Скорость»).

- Android-приложения с Google Play на столе (просьба пользователя): Waydroid — второй Android 13 в контейнере на
  том же ядре и видеочипе, не эмулятор. Меню N → новая плитка **ANDROID** → список «03 ANDROID», как «03 GAMES» у
  НаомиОС: приложения со значками, сами появляются после установки в Google Play и пропадают после удаления; справа —
  «Настройки» Android и Google Play. У каждого приложения своё окно со своей полосой Android: двигать, развернуть,
  свернуть, закрыть. Android включается по первому щелчку (15–20 с, плашка «ANDROID — ВКЛЮЧАЕТСЯ»), без окон спит,
  стол не смотрят — замерзает вместе с браузером. Мышь, колесо и клавиатура работают — и по-русски (раскладка та же,
  что на столе); экран телефона — ещё нет. Регистрация Google Play и вход в аккаунт — руками пользователя ([ANDROID.md](docs/ANDROID.md)).

- «Просмотр» на столе, как в НаомиОС (просьба пользователя): пробел в «Файлах» открывает окно просмотра — фото по
  центру, окно под его пропорции, на полосе имя, размер, формат и вес; ← → листают (выделение в «Файлах» идёт следом),
  F — на весь стол, Alt+Backspace — в корзину. Видео играет со звуком: пауза щелчком, перемотка по полосе хода.
  Быстро: фото 4000 × 3000 — на экране через 90 мс, соседние подгружены заранее; видео — аппаратным декодером
  телефона (процессора вчетверо меньше), а первый кадр — сразу, через 0,14 с. Щелчок по миниатюре скриншота открывает
  его здесь один к одному, пиксель в пиксель ([DESKTOP.md](docs/DESKTOP.md) «Просмотр»).

- Скриншоты стола, как в НаомиОС (просьба пользователя): **Shift+Alt+3** — весь стол, **Shift+Alt+4** — область
  прицелом (рамку тянут мышью; Esc или правая кнопка — отмена). В окне Mirror на Mac — ⇧⌥3 / ⇧⌥4, у клавиатуры Apple
  на приставке — и ⌘⇧3 / ⌘⇧4. Снимок — пиксель в пиксель, как на мониторе, в `~/Screenshots` («Скриншот 2026-11-18 в
  08.12.41.png», папка закреплена в «Файлах»), за 0,3–0,6 с; справа сверху выезжает миниатюра, щелчок по ней —
  снимок на весь стол один к одному, стрелки листают прошлые. Работает и когда в фокусе браузер — странице клавиша не
  уходит. Раскладка у приставки (Alt+Shift) теперь переключается, когда отпускают, — иначе скриншот менял бы её. Запись
  экрана пока не перенесена ([DESKTOP.md](docs/DESKTOP.md) «Скриншоты»).

- Эквалайзер в окне звука появляется вместе с окном (просьба пользователя): раньше он ждал, пока окно дорастёт, и сам
  открывал запись звука — столбики приходили через ~0,2 с. Теперь звук «Динамиков» он берёт у трансляции, которая и
  так снимает его для приставки, — столбики уже на месте в первом кадре окна; с выходом «Телефон» — через 35–47 мс,
  пока окно ещё растёт. И легче: второй записи звука нет (PulseAudio −1,75 % ядра), кадр эквалайзера на 20–33 %
  дешевле и точнее, а в тишине открытое окно больше не заставляет стол рисовать 30 одинаковых кадров в секунду
  ([DESKTOP.md](docs/DESKTOP.md) «Эквалайзер вместе с окном»).

- Край окна на столе (просьба пользователя): жёлтые полоски у края при наведении мыши убраны, остаётся только форма
  курсора ↔ ↕ ⤡ ⤢. Схватить край стало проще: полоса хвата 16 точек вместо 8 (12 снаружи окна, 4 внутри), у
  браузера 12 снаружи вместо 8; угол ловится на 32 точки вдоль каждой стороны — по всей дуге закругления, а не в
  квадрате 14 × 14. Кнопки окна не задеты ([DESKTOP.md](docs/DESKTOP.md) «Курсор»).

- Плитка «модем» в шторке и «Настройках» (просьба пользователя): выключает сотовый модем, как режим самолёта, но
  Wi-Fi и Bluetooth остаются. Выключен — не ищет сеть и не задевает Wi-Fi (в телефоне неактивная SIM, модем искал
  сеть без конца). Выбор помнится. С выключенным модемом стол по Wi-Fi: 6 прогонов, худший кадр по связи 7–13 мс
  ([NAOMID.md](docs/NAOMID.md), Cell).

- Стол по Wi-Fi без провалов (этап 5 «стола на полную»). Без кабеля перетаскивание окон через раз вставало на
  20–130 мс по нескольку раз подряд. Виноват оказался незанятый второй интерфейс Wi-Fi телефона: система сама искала
  на нём сети и на 3 секунды уводила радио с рабочего канала. Теперь не ищет: за 8 прогонов ни одного провала связи
  длиннее 17 мс (было — в 8 прогонах из 15). Ролик YouTube по Wi-Fi идёт как по кабелю
  ([STREAM.md](docs/STREAM.md) «Сканы wlan1»).

- Стол больше не рисует кадры-повторы (этап 4 «стола на полную»). При ролике YouTube картинка меняется 25 раз в
  секунду, а стол рисовал, сжимал и слал 60 кадров: Qt после каждого кадра страницы браузера просил ещё один, точную
  копию (страница идёт через слой ради скруглённых углов окна). Теперь повтор не рисуется: 30 кадров вместо 60,
  телефон берёт 2,9 Вт вместо 3,2–3,3, видеочип занят на 32 % вместо 48 %, поток вдвое тоньше; ролик и перетаскивание
  окон — как были. За 9 минут ролика корпус 42,0 °C вместо 44,2, плата у процессора 56,5 °C вместо 61,8
  ([DESKTOP.md](docs/DESKTOP.md) «Стол на полную»).

- Откуда тепло в режиме стола (этап 3): телефон сам показывает, сколько берёт от кабеля, — по нему и мерили. Покой
  1,2 Вт, перетаскивание окна 2,4, ролик YouTube 2,9; без обдува телефон рассеивает около 3 Вт. Декодирование ролика —
  малая часть (~0,3 Вт), больше всего тратит наш конвейер «нарисовать — упаковать — сжать — отправить» (~1 Вт).
  Аппаратный декодер в браузере не делаем: движок умеет только VA-API, драйвера для декодера телефона нет
  ([DESKTOP.md](docs/DESKTOP.md) «Стол на полную»).

- Кодер трансляции разобран заново (этап 2 «стола на полную»). Старый вывод «кодер тянет сжатие на весь период
  кадра» оказался ошибкой замерщика — он исправлен. На деле время сжатия задают «скорость работы» кодера и размер
  кадра: стол теперь всегда на наибольшей скорости (кадр сжимается 9,0–9,4 мс вместо 9,9, пропусков кадров из-за
  занятого буфера больше нет). Нашёлся и режим малой задержки (до 6,8 мс), но с ним поток вдвое толще и выигрыш
  съедают связь и распаковка — оставлен выключенным ([STREAM.md](docs/STREAM.md) «Кодер: честный замер»).

- Режим «стол на полную» (просьба пользователя: в режиме стола телефон — компьютер, без энергосбережения). Разбор от
  ядра до монитора: у телефона 2 больших ядра из 8, ускорителей Android в ядре нет, а оболочка со столом жила без
  приоритета, и частоты ядер проседали между кадрами. Теперь, пока стол смотрят, ядра не опускают частоту ниже
  середины, оболочка поднята, видеочип — 625 МГц; выключили стол — всё как у телефона. Кадр готовится 17,5 мс вместо
  20,4, худший — 24 вместо 32, окно едет без рывков. Нашлась следующая стена: на долгом ролике Android режет частоты
  по нагреву корпуса (37–43 °C) — с режимом и без. По выбору пользователя в режиме стола эти пороги — на 4 °C позже:
  за 9 минут ролика частоты ни разу не упали ниже пола, кадр готовится ровно 18 мс (было 21–22 под ограничением);
  корпус 44 °C ([DESKTOP.md](docs/DESKTOP.md) «Стол на полную»).

- Новая вкладка в браузере открывается сразу и без белой вспышки (разбор «почему браузер туго»): одна новая вкладка
  готова заранее, «+» её просто показывает — на мониторе через ~0,2 с после нажатия вместо ~0,45, ни одного белого
  кадра (было 1–3 на каждое открытие). Программы стола, пока стол смотрят, — на больших ядрах (механизм Android,
  который Ubuntu Touch не подключал). YouTube открывается дольше всего не из-за телефона: ~3 с сервер собирает твою
  ленту, ~3 с выполняется его JavaScript ([WEB.md](docs/WEB.md)).

- Приставка больше не держит большую задержку после открытия окон (просьба пользователя). Первые кадры после паузы
  всегда идут дольше (40–60 мс против 30) и раньше держали задержку высокой почти всю работу. Теперь у приставки две
  задержки: побольше — на начало движения, она ставится заранее, пока стол стоит; поменьше — пока тащат окно или
  листают, туда она сходит за доли секунды. Пока окно тащат — 36–44 мс вместо 52–62, выброшенных кадров на том же
  сценарии 9 → 5 ([PI.md](docs/PI.md) «Опережение: начало и движение»).

- «Файлы» открываются без рывков (жалоба пользователя: «анимация появления лагает, а если сразу двигать окно — лагает
  капец как, через пару секунд проходит»). Нашлось: невидимая подсветка списка Qt 2,4 с после открытия заставляла
  стол рисовать 75 кадров в секунду впустую, окно ~100 мс создавалось, пока стол стоял, а приставка в ответ поднимала
  задержку до 80 мс и выбрасывала по 20–33 кадра в секунду. Теперь подсветки нет, а одно окно «Файлов» собирается
  заранее, в покое, и по щелчку просто появляется. На одном и том же сценарии выброшенных кадров 247 → 9, от кадра до
  экрана 80 → 54 мс ([FILES.md](docs/FILES.md) «Открытие без рывков»).

- Окно «Файлов» открывается там, где его закрыли, и того же размера; закрыли развёрнутым — откроется развёрнутым
  (просьба пользователя). Помнит и после перезапуска; второе окно встаёт рядом лесенкой ([FILES.md](docs/FILES.md)).

- Новый курсор стола и подсказка у края окна (просьба пользователя: «видоизменение мышки, когда навожу на край окна…
  и мышку другого дизайна»). Из 10 наборов макета — «Мягкая»: тёплая белая, скруглённая, с мягкой тенью, и все её
  формы: рука над кнопками панели и ссылками, текст в полях, ↔ ↕ и диагонали у краёв окон, «тащу», пока окно тянут,
  «занято» — наша крутилка «Морфинг». Страницы браузера ставят свою форму сами (текст, ссылка). У края «Файлов» и
  браузера — янтарный «бегунок» за мышью, в углу — дугой по закруглению окна. Форма меняется за 7–12 мс
  ([DESKTOP.md](docs/DESKTOP.md) «Курсор»).

- Часы без дока — куда захочешь: в «Настройках» → «Без дока» их тащат мышью прямо на рисунке монитора, «На место» —
  вернуть слева сверху (просьба пользователя). И зелёный блок слева сверху, мелькавший при каждом возвращении в док
  (фото пользователя), — починен: приставка сжимала слой часов при уходе, и видеочип на это время терял цвет кадра под
  ними; теперь часы уходят и появляются прозрачностью со сдвигом, без масштаба ([PI.md](docs/PI.md) «Зелёный блок»).

- «Настройки» → «Экран» (вопрос пользователя: «масштабируемость? разрешение? что было бы актуально?»; макет —
  «да, строй»). Размер стола — крупнее или больше места (1280…2048 точек, сейчас 1600 — как 160 %), рядом монитор с
  окном «Файлов» в настоящих долях; резкость — сколько пикселей рисует телефон, с лупой и замерами; частота монитора —
  из того, что он умеет (60 или 75 Гц), меняется за 50 мс. Разрешение монитора менять незачем: приставка всегда
  включает родное и сама растягивает кадр. «Обои» — свои, НаомиОС, «Тёмные» или любая картинка из «Файлов» («Сделать
  фоном» кладёт её сюда же); «Без дока» — часы на обоях вкл/выкл, когда спит монитор (10 мин…никогда), будить ли его
  движением мыши. Всё меняется сразу, приставка помнит настройки и после перезагрузки ([DESKTOP.md](docs/DESKTOP.md)
  «Настройки» → «Экран»).

- «Настройки» на рабочем столе (просьба пользователя: «панель настроек… раздел экран и мышка… колёсико сделать мягкую
  прокрутку без рывков»): шестерёнка в меню N открывает окно в виде, выбранном из десяти макетов. «Мышь» — скорость
  указателя, направление колеса (обычное или обратное, как пальцем), сколько строк за щелчок, плавность и её мягкость;
  «Подробно» — наборы «Точно / Обычно / Быстро», график прокрутки и поле, где колесо пробуют сразу, разгон колеса и
  ускорение указателя с графиком. Настройки запоминаются и действуют на мышь у приставки — и в браузере тоже; по
  умолчанию всё как было ([DESKTOP.md](docs/DESKTOP.md) «Настройки»).

- Плитка «трансляция» в шторке — выключатель рабочего стола (пользователь: «малинка включена, и телефон постоянно к
  ней подключается… хочу, чтобы кнопка включала и выключала, выбор сохранялся, а выключенный режим реально не тратил
  ресурсы»). Выключено — ни Малинка, ни окно Mac телефон не смотрят: они ждут на одной тихой связи (проверка раз в
  30 с), а не стучатся каждые 1,5 с; включил — стол на мониторе через 0,36 с. Выключил посреди YouTube — всё замирает:
  ролики на паузе, браузер заморожен целиком (0 % процессора), включил — тот же ролик на том же месте, play — играет
  дальше. Если Малинка ушла сама — ролик сразу на паузу, звук больше не перескакивает на динамик телефона. Выбор
  сохраняется и после перезагрузки. Заодно: часы на панели стола больше не стоят ([STREAM.md](docs/STREAM.md)
  «Выключатель», [DESKTOP.md](docs/DESKTOP.md) «Выключатель», [WEB.md](docs/WEB.md) «Без зрителя»).

- Звук рабочего стола — на мониторе приставки (просьба пользователя: «научить выводить звук на подключённый
  монитор… кнопку колоночки задействовать, как в НаомиОС, и эквалайзер»): звук телефона идёт приставке теми же путями,
  что картинка, и звучит из HDMI монитора вровень с ней; выдернули кабель посреди звука — 0 опоздавших кусков. На
  панели стола значок звука открывает окно НаомиОС: громкость, MUTE, эквалайзер и выход — «Динамики» (монитор) или
  «Телефон», щелчком; выбор и громкость помнятся до следующего подключения. Клавиши громкости клавиатуры и колесо над
  значком — плашка громкости, как в НаомиОС ([PI.md](docs/PI.md) «Звук на мониторе», [DESKTOP.md](docs/DESKTOP.md)
  «Звук»).

- Экран телефона после перезагрузки больше не увеличен (пользователь прислал фото: шторка и часы в 1,5 раза крупнее,
  правый край срезан). Первая починка чёрного экрана отдавала дисплею кадр 720 × 1560, и дисплей перестраивался под
  этот размер. Теперь при отказе дисплей получает чёрный кадр размером с панель. Новая команда `tools/naomi panel`
  показывает по журналу драйвера, как кадр на самом деле лёг на панель ([DISPLAY.md](docs/DISPLAY.md)).

- Чёрный экран после перезагрузки телефона — исправлен (пользователь: «после перезагрузки на телефоне просто чёрный
  экран»): composer на первый кадр просил «составлю сам», и оболочка навсегда переводила кадр в вид, которого не умеет
  показывать; теперь отказ стоит одного кадра. 90 Гц после перезагрузки больше не «теряются» из-за ошибочного замера.
  Перезагрузить телефон можно самому: плитка «перезагрузка» в шторке и кнопки «перезагрузить» / «выключить» в меню N
  рабочего стола — со второго нажатия, как в НаомиОС ([DISPLAY.md](docs/DISPLAY.md), [NAOMID.md](docs/NAOMID.md)
  «Система»).

- Громкость телефона больше не «врёт» (пользователь: «в шторке 20, а играет на 100 %, чуть двину — встаёт на место»):
  драйвер звука терял громкость, когда динамик просыпался после тишины, — теперь громкость считает PulseAudio, и она та
  же всегда. Громкость телефона и монитора запоминается и после перезагрузки. Значки звука и пути на панели стола больше
  не сдвигают соседей, а стол после перезагрузки телефона поднимается сам ([NAOMID.md](docs/NAOMID.md) «Громкость
  динамика»).

- Звук на колонках монитора — без хрипа (пользователь: «пережатый, хриплый»; после: «звук стал хороший»): куски
  звука теперь встают встык (на каждом стыке, 100 раз в секунду, терялся или вставлялся кусочек), а звук идёт во float
  — громкость не съедает разрядов. Запись звука больше не может переехать на микрофон (из-за этого повис PulseAudio и
  понадобилась перезагрузка телефона) ([PI.md](docs/PI.md) «Звук на мониторе»).

- Звук стола — по проверке на настоящем YouTube (пользователь: «кликаю на иконку — звук пропадает, громкость всегда
  на 40 возвращается, клава невпопад»): пока телефон лежал в AOD, оболочка не слушала службу звука и стол видел
  громкость, устаревшую на минуты. Теперь: у каждого выхода своя громкость, переключение их не трогает и никогда не
  даёт тишины, клавиши и кнопки телефона (и в AOD) меняют громкость того, где звук; звук не стоит в очереди за
  кадрами — опоздавших кусков 0 и при выдернутом кабеле ([DESKTOP.md](docs/DESKTOP.md) «Звук», [PI.md](docs/PI.md)).

- Жест «домой» от самого нижнего края (пользователь: «из настроек свайпом не выходит, может через 10 секунд»): касание
  с последнего ряда сенсора Qt относил на точку ниже окна и выбрасывал весь смах; теперь с края — 5 из 5, было 0 из 7
  ([SHELL.md](docs/SHELL.md), «Жест от самого края»).

- «Настройки» открываются 1 в 1, как «Заметки» и «Наоми» (пользователь: «экран настроек медленнее открывается, чем
  блокнот и чат»): переход начинается через ≈ 20 мс после касания вместо 145–241 мс, открыто — за 0,30 с вместо 0,44,
  пропущенных кадров — 0 вместо 13 на 12 открытий. Страница создаётся один раз и живёт, как программа: закрыли —
  спрятана, открыли — там же, где была (раздел, прокрутка); опрос тока и нагрузки — после перехода
  ([SHELL.md](docs/SHELL.md), «Переходы»).

- Окно NaomiPhone Mirror само держит AWDL Mac выключенным, пока открыто, и включает обратно при закрытии (вопрос
  пользователя: «как выключить AWDL, чтобы не включался сам?»): macOS включает его сама, а он раз в полсекунды
  рвёт Wi-Fi на 90–190 мс. Нужно одно разрешение sudo на две команды — пользователь ставит его сам. По Wi-Fi:
  кадров позже 60 мс — 11 вместо 42, выброшено 1,4 % вместо 8,1 % ([STREAM.md](docs/STREAM.md), «Плавность по Wi-Fi»).

- Браузер не тратит батарею, когда стол никто не смотрит (вопрос пользователя: «закрыл окно на Mac — а на телефоне
  всё работает?»): трансляция и стол и так останавливались, а страницы браузера продолжали — ролик YouTube играл из
  динамика, процессы страниц ≈ 2 ядра. Теперь через 2 минуты без зрителя видео встаёт на паузу, страницы
  замораживаются (1 % ядра); окно открыли — оттаивают сразу, с того же места ([WEB.md](docs/WEB.md), «Без зрителя»).

- Форма рабочего стола — под окно на Mac (просьба пользователя: «у MacBook экран чуть выше — чтобы в ширину так же, а
  в высоту чуть больше»): стол той же ширины, высота — по окну NaomiPhone Mirror, от 16:9 до 4:3. Растянули окно или
  полный экран — стол перестраивается за доли секунды (на телефоне — 29 мс, пауза показа на Mac — меньше 0,1 с),
  полос нет; в полном экране Mac — 1920 × 1206, к/с и качество те же, что в 16:9 ([DESKTOP.md](docs/DESKTOP.md),
  «Форма стола»).

- Щелчки по кнопкам рабочего стола — всегда (пользователь: «кликаю FILE, WEB по очереди — проходит через раз»): у
  событий мыши стола не было места нажатия, и малейший сдвиг курсора во время щелчка кнопки панели считали
  перетаскиванием и отменяли. Теперь место и время нажатия — в каждом событии; щелчки с дрожанием — 36 из 36
  (было 0 из 12) ([DESKTOP.md](docs/DESKTOP.md), «Как устроено»).

- Браузер Naomi Web с НаомиОС на рабочем столе (просьба пользователя: «тот же браузер, сразу с логинами… чтобы так же
  трансформировался в панель, 1 в 1»): движок Qt WebEngine системы (Chromium 134) — отдельной программой, полосу
  рисует стол по меркам bcdesk — звезда «Избранного», вкладки со значками и бегом «грузится», строка адреса, «+»,
  меню вкладки и страницы, «Избранное» с правкой имени, загрузки, режим панели (вкладки на панели), видео на весь
  стол, ⌘-щелчок — новая вкладка; новая вкладка НаомиОС («Избранное» и «Часто»). `tools/naomi web-import` переносит
  с компьютера НаомиОС прямо в память телефона вход на сайтах (2493 куки, база одного вида — версия 24; значения
  расшифровываются), «Избранное», историю и значки — Google и YouTube сразу с входом. Окна программ теперь бывают
  и на столе (свой вывод Wayland); сайтам клавиши — нажатиями ([WEB.md](docs/WEB.md)).

- Окна стола — формы окон НаомиОС (замечание пользователя: «границы выходят»): боковая панель и строка «Файлов»
  закрывали рамку и вылезали за скругление. Теперь углы «непрерывные», как у модулей, волосяная рамка янтарём поверх
  содержимого, без тени (у окон НаомиОС мягких теней нет); у папок дома дата «16 ноя 1970» (так ядро запомнило их
  рождение до установки часов) — теперь время изменения ([DESKTOP.md](docs/DESKTOP.md), [FILES.md](docs/FILES.md)).

- Трансляция без квадратиков (просьба пользователя): на резких сменах всего кадра (окна, анимации) кодер квантовал до
  QP 51 — кадр распадался на блоки. Теперь потолок QP 22 (своя упакованная настройка Qualcomm) и цель 60 Мбит/с:
  худший блок — 22, кадров и к/с столько же, сжатие не медленнее; кадры на сменах крупнее (по Wi-Fi задержка на них
  выше). Для разведки — запись потока окном (`NAOMI_MIRROR_DUMP`), настройки кодера (`NAOMI_STREAM_CTRL`), частота
  стола (`NAOMI_DESK_FPS`) ([STREAM.md](docs/STREAM.md), «Качество»).

- Трансляция без шва между кабелем и Wi-Fi (просьба пользователя): окно держит оба подключения сразу, телефон шлёт
  каждый кадр по обоим, окно берёт первую копию, ввод — по обоим с номером. Вынули кабель — картинка не прерывается
  (ни нового зрителя, ни перезапуска кодера, ни опорного кадра), основной путь — Wi-Fi через 0,14–0,24 с; вставили —
  кабель встаёт основным, как только Mac поднял USB-сеть. Значок пути на панели рабочего стола (вилка / Wi-Fi) и
  заголовок окна — со слов телефона: на какой интерфейс пришло подключение, телефон узнаёт по ядру (`naomid`
  `Net.SshInterface`) ([STREAM.md](docs/STREAM.md), [DESKTOP.md](docs/DESKTOP.md), [NAOMID.md](docs/NAOMID.md)).

- Окно Mirror переходит между кабелем и Wi-Fi на ходу (вопрос пользователя: «вставляю и вынимаю кабель — ничего не
  меняется»): путь выбирался один раз при подключении. Теперь вставили кабель — через ≈ 6 с (столько Mac поднимает
  USB-сеть) картинка идёт по нему, вынули — через ≈ 1 с по Wi-Fi; в заголовке окна — «по кабелю» / «по Wi-Fi».
  Картинка и к/с по обоим путям одинаковые, кабель ровнее: связь 1,2 против 5,4 мс, без провалов
  ([STREAM.md](docs/STREAM.md)).

- Плавность трансляции по Wi-Fi (просьба пользователя): замер каждого кадра (`NAOMI_MIRROR_TRACE`) показал три
  причины. Mac сам раз в 0,52 с на ~90 мс уходит с канала (AWDL: AirDrop, Handoff, AirPlay) — выключить может только
  пользователь (`sudo ifconfig awdl0 down`); проверено с выключенным: провалы 80–95 мс раз в 0,52 с ушли, рывки
  показа p99 82 → 29 мс; окно подсказывает в ⌘I. Телефон со сбережением Wi-Fi отвечал
  с опозданием — теперь, пока есть зритель, радио без сна (`naomid` `HoldAwake`): пинг к телефону в покое 54 → 18 мс.
  Окно рисовало кадры сразу и затирало их — теперь в обновление экрана, каждый в своё: невидимых кадров 11% → меньше
  1%, после провала связи — плавный догон до 3 кадров ([STREAM.md](docs/STREAM.md), [NAOMID.md](docs/NAOMID.md)).

- «Файлы» НаомиОС на рабочем столе (просьба пользователя): перенос 1 в 1 по виду и логике на файловую систему
  телефона — «01 МЕСТА» (Дом, Избранное, Корзина с числами), «02 СВОИ» (закреплённые — закладки GTK), путь «ДОМ / …»,
  плитка и список с порядком по колонкам, звёздочки, корзина freedesktop (восстановить, удалить навсегда с вопросом,
  очистить), переименование на месте, новая папка, копировать / вырезать / вставить, отмена (Ctrl+Z, ОТМЕНИТЬ 8 с),
  перетаскивание в папки, места и между окнами, рамка выделения, просмотр фото пробелом, «Сделать фоном рабочего
  стола», клавиши НаомиОС и сочетания Finder в окне Mirror. Окна стола: двигать, развернуть, свернуть (FILE на
  панели), размер краями. Исправлено по пути: элементы стола не получали клавиш (окно за кадром без фокуса), двойной
  щелчок мышью стола, ⌘V в окне Mirror (не работал никогда), Ctrl / Shift при щелчке ([FILES.md](docs/FILES.md),
  [DESKTOP.md](docs/DESKTOP.md), [STREAM.md](docs/STREAM.md)).

- Рабочий стол 16:9 в окне NaomiPhone Mirror (просьба пользователя): второй экран оболочки 1280 × 720 точек (кадр
  1920 × 1080), которого нет на панели, — телефон в это время живёт своим интерфейсом. Вид — рабочий стол НаомиОС по
  тем же меркам: обои НаомиОС (`tools/naomi desktop-wallpaper`), панель сверху (N, звук и Bluetooth телефона,
  раскладка, дата и время), меню под N («01 NAOMIOS», «02 APP»); кнопки пока ничего не делают. В окне стол по
  умолчанию, ⌘1 / ⌘2 — стол / экран телефона без переподключения; мышь у стола — мышь, с наведением. От кадра стола до
  кадра на Mac — 25–38 мс по кабелю. Исправлено по пути: кадр 1920 × 1080 шёл в зелёных полосах (gralloc выравнивал
  строку буфера), окно застревало на первом кадре (холст `desynchronized`) ([DESKTOP.md](docs/DESKTOP.md),
  [STREAM.md](docs/STREAM.md)).

- Приложение NaomiPhone Mirror (`mirror/build.sh`) — в «Программах» и на рабочем столе, со своим значком (окно Mac с
  экраном телефона и курсором). Окно открывается под экран в пропорциях телефона (было: macOS ужимала его по высоте
  с заголовком, кадр был сжат на 2 %), при растягивании кадр не искажается. Проверено: запуск из Finder — живой
  экран телефона в окне, щелчки попадают ([STREAM.md](docs/STREAM.md)).

- Трансляция экрана на Mac с управлением (просьба пользователя): `tools/mirror` — окно с экраном телефона 720 × 1560,
  мышь — палец, трекпад — прокрутка, клавиатура Mac печатает (любая раскладка), Esc — «назад», ⌘↑ — «домой»,
  ⌘↓ — шторка. Кадр пакует видеочип прямо в буфер аппаратного кодера Venus (V4L2, без Android), H.264 идёт через SSH;
  на Mac — аппаратная распаковка (WebCodecs). От конца кадра в оболочке до кадра на Mac: 23–31 мс по кабелю,
  28–49 мс по Wi-Fi; из них 11–15 мс телефон ждёт и для своего экрана. Пока зрителя нет — ничего не стоит. Плитка
  «трансляция» в шторке. Программы принимают текст с внешней клавиатуры по D-Bus ([STREAM.md](docs/STREAM.md),
  [KEYBOARD.md](docs/KEYBOARD.md), «Внешняя клавиатура»). Не проверено руками на настоящей клавиатуре и трекпаде.

- Кадр 720 × 1560 (просьба пользователя — сравнить глазом с 1080 × 2340): плитка «разрешение» в шторке и «Настройках»
  (`1080p` / `720p`). В 720 оболочка рисует кадр в 2,25 раза меньше, а на панель его растягивает сам дисплей (слой
  DEVICE с масштабом, MDP Qualcomm) — бесплатно; раскладка та же, программы — с той же плотностью. Смена — перезапуск
  оболочки (≈ 2 с, код выхода 75 служба перезапускает). Проверено: 90 к/с без пропусков внутри анимаций, обе стороны
  переключения, «Заметки» во весь экран; глазом на панели — ещё нет ([DISPLAY.md](docs/DISPLAY.md), «Кадр меньше
  панели»).

- Студия в приложении NaomiPhone снова видит телефон. macOS не пускала приложение в локальную сеть (а телефон для Mac
  — её устройство и по кабелю, и по Wi-Fi) и путала его с PopkaStudio: у приложений на одном Electron одинаковый UUID
  исполняемого файла, и разрешение бралось от PopkaStudio. `studio/app/build.sh` теперь объявляет локальную сеть
  (вопрос macOS) и даёт исполняемым файлам NaomiPhone свои постоянные UUID. Проверено: точка телефона зелёная
  (studio/README, «Приложение для Mac»).

- Слой apps: стрелки «›» теперь второго цвета слоя, как подписи и значки (было — всегда серые `Theme.dim`, и при
  чёрном списке оставались светлыми; просьба пользователя). В студии этот цвет называется Rest. Проверено снимком
  телефона: список чёрный целиком (studio/README, «Виды слоёв»).

- Ночной замер AOD в две ступени без кабеля (13 ноября 2026): 6 ч 35 мин — 14,9 % батареи, средний ток 86 мА;
  первая ступень 124 мА, малое питание 84 мА, ровно каждый час. За 8 часов сна — ≈ 18 % (в одну ступень — 27 %,
  с погашенным экраном — 13 %). Ни одного пробуждения за ночь ([POWER.md](docs/POWER.md), «Ночь 13 ноября 2026»).

- Исправлено: после долгого AOD без кабеля (вторая ступень, малое питание) телефон просыпался без половины
  главного — панель Background не возвращалась, тёмные часы и подписи сливались с чёрным. Оболочка снимала малое
  питание при пробуждении, но не сообщала об этом интерфейсу. Проверено на телефоне: кнопкой и касанием из малого
  питания — главный целиком ([SHELL.md](docs/SHELL.md), «AOD»).

- Экрана блокировки больше нет (решение пользователя): включили экран — главный; AOD — тот же главный, притушенный
  на месте, без уменьшения и копии (обои под ним — ещё темнее, слои против выгорания сдвигаются от своего места).
  Погас экран в программе — она уходит в чёрный, под ним проявляется притушенный главный; программа живёт дальше.
  `LockScreen.qml` и `ClockFace.qml` удалены. Студия: первый экран проекта — главный (старые проекты перестраиваются
  при открытии: бывшая блокировка остаётся обычным экраном), Try начинается с главного, обои — только под главным
  ([SHELL.md](docs/SHELL.md), «AOD», «Проект студии»; studio/README).

- Автоблокировка (плитка «aod через» / «гасить экран» в шторке и «Настройках», After в System студии) — новые шаги по
  просьбе пользователя: 10 с → 1 → 3 → 5 → 10 мин → никогда (было 10 / 15 / 30 с / 1 / 5 мин). Значение не из шагов —
  касание ведёт на следующий выше. Проверено на телефоне касаниями по плитке ([SHELL.md](docs/SHELL.md), «AOD»).

- Слой «Background» в студии NaomiPhone (идея пользователя): верх экрана чисто чёрный (пиксели AMOLED не светятся),
  ниже — панель своего цвета и яркости со скруглёнными верхними углами: радиус и сглаживание угла (как у iPhone,
  «corner smoothing» Figma). Тащится вверх-вниз; на блокировке, главном и любом экране (studio/README).

- Выход с блокировки — без движения (просьба пользователя): свайп вверх больше не уводит часы и весь экран вверх —
  блокировка гаснет на месте, главный проявляется на месте, палец ведёт ход; открывается, если отпустить, когда
  блокировка уже погасла, или резко. Так же в Try студии ([SHELL.md](docs/SHELL.md), «Переходы»).

- AOD с обоями, как у новых iPhone (просьба пользователя): блокировка уходит в AOD вместе с картинкой — тускнеет на
  месте, кадр раз в минуту (панель командная, держит картинку сама). Вторая ступень (малое питание панели) — снова часы
  на чёрном. Выключается в System → Wallpaper → Always On ([SHELL.md](docs/SHELL.md), «Проект студии»).

- Обои: System → Wallpaper студии NaomiPhone — картинка с Mac (JPEG, PNG, HEIC с iPhone; можно бросить файл на
  поле) фоном на блокировке и/или главном. Размер, сдвиг, Fill и Fit, затемнение — в свойствах; на экране — рамка с
  углами: тащить и растягивать, за краями экрана картинка видна бледной. На телефоне одни обои под блокировкой и
  главным — стоят на месте, пока лист блокировки уходит вверх; в AOD их нет (гаснут на месте). Load и Live кладут
  картинку на телефон до проекта (studio/README, «Обои»; [SHELL.md](docs/SHELL.md), «Проект
  студии»). Пользователь предложил сделать обои пунктом System.

- Переход в AOD с блокировки проекта студии — снова без погашения: блокировка тускнеет и чуть уменьшается на месте,
  как свои часы (замер по снимкам посреди перехода: чёрного кадра нет; нашёл пользователь).

- Исправлено (нашёл пользователь): AOD с проектом студии показывал старые часы оболочки — кнопка блокировки
  давала «старый» экран, касание — новый. Теперь AOD — экран Always On проекта или его блокировка, притушенная, со
  сдвигом против выгорания ([SHELL.md](docs/SHELL.md), «Проект студии»). В папке Layers у батареи и погоды по
  плитке на каждый вид (Battery · Line, Ring, Bar, Icon; Weather · Line, Big, Card, Hours). Картинки плиток больше не
  теряются: очередь снимков движка держала живые ссылки на последнее сообщение, а не копии
  (studio/README).

- Экраны программ в студии NaomiPhone — настоящие: «Настройки» (страница оболочки), «Заметки» (лента), «Наоми»
  (чат) — их QML с телефона на выдуманных данных (`studio/qml/Fakes.qml`, заменители C++-типов в `studio/fakes/`);
  на карте, в Edit и в Try, где они отвечают пальцу (studio/README, «Экраны программ»).

- Слои «Батарея» и «Погода» в студии NaomiPhone и на телефоне, по четыре вида: батарея — строка, кольцо, полоска,
  значок; погода (Open-Meteo, город — поиском в свойствах) — строка, крупные цифры, карточка, по часам. Из
  проекта «Варианты: батарея и погода» пользователь оставил все восемь (studio/README).

- System студии NaomiPhone на телефоне: Always On (через сколько, малое питание), кнопка питания (ничего, выключить,
  в AOD, блокировка — новая настройка оболочки `powerKey`), Wi-Fi, Bluetooth, языки клавиатуры (RU · EN, RU, EN —
  `keyboard.ini`, общий для оболочки и программ). На телефон идёт только то, что меняли в студии; остальное студия
  показывает таким, какое оно на телефоне. Wi-Fi не выключить, пока связь только по Wi-Fi ([SHELL.md](docs/SHELL.md),
  «Проект студии»).

- Экраны проекта студии на телефоне: ссылка слоя открывает экран проекта поверх главного (как программу: тот же
  переход, «домой» снизу закрывает), с него — следующий; «назад» от края — предыдущий. Ссылка на экран программы —
  настоящая программа; ссылка на блокировке снимает блокировку и открывает экран (Try студии — так же). Исправлено:
  касание по блокировке доходило до строки программы на главном под ней ([SHELL.md](docs/SHELL.md), «Проект студии»).

- Live студии NaomiPhone: переключатель в Edit — каждая правка проекта сразу на телефоне, без Load (одно открытое
  SSH-соединение, правка доходит за 3–5 мс по кабелю; оболочка берёт её не позже чем через 16 мс). На телефоне другой
  проект или свои экраны — Live «на замке», щелчок предлагает Load и включается после него сам
  (studio/README, «Live»).

- Load студии NaomiPhone на телефон: кнопка отправляет проект (`~/.config/naomi/project.json`), и оболочка сразу
  показывает его экран блокировки и главный — без перезапуска (0,2 с); изменился QML оболочки — Load кладёт и его и
  перезапускает её (1,5 с). Экраны и слои — общий модуль `Naomi.Screens`: один QML рисует и в студии, и на телефоне.
  Строка программы на главном из проекта открывает настоящую программу; шторка — если её слой есть на главном;
  нет проекта — свои экраны. Точка телефона в студии: зелёная — этот проект, синяя — другой, жёлтая — свои экраны
  ([SHELL.md](docs/SHELL.md), «Проект студии»; studio/README, «Load»).

- Студия NaomiPhone — окно 1 в 1 как у Попка Студио (`studio/`, README): карточки, папки
  Screens / Layers / Apps / System, список экранов и слоёв, карта со связями, Edit / Try / Map, стартовое окно с
  Pixel 5 и проектами, About. Экраны на поле рисует один движок Qt — настоящий QML: блокировка (часы, дата,
  зарядка), главный (список программ), шторка — настоящая `QuickSettings.qml` оболочки; в Try мышь — палец
  (свайп вверх — главный, касание строки — программа, свайп от нижнего края — назад). Приложение
  `NaomiPhone.app` со своим значком (телефон с часами в рамке выделения) и ссылкой на рабочем столе.
  Load и Live на телефон — следующий шаг.

- Студия NaomiPhone — начало (`studio/`, README): конструктор экранов телефона по образцу
  Попка Студио. Проба движка удалась: настоящий QML оболочки (экран блокировки, главный, AOD, шейдер
  `MorphIcon`) рисуется в браузере тем же Qt 6.10.2, что на телефоне, собранным для WebAssembly; запуск 0,3–0,4 с,
  в покое 0 кадров, в анимации 120 в секунду. Помощник `studio.py` (порт 8476): проекты, паспорт Pixel 5,
  файлы движка.

- Чат по одной линии с клавиатурой: облачко ввода и лента — до краёв клавиш (до самого края экрана), облачко —
  со скруглением клавиши (5,2 точки) вместо капсулы. Совпадение проверено по пикселям снимка: края и
  скругление облачка и клавиш — пиксель в пиксель (просьба пользователя; CHAT.md, «По одной
  линии с клавиатурой»).

- Чат без шапки: ни названия «Naomi», ни затемнения — лента идёт до самого верхнего края экрана, под вырез
  камеры. Окно чата теперь от верхнего края (просьба пользователя; CHAT.md, «Шапки нет»).

- Цена датчика движения: два часа без кабеля с AOD в две ступени. Малое питание с работающим датчиком — 88 мА,
  без него было 87–89; обычный режим — 125 мА против 127. Ложных пробуждений нет. За 8 часов сна две ступени
  тратят ≈ 19 % батареи, одна — 27 % ([POWER.md](docs/POWER.md), «После обеда 12 ноября 2026»).

- AOD в две ступени (замысел пользователя): 20 минут AOD без движения — панель уходит в малое питание (87 мА
  против 127); сдвинули телефон — обратно в обычный режим, отсчёт заново; касание, отпечаток и кнопка будят из
  обеих. Движение — новый датчик `motion` службы `naomid` (акселерометр, 2 замера в секунду, отход больше
  50 mG). Плитка «aod» — по кругу «выкл» → «вкл» (две ступени) → «без экономии» ([SHELL.md](docs/SHELL.md),
  «AOD»; [NAOMID.md](docs/NAOMID.md), «Sensors»).

- AOD — вторая ступень, пока для замеров (`NAOMI_AOD_DEEP=секунд`): спустя столько секунд в AOD та же картинка
  остаётся, а панель уходит в режим малого питания; пробуждение возвращает её в обычный режим и 90 Гц. Замер
  днём без кабеля: малое питание — 87 мА, обычный режим — 127 мА, погашенный экран — 61 мА; цена AOD сверх
  погашенного экрана в малом питании в 2,5 раза меньше. Будят из малого питания кнопка, датчик отпечатка и
  касание экрана (контроллер касаний держится у процессора). Драйвер экрана снова знает три режима панели:
  выключена, включена, малое питание ([POWER.md](docs/POWER.md), «День 12 ноября 2026»; [SHELL.md](docs/SHELL.md),
  «AOD»; [DISPLAY.md](docs/DISPLAY.md), «Малое питание»).

- Ночь без кабеля в AOD (`device/battlog.sh`, 9,5 ч): 128 мА в среднем, 3,4 % заряда в час — вдвое больше, чем
  с погашенным экраном (61 мА, 1,6 %). Процессор тратит столько же, вся разница — панель в обычном режиме
  90 Гц при 47 нит: ≈ +275 мВт. За 8 часов сна — 27 % батареи против 13 % ([POWER.md](docs/POWER.md),
  «Ночь 12 ноября 2026»).

- «Настройки»: под «о телефоне» — «нагрузка» (занятая память из всей, загрузка процессора, частоты кластеров
  ядер) и «адреса и трафик» (адреса Wi-Fi, мобильной сети и кабеля, скорость приёма и отдачи); обновляются раз в
  3 с, пока «Настройки» открыты ([SHELL.md](docs/SHELL.md)).

- Блокнот: заметка открывается как программа с главного экрана — лента гаснет, заметка проявляется, вырастая,
  без сдвига вбок; выход — как был, вбок (NOTES.md, «Назад»).

- Чат: плюс в облачке ввода превращается в крутилку и обратно одной фигурой — лучи стягиваются в точку, точка
  раздувается кольцом, кольцо крутится «Морфингом»; конец — крутилка докручивает круг и стягивается в плюс.
  Рисует шейдер на видеочипе, движение — в потоке отрисовки: ровно 90 кадров/с даже при занятом интерфейсе
  (раньше — смена двух картинок с подтормаживанием). Новый общий элемент `Naomi.Ui/MorphIcon`
  ([SHELL.md](docs/SHELL.md), «Интерфейс»).

- Одна крутилка ожидания на весь телефон — «Морфинг» из ПопкаТВ (НаомиОС): белый контур перетекает круг →
  скруглённый квадрат → лист → круг с полным оборотом за 1,8 с, без круглой подложки. Стоит на заставке запуска
  программы (по центру, вместо названия), в чате («Наоми думает» вместо двух шариков, ожидание подключения), в
  блокноте (сохранение, удаление, обновление) ([SHELL.md](docs/SHELL.md), «Интерфейс»).

- Строки состояния больше нет: ни часов, ни батареи ни на одном экране — ни в программах, ни на экране
  блокировки (там оставались батарея). Сверху — пустая полоса выреза камеры; время — крупно на блокировке и в
  AOD, заряд — в шторке и «Настройках» ([SHELL.md](docs/SHELL.md), «Строки состояния нет»).

- «Заметки» — теперь блокнот Наоми, перенесённый из приложения для iPhone вместо листа текста: те же карточки,
  что в вебе, с сервера НаомиОС (пропуск — от сопряжения в «Наоми»), стопками по дням, меню облачков и
  «Напоминания». Заметка правится вживую, как в iOS: жирное жирным, списки с точками и номерами, чек-лист с
  квадратиками, цитата с полоской, код; Enter продолжает список, «- », «1. », «[] » в начале строки делают пункт;
  своя история на 10 шагов. Панель разметки над клавиатурой: жирный, код, цитата, список, номера, чек-лист,
  отмена, возврат. Тап где угодно ставит курсор; двойной тап выделяет слово с ручками по краям и меню
  «Вырезать / Скопировать / Вставить»; долгое нажатие водит курсор. Сохранение — галочкой, «Удалить» — в
  корзину. «Назад» от края закрывает заметку, а не программу (NOTES.md).
- Блокнот: долгое нажатие на слове сразу выделяет его (как двойной тап) и показывает «Вырезать /
  Скопировать / Вставить»; не отпуская, можно тянуть выделение. Тачпад клавиатуры — на 12 % быстрее пальца.
- Блокнот: текст больше не уезжает в пустоту под собой — край прокрутки — конец текста у клавиатуры (или у
  низа экрана). Ручка выделения у края прокручивает, только если там ещё есть текст, и выделение идёт за
  прокруткой; спрятал клавиатуру — выделение снимается (NOTES.md, «Касания»).
- Клавиатура — тачпад, как в iOS: пробел подержать (или повести по нему вбок) — клавиши гаснут, палец водит
  курсор в любом поле ([KEYBOARD.md](docs/KEYBOARD.md), «Тачпад»). Касания по клавиатуре больше не проходят к
  тому, что под ней.

- Клавиатура: смена букв и знаков — в ≈ 4 раза быстрее: «123» и «#+=» срабатывают по касанию, а все страницы
  созданы заранее (было — по отпусканию и с пересозданием клавиш: ≈ 180 мс от касания до кадра, стало ≈ 40)
  ([KEYBOARD.md](docs/KEYBOARD.md), «Смена страницы»).
- Исправление опечаток меняет и слово из словаря, если касание легло у самой границы клавиш, а другое слово
  в 20 раз вероятнее: «деда» пальцем за границей «л» → «дела». Проба с разбросом пальца пользователя: слов
  «как в речи» верно после исправления 99,2 % вместо 98,2 %, испорченных верно набранных — 0
  ([KEYBOARD.md](docs/KEYBOARD.md), «Исправление опечаток»).
- Клавиатура — до упора: крайние клавиши у самого края экрана (было поле 10 пикселей, как на снимке iOS);
  промежуток между клавишами — 14 пикселей вместо 17, клавиши шире (85 вместо 82) и чуть выше (125 вместо
  118). Без янтаря и рамок: «Ввод», ⇧, нажатая клавиша — в цвет клавиш. Значок Shift больше не двоится контуром.
- Клавиатура попадает туда, куда целишься: точка касания поднята на 30 пикселей. Замер набора пользователя
  (`tools/kbtaps`, 72 буквы): палец ложился ниже клавиши — на 63 пикселя в верхнем ряду, — все 9 промахов были
  клавишей ниже («е» → «п», «к» → «а»); с поправкой — 1 ([KEYBOARD.md](docs/KEYBOARD.md), «Попадание»).
- Главный экран — только список программ внизу, как в приложении Tesla (образец пользователя): «Наоми»,
  «Заметки», «Настройки» — значок, название, подпись, «›». Строки состояния на главном больше нет (проявляется
  вместе с программой), плитки-заглушки (телефон, камера, …) и «Тест экрана» убраны ([SHELL.md](docs/SHELL.md)).
- AOD из программы или с главного экрана — через чёрный, как переход программы и главного, без движения:
  экран гаснет полностью, потом проявляются часы; раньше часы проявлялись поверх гаснущей программы.
  С экрана блокировки — как было: те же часы тускнеют и уменьшаются ([SHELL.md](docs/SHELL.md), «AOD»).
- Шрифты Google — мягкие, скруглённые: весь текст и подписи — Google Sans (капс Medium с разрядкой), цифры
  часов на блокировке и в AOD — Google Sans Flex со скруглением на максимуме и тонкой насыщенностью; прежний
  Ubuntu Sans пользователю не нравился. Моноширинный остался только у HUD и «Теста экрана». Время в строке
  состояния — на прежнем месте по вырезу камеры (замер по снимку) ([SHELL.md](docs/SHELL.md), «Шрифты»).
- AOD — «экран всегда включён»: вместо темноты — часы, дата и зарядка на чёрном, там же, где на блокировке,
  на 4 % меньше и тусклее, со смещением против выгорания; кадр — раз в минуту. Включается плиткой «aod» в
  шторке и «Настройках», рядом «aod через» — 10 с / 15 с / 30 с / 1 / 5 мин / никогда. Будят касание экрана,
  касание датчика отпечатка (его прерывание) и кнопка питания. Переход плавный: часы блокировки переливаются
  в часы AOD, не пропадая, яркость идёт за ними — кадр за кадром при 90 Гц, без пропусков. Панель при этом в
  обычном режиме: её режим малого питания пробовали — при смене режима она гаснет, часы мигали; копии
  часов блокировки и AOD совпадают точка в точку — контур в переходе не двоится
  ([SHELL.md](docs/SHELL.md), «AOD»; [DISPLAY.md](docs/DISPLAY.md), «Малое питание»). Яркость теперь пишет
  свой поток — запись держала интерфейс до 11 мс. Расход батареи в AOD не измерен.
- Программы под пальцем — 90 кадров в секунду, было 45 (нашёл пользователь: «лагает, когда веду палец по
  чату»). Поток интерфейса оболочки ~10 мс каждого кадра стоял в синхронизации до vsync и не читал Wayland:
  кадр программы и ответ ей «рисуй дальше» проходили через кадр. Теперь кадр Qt Quick начинается по концу
  обмена буферов прошлого, в пути не больше одного кадра; касания уходят программам в начале синхронизации,
  раньше ответа — кадров без сдвига под пальцем 1 из 107, было 14 ([SHELL.md](docs/SHELL.md), «Кадры
  программ»). В переходах оболочки поток интерфейса больше не стоит (≥ 5 мс — 0 кадров из 225, было 136 из
  235).
- Клавиатура уходит из-под пальца пружиной со скоростью пальца, как в iOS: кривая снята по кадрам с видео
  пользователя (Telegram на iPhone) — ω = 26 1/с, затухание 0,8. Раньше после отпускания она срывалась в ~5
  раз быстрее пальца (OutCubic) и кадр стояла на месте. Облачко садится на место плавно, а не упирается на
  ходу; лента, оказавшаяся ниже нового низа, едет вниз вместе с облачком — без пустоты между ними
  ([KEYBOARD.md](docs/KEYBOARD.md), «В окне программы»).
- Ночь без кабеля с выключенным экраном (`device/battlog.sh`): 61 мА в среднем, 1,6 % заряда в час —
  телефон ни разу не уснул, глубокого сна чипа нет. Больше всех процессора ушло на `servicemanager` и `init`:
  службы производителя по 5 раз в секунду ищут службу статистики Android, которой в системе нет
  ([POWER.md](docs/POWER.md)). Настоящий сон пока не делаем — решение пользователя: телефон с погасшим
  экраном остаётся «живым», как обычный Linux.
- Клавиатура — в окне самой программы (чат, «Заметки»): модуль ввода и исправление — общая библиотека
  `shell/input`, клавиатура — `Naomi.Ui/KeyboardPanel`. Чат и клавиатура едут кадр в кадр при выезде и
  уходе (выезд чуть медленнее: 320 / 280 мс), клавиатуру можно смахнуть пальцем по ленте — облачко с ней
  идут за пальцем ([KEYBOARD.md](docs/KEYBOARD.md), «В окне программы»).
- «Наоми»: с клавиатурой над облачком ввода больше нет лишнего промежутка — лента вставала на старый низ,
  на 30 точек дальше (CHAT.md).
- Жесты без рывка в начале: «домой», «назад», шторка и лист блокировки ведутся от места, где палец был при
  подхвате, — раньше переход за один кадр прыгал на порог жеста (10 dp) ([SHELL.md](docs/SHELL.md),
  «Переходы»).
- Программа с открытой клавиатурой уходит домой одним окном: клавиатура едет вместе с ней, а не
  уезжает посреди жеста, окно не меняет размер. Открывается снова — уже с клавиатурой, как уходила.
  В чате облачко ввода больше не прыгает посреди жеста: запас под зону «домой» — по высоте окна
  ([SHELL.md](docs/SHELL.md), «Переходы»).
- Клавиатура: ряды чуть плотнее (на снимке iOS 39, теперь 33), полоска вариантов над клавишами убрана —
  исправление пробелом осталось ([KEYBOARD.md](docs/KEYBOARD.md)).
- Клавиатура — как у iOS по снимку пользователя (цвета свои): края, промежутки и высота клавиш в тех же
  пропорциях, нижний ряд «123 / Пробел / Ввод», две страницы знаков, глобус и «спрятать» под клавишами,
  два пробела — «. ».
- Исправление опечаток — своё, локальное: слово ищется по точкам касаний в дереве из 484 тыс. русских
  слов (частоты субтитров, FrequencyWords, CC BY-SA 4.0; `tools/naomi dict`), над клавишами — полоска из
  трёх вариантов, пробел заменяет слово, «стереть» сразу после — возвращает набранное и запоминает его.
  Проба на телефоне: небрежный набор — 18,6 % слов верно, после исправления — 86,6 %; разбор слова — 0,7 мс
  ([KEYBOARD.md](docs/KEYBOARD.md)).
- Клавиатура выше на ряд клавиш: нижний ряд был слишком низко, у самой полосы «домой»; «скрыть» — в
  освободившемся ряду справа.
- Программа «Наоми» (плитка «НАО»): чат с настоящей Наоми из НаомиОС — экран чата перенесён из приложения
  для iPhone, только переписка. Подключение — по QR из веба Наоми, камерой телефона (libhybris + libzbar);
  ответ идёт словами с проявлением букв, дела Наоми — плашками, «стоп», реакции, ответ на сообщение
  (CHAT.md). Пользователь подключился и переписался; подача словами и «думает» после этого
  исправлены и проверены пробой. `tools/naomi fonts` вернулся (шрифты Google Sans и значки Material Symbols).
- Дисплей: при прокрутке были горизонтальные полосы и белые точки — видеочип рисовал новый кадр в буфер,
  который панель ещё показывала (метка освобождения буфера была пустой). Теперь буфер прошлого кадра
  свободен по present fence текущего, как в Android ([DISPLAY.md](docs/DISPLAY.md), «Буферы»). Пользователь
  проверил: пропали.
- Новый интерфейс, шаг 4 — программы: открыть, «назад» и домой — переходы Попка Студио (7″) одним модулем
  `Naomi.Ui/Axis`: открыть — без направления (главный гаснет, программа проявляется и растёт с 0,92; 0,30 с),
  «назад» — вбок (0,26 с), домой снизу — по вертикали (0,38 с); палец ведёт
  переход, смахнул — сработало, без отскока. Разделы «Настроек» — так же ([SHELL.md](docs/SHELL.md),
  «Переходы»).
- Новый интерфейс, шаг 3 — шторка: отдельный чёрный экран, переход к ней и назад — как переход к галерее в
  Попка Студио на 7-дюймовой плате (вертикальная ось Material, 0,38 с, без отскока); смахнул вверх как угодно —
  закрылась ([SHELL.md](docs/SHELL.md), «Переходы»).
- Новый интерфейс, шаг 2: фон всех экранов и панелей — чистый чёрный (AMOLED: чёрная точка не светится);
  на главном — только значки программ (без названия, версии и даты); на блокировке нет подсказки
  «проведи вверх»; HUD по умолчанию выключен (включается в шторке).
- Новый интерфейс, шаг 1 — строка состояния: выше (50 точек, до низа выреза камеры), время и батарея
  ровно по середине выреза, поля симметричны ему; крупнее время, батарея-таблетка с процентом внутри
  (чёрные цифры по центру); без Wi-Fi и сотовой сети ([SHELL.md](docs/SHELL.md), «Строка состояния»).
- Интерфейс в духе Android 17 / Material 3 Expressive попробовали — веб-макет `design/mockup` и его перенос
  в оболочку — и по решению пользователя откатили к виду 0.3: новый интерфейс строим с нуля вместе, как
  НаомиОС ([UI.md](docs/UI.md): почему, что узнали; Android-вариант — в истории git). Автоповорот ушёл
  вместе с ним.
- Кабель разработчика: своя служба `naomi-usb` вместо `usb-moded` — ADB и сеть по USB (NCM) всегда,
  с 15-й секунды загрузки, без переключения режимов. `tools/naomi` ходит по кабельной сети
  (≈ 36 МБ/с против ≈ 10 по Wi-Fi), установка — `tools/naomi usb install`.
- Служба железа `naomid`: батарея, Wi-Fi (состояние, сети вокруг, включить, подключиться, забыть),
  сотовая сеть — по событиям NetworkManager, UPower и oFono, без опроса. Оболочка берёт данные у неё
  (`naomiPhone`) вместо своего опроса раз в 3 с. `tools/naomi phone` — состояние службы.
- `tools/naomi install` больше не переставляет пакеты композитора той же версии: из-за этого корень
  оставался открытым на запись.
- Экран Wi-Fi: «Настройки» → «сети» → Wi-Fi (или долгое нажатие на «wi-fi» в шторке) — включить, сети
  рядом, подключиться, пароль своей клавиатурой, отключиться, забыть. Переключатель Wi-Fi — в шторке и
  «Настройках». «Назад» умеет шаг внутри страницы. Клавиатура в поле пароля — без заглавных сама по себе
  и сразу на латинице.
- Звук: в службе — громкость, «без звука», куда идёт звук (PulseAudio пользователя через `pactl`);
  кнопки громкости работают (громкость поверх экрана), ползунок громкости и «звук» — в шторке и
  «Настройках». Наушники Bluetooth подключились — звук переводится на них.
- Bluetooth: в службе — включить, устройства (свои и рядом), спарить с подтверждением кода, подключить,
  отключить, забыть; переключатель в шторке, экран Bluetooth в «Настройках» (долгое нажатие в шторке).
- Касание по пустому месту шторки больше её не закрывает (закрывает касание ниже неё).
- Датчики: в службе `naomid` — положение телефона и освещённость от службы датчиков порта (`sensorfwd`, свой
  клиент её протокола); работают, только пока их просят. Автояркость: яркость по освещению, ползунок
  сдвигает кривую под себя, тень руки не мигает экраном; плитка «автояркость» в шторке и «Настройках»
  ([SHELL.md](docs/SHELL.md), «Автояркость»).
- Касания в окнах программ попадают точно: раньше палец попадал в 2,75 раза ближе к углу окна, и курсор в
  «Заметках» вставал в первую строку. Клавиатура в «Заметках» — с первого касания.

## 0.3 — 10 ноября 2026: окна и клавиатура

- Оболочка — Wayland-композитор: программы — отдельные процессы со своими окнами, GPU без копий
  (буферы Android через libhybris), дробная плотность 2,75 без размытия — окно ложится на экран 1:1.
- Своя клавиатура: русская ЙЦУКЕН, английская, цифры и знаки; печать двумя пальцами внахлёст, ё и ъ
  долгим нажатием, цифры на верхнем ряду, заглавные сами, повтор «стереть». Окно программы над ней
  становится ниже, поле прокручивается к курсору.
- Первая программа — «Заметки»: сохраняются сами.
- Дома и при погасшем экране программы неактивны и ничего не рисуют (0 тиков процессора за 10 с).
- Остановка оболочки мягко закрывает программы — они успевают сохраниться.
- Исправлено: жест «назад» не срабатывал; «назад» теперь сначала прячет клавиатуру.
- Журнал systemd — в памяти (на корне он забил место до 100%); снимок экрана больше не обрезается.
- Для разработки: `NAOMI_FRAMESTATS=drops` — каждый пропуск кадра с соседними интервалами;
  виртуальный палец набирает текст (`touchinject.py tap X Y X Y …`).
- Проверено виртуальным пальцем и снимками; пользователем — набор пальцем в «Заметках» на телефоне.

## 0.2 — 10 ноября 2026: телефон живёт сам

- Оболочка — системная служба: стартует при включении (≈ 21 с), перезапускается при падении,
  4 падения за минуту — безопасный режим (экран-заглушка, SSH и ADB работают).
- Кнопка питания гасит и включает экран; без касаний экран гаснет через минуту (настраивается);
  при выключенном экране кадры не выводятся. `repowerd` выключен, `logind` кнопку не трогает
  (иначе нажатие выключало телефон).
- Экран блокировки, главный экран с сеткой программ, строка состояния (батарея, Wi-Fi, сеть — живые),
  шторка (яркость, 90 Гц, HUD, гашение), «Настройки» с «о телефоне», «Тест экрана».
- Жесты: снизу вверх — домой, от края — назад, сверху вниз — шторка.
- HUD разработчика, снимок экрана с Mac (`tools/naomi shot`), виртуальная клавиатура для проверок.
- Касания: фильтр движения контроллера выключен, восстанавливается после включения экрана.
- Проверено пользователем на телефоне: кнопка питания, разблокировка, программы и жесты, шторка, яркость.

## 0.1 — 10 ноября 2026: свой экран

- Свой драйвер экрана для Qt (`eglfs_naomi`): Qt Quick прямо в HWC2 через libhybris, 90 Гц без пропусков.
- Касания каждый кадр (`QT_QPA_UPDATE_IDLE_TIME=0`).
- Среда сборки на телефоне, режим разработчика по кабелю: опись, тесты железа, нагрев, виртуальный палец.
