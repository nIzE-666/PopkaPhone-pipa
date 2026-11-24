# PopkaPhone для Xiaomi Pad 6 (`pipa`)

**Полноценный настольный Linux (Ubuntu Touch 26.04 LTS / Halium 13 + оболочка PopkaPhone) на планшете Xiaomi Pad 6 (`pipa`) с dual-boot на слоте `_b` без удаления Android на слоте `_a`.**

> Форк и планшетный порт оригинального проекта [PopaSpinka/PopkaPhone](https://github.com/PopaSpinka/PopkaPhone) (MIT), адаптированный под 11″ экран 2880×1800 60/120/144 Гц, чипсет Snapdragon 870 (`SM8250`, `kona`), нативные камеры, продвинутый Linux-терминал, удалённое управление через Telegram-бота и Windows 11 Mirror.
>
> **English summary:** Full desktop Linux shell for **Xiaomi Pad 6 (`pipa`)** running Ubuntu Touch 26.04 (Halium 13) on slot `_b` alongside stock/custom Android on slot `_a` (no data wipe). Includes SM8250 power/thermal/ZRAM tuning, native 120/60 Hz HWC2 compositor, PiP & 50/50 split-screen windows, touch-friendly Wayland terminal, native 13 MP / 8 MP `libhybris` camera app (`naomi-camera`), Pro Settings (24h clock, night filter, CPU profiles, service toggles), a 20-command Russian Telegram Control Hub (`extras/tg-bot`), Windows PC Companion Agent (`extras/pc-agent`), and Windows 11 Mirror client (`mirror/`).

---

## Возможности

### 1. Система, ядро SM8250 и автономность (`device/shell-prepare`)
- **Dual-boot без потери Android**: Ubuntu Touch 26.04 LTS (Halium 13) ставится на слот `_b` (ext4-контейнер на `/data`), Android остаётся нетронутым на слоте `_a`. Переключение систем в 1 тап — из меню `N` («Перезагрузить в One UI») и через Android-приложение/плитку `PopkaPhone-Boot-Switch` (`tablet/boot-switch/`).
- **Тюнинг Snapdragon 870 (`SM8250`)**: динамический губернатор `schedutil` + `input_boost` (`0:1344000`, 120 мс), разблокированные C-states (`lpm_levels`), динамические шины L3/LLCC/DDR/NPU (`mem_latency`/`bw_hwmon`/`cdspl3`), разнесённые IRQ (`kgsl` / `msm_drm`). Температура в простое ~39–40 °C.
- **Память и хранилище**: 4 ГБ ZRAM (`zstd`, 8 потоков, `swappiness=100`, `page-cluster=0`), `read_ahead_kb=128`, ограничение `journald`, нативный звук PulseAudio 48 kHz (`avoid-resampling=yes`).
- **Адаптивная частота экрана**: 120 Гц в работе / 60 Гц в покое (с гистерезисом 2 кадра), выбор 30/48/50/60/90/120/144 Гц.
- **Защита батареи**: ограничение заряда на 80% через аппаратный узел `night_charging`.

### 2. Нативный рабочий стол и эргономика планшета (`shell/`)
- **Оконный композитор Wayland на 2880×1800 (`DPR 2.0`, 1440×900 dp)** с приоритетом `Nice=-5`.
- **Многозадачность**:
  - **Split-Screen 50/50** — кнопки прижатия любого окна к левой/правой половине экрана.
  - **Always-on-Top / PiP** — кнопка булавки в заголовке держит выбранное окно поверх всех остальных.
  - **История буфера обмена (`06 БУФЕР`)** в верхней панели (15 последних записей) + кнопки мгновенного обмена буфером с Windows ПК (`С ПК → Планшет` / `Планшет → ПК`).
  - **Быстрый скриншот** из панели с автокопированием в буфер обмена.
  - **Автоповорот и блокировка ориентации**: тап — мгновенный поворот 90°, долгое нажатие — вкл/выкл автоповорота.
  - **Авто-режим «Ноутбук»**: при подключении USB/Bluetooth-клавиатуры сенсорная панель спецклавиш в терминале сворачивается автоматически; мониторинг USB-C DisplayPort Alt Mode (`card0-DP-1`).
- **Прокачанный терминал (`naomi-term` + `TermWindow.qml`)**:
  - Сенсорная панель спецклавиш (`ESC`, `TAB`, `^C`, `^D`, `^R`, стрелки, `|`, `/`, `~`, `-`, `COPY`, `PASTE`, `A−`, `A+`) с виброоткликом.
  - Жесты: скролл пальцем, долгое нажатие (Copy/Paste), щипок двумя пальцами и `Ctrl+`/`Ctrl-` для размера шрифта.
  - Автопроброс `WAYLAND_DISPLAY=wayland-naomi` — GUI-приложения, запущенные из консоли, открываются окнами в оболочке.
  - Кнопка **«Открыть в Терминале»** в файловом менеджере.
- **Приложения**:
  - **Раздел `03 LINUX`** с поиском в меню `N` (все установленные `.desktop`-программы).
  - **Браузер (`naomi-web`, Qt WebEngine)** с GPU-растеризацией, плавным скроллом, фоновой загрузкой блокировщика рекламы и переключателем тёмной/светлой темы на вкладку.
  - **Android-приложения (Waydroid)** с автоостановкой контейнера после 10 минут простоя без открытых окон.
  - **Удалённый экран ПК**: нативный Wayland RDP (`wlfreerdp`) и Parsec Web.
- **Продвинутые Настройки**:
  - **Интерфейс**: 24-часовой и 12-часовой формат времени, секунды, русская/компактная дата, **ночной тёплый фильтр экрана** (0–60%), шрифт и режим спецклавиш терминала, управление буфером обмена.
  - **Система**: переключение профиля CPU (`Эконом / Баланс / Максимум`), живые температуры, ZRAM, свободное место, **тумблеры служб** (Telegram-бот, Камера, Waydroid, Desktop Commander), часовой пояс, NTP и очистка журнала.
  - **Экран, Обои, Батарея (лимит 80%), Здоровье, Мышь, Геймпад, VPN**.

### 3. Нативное приложение «Камера» (`shell/apps/camera/` → `naomi-camera`)
- Работает напрямую с Android Camera HAL на Halium 13 через `libhybris` (`libcamera.so.1`) — поддерживает обе камеры планшета:
  - **Задняя 13 МП (`4208×3120`)** — фото, зум (`1.0×..6.0×`), автофокус по тапу, вспышка (`Выкл / Авто / Вкл / Фонарик`).
  - **Фронтальная 8 МП (`3264×2448`)** — фото, авто-зеркало, зум.
- **Запись видео MP4 (H.264)** через `ffmpeg` (`~/Videos/`), **таймер (3 с / 10 с)**, выбор разрешения, превью последнего снимка (`~/Pictures/Camera/`).
- **CLI-режим без GUI**: `naomi-camera --snap back|front --out photo.jpg` и `--torch <сек>` (используется Telegram-ботом).

### 4. Telegram-бот центр управления (`extras/tg-bot/`) и ПК-агент (`extras/pc-agent/`)
- Работает 24/7 на планшете (`aiogram 3`, systemd user unit + `Linger=yes`, поддержка `TELEGRAM_PROXY` SOCKS5).
- **20 русских команд** с описаниями и интерактивными inline-пультами + защита по `OWNER_ID` (одноразовая 6-значная привязка `/pair`):
  - Управление планшетом (`/tablet`, `/status`, `/screen`, `/apps`, `/files`, `/alerts`).
  - **Интерактивный bash-терминал (`/term`)** с сохранением текущей папки `cd` + разовые команды `/cmd`.
  - **Удалённая камера и фонарик (`/cam`)**: фото с задней 13 МП / фронтальной 8 МП камеры прямо в Telegram, зум 2×/4×, вспышка, фонарик.
  - **Управление Windows ПК (`/pc` + `extras/pc-agent/pc_agent.py`)**: Wake-on-LAN, статус CPU/RAM, скриншот экрана ПК, громкость/медиа, блокировка, сон, **общий буфер обмена (`/clip`)**.
  - **Samsung Smart TV (`/tv`, `/tv_text`, `/tv_cast`)**: пульт по Tizen WebSocket (`8002`/`8001`), WoL, **ввод текста в поиск ТВ**, **DLNA-каст видео/фото/ссылок** через встроенный LAN-медиасервер (`:18766` → `:9197`).
  - **Android TV / Chromecast (`/atv`)**: пульт и приложения через ADB (`5555`).
  - **Макро-сценарии (`/scenes`)**: *Кино*, *Работа*, *Ночь* в 1 тап.

### 5. Клиент зеркалирования для Windows 11 (`mirror/`, `tools/mirror_win.py`)
- Порт Electron-клиента `NaomiPhone Mirror` под Windows 11 с подключением по USB (`adb forward`) и Wi-Fi, аппаратным декодированием H.264/HEVC (Annex B), передачей мыши, клавиатуры и буфера обмена. Подробнее: [docs/WINDOWS-MIRROR.md](docs/WINDOWS-MIRROR.md).

---

## Матрица работы железа (Xiaomi Pad 6 `pipa`)

| Компонент | Статус | Примечание |
|---|---|---|
| Дисплей 11″ 2880×1800 IPS | Работает | 60/90/120/144 Гц (авто 60↔120 Гц), подсветка `4-0011` |
| Тачскрин и автоповорот | Работает | Мультитач, жесты в терминале, `sensorfwd` + кнопка блокировки |
| GPU Adreno 650 (EGL/GLES3) | Работает | Аппаратная отрисовка `naomi-shell`, браузера и видео |
| CPU SM8250 и охлаждение | Работает | `schedutil`, C-states, devfreq, 4 ГБ ZRAM (`zstd`), ~39–40 °C в покое |
| 4 динамика (Aqstic WCD938x) | Работает | PulseAudio 48 kHz без ресемплинга |
| Wi-Fi (`wlp1s0`) и Bluetooth | Работает | Клавиатуры, мыши, наушники, геймпады |
| Задняя 13 МП + Фронтальная 8 МП | Работает | `naomi-camera` (`libhybris` `libcamera.so.1`) + вспышка/фонарик + видео |
| Батарея и лимит заряда 80% | Работает | Аппаратный `night_charging` в Настройках → Батарея |
| Dual-boot Android `_a` ↔ Linux `_b` | Работает | Без вайпа Android, переключение в 1 тап |
| Waydroid (Android 13) | Работает | С автостопом после 10 мин простоя |

---

## Быстрый старт (установка с нуля)

Подробная пошаговая инструкция со всеми командами, бэкапом слота `_a`, настройкой `devroot`, сборкой, установкой камеры, Telegram-бота и откатом находится здесь:

👉 **[Пошаговая инструкция по установке на Xiaomi Pad 6 (`docs/PIPA-INSTALL.md`)](docs/PIPA-INSTALL.md)**

Краткий обзор этапов:
1. **Разблокируйте загрузчик** Xiaomi Pad 6 и обязательно снимите резервную копию разделов слота `_a` (`boot_a`, `dtbo_a`, `vendor_boot_a`, `vbmeta_a`, `vbmeta_system_a`).
2. **Установите базу Ubuntu Touch 26.04 (Halium 13)** из порта [`thespider2/ubuntu-touch-xiaomi-pipa`](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa) на слот `_b` (не трогая `_a` и `super`).
3. **Настройте SSH-доступ** к планшету (`ssh phablet@<IP_ПЛАНШЕТА>`) и соберите среду сборки (`sudo sh device/devroot.sh ...`).
4. **Соберите и установите PopkaPhone + Камеру** с компьютера:
   ```sh
   python3 tools/pipa.py check --ssh <IP_ПЛАНШЕТА>
   python3 tools/pipa.py build --ssh <IP_ПЛАНШЕТА> --jobs 4
   ```
   Затем на планшете:
   ```sh
   ln -sfn /home/phablet/naomi/build-pipa-<COMMIT> /home/phablet/naomi/build
   sudo sh /home/phablet/naomi/src/pipa-<COMMIT>/device/install-shell.sh install
   sudo systemctl mask lightdm.service
   sudo systemctl enable --now naomid.service naomi-shell.service
   ```
5. **(Опционально) Установите Telegram-бот** (`bash extras/tg-bot/install.sh`), **ПК-агент** (`extras/pc-agent/install_windows_task.ps1`) и **Windows Mirror** (`mirror/build-win.ps1`).

---

## Структура репозитория

| Папка | Назначение |
|---|---|
| `shell/` | Оболочка `naomi-shell`, драйвер `naomi_hwc`, служба `naomid`, браузер `naomi-web`, терминал `naomi-term`, **камера `naomi-camera`**, игры `naomi-game` |
| `device/` | Скрипты для планшета: `shell-prepare` (тюнинг SM8250 + ZRAM), `install-shell.sh`, `devroot.sh`, `naomi-camera`, `naomi-telegram`, `naomi-deepseek`, `naomi-pipa-slot-switch` |
| `extras/tg-bot/` | Telegram-бот центр управления (20 русских команд, пульт планшета/ПК/Samsung TV/Android TV, DLNA-медиасервер, `/term`, `/cam`, `/clip`, `/scenes`) |
| `extras/pc-agent/` | Лёгкий локальный HTTP-агент для Windows ПК (без сторонних зависимостей, только LAN + токен) |
| `mirror/` | Клиент `NaomiPhone Mirror` для Windows 11 / macOS (Electron) |
| `tablet/boot-switch/` | Android-приложение и плитка быстрых настроек для перезагрузки из Android в Ubuntu Touch |
| `tools/` | Инструменты сборки и тестирования (`pipa.py`, `mirror_win.py`, `tools/tests/` — 90+ автотестов) |
| `docs/` | Документация: [PIPA-INSTALL.md](docs/PIPA-INSTALL.md), [PIPA.md](docs/PIPA.md), [WINDOWS-MIRROR.md](docs/WINDOWS-MIRROR.md) и технические заметки |

---

## Лицензия и благодарности

- Распространяется под лицензией **MIT** ([LICENSE](LICENSE), сторонние компоненты — [THIRD-PARTY.md](THIRD-PARTY.md)).
- Основано на оригинальном проекте **[PopaSpinka/PopkaPhone](https://github.com/PopaSpinka/PopkaPhone)**.
- Базовый порт Ubuntu Touch Halium 13 для Xiaomi Pad 6 — **[thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa)**.
- **Отказ от ответственности**: прошивка и модификация загрузчика выполняются на ваш страх и риск. Всегда делайте резервную копию разделов слота `_a` перед началом работы.
