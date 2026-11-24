# Third-Party Components & Attribution

Original code in this repository is licensed under the **[MIT License](LICENSE)** (`Copyright (c) 2026 Popka` and `Copyright (c) 2026 nIzE-666`).

No proprietary firmware images, ROMs, vendor binaries, game files, fonts, or personal credentials are distributed in this repository. External OS images, libraries, and optional tools are fetched directly from their respective upstream sources during installation and remain under their own licenses.

> Note: This document is an attribution reference, not legal advice. Verify upstream licenses directly before redistributing derived binaries.

## Upstream Foundation

| Project | Source | License | Role |
|---|---|---|---|
| **PopkaPhone** | [PopaSpinka/PopkaPhone](https://github.com/PopaSpinka/PopkaPhone) | MIT | Original Qt 6 / Wayland phone & desktop shell, `naomid`, HWC2 driver, and streaming protocol (originally built for Google Pixel 5 / `redfin`) |

## Bundled Third-Party Headers in This Repository

| File(s) | Source | License |
|---|---|---|
| `shell/apps/game/third_party/libretro.h` | [libretro/Mesen](https://github.com/libretro/Mesen) | MIT (header text included in file) |
| `shell/apps/web/third_party/QtWebEngineCore/*.h` (3 API headers) | [Qt WebEngine 6.10.2](https://github.com/qt/qtwebengine/tree/v6.10.2/src/core/api) | LGPL-3.0 / GPL-2.0 / GPL-3.0 (SPDX identifiers in each file) |

Motion and geometry reference notes in `design/mockup/` reference AOSP (Apache 2.0) and Material 3 specifications (Apache 2.0 / CC BY 4.0); see [docs/ui-research/licenses.md](docs/ui-research/licenses.md).

## Xiaomi Pad 6 (`pipa`) Base OS, Kernel & Runtime Dependencies (Fetched Separately)

| Component | Source | License | Usage |
|---|---|---|---|
| Ubuntu Touch 26.04 (`pipa` port) | [thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa), UBports | Open-source OS components (GPL/LGPL/MIT/etc.) + device vendor blobs from stock firmware | Base Halium 13 rootfs and boot images for slot `_b` |
| Linux Kernel 4.19 (`pipa` / `droidian`) | [aymanrgab/linux-android-xiaomi-pipa](https://github.com/aymanrgab/linux-android-xiaomi-pipa) | GPL-2.0 | SM8250 kernel & V4L2 Venus driver |
| `libhybris` (`libhwc2`, `libcamera.so.1`, EGL) | [libhybris/libhybris](https://github.com/libhybris/libhybris) / UBports packages | Apache 2.0 | Bridge between Android HALs (HWC2, Camera, Gralloc) and glibc Linux |
| Qt 6.10.2 (`qt6-base`, `qt6-declarative`, `qt6-wayland`, `qt6-webengine`) | Ubuntu / UBports repositories | LGPL-3.0 / GPL-3.0 | Shell UI, Wayland compositor, camera UI, and browser engine |
| `ffmpeg` | Ubuntu repositories | LGPL-2.1+ / GPL-2.0+ | H.264 MP4 video encoding in `naomi-camera` |
| `freerdp-wayland` (`wlfreerdp`) | [FreeRDP/FreeRDP](https://github.com/FreeRDP/FreeRDP) | Apache 2.0 | Optional native Wayland RDP client |
| Electron & `@electron/packager` | npm (`mirror/package.json`) | MIT / BSD-2-Clause | Windows 11 / macOS Mirror desktop client |
| `aiogram`, `aiohttp`, `aiohttp-socks`, `websockets` | PyPI (`extras/tg-bot/requirements.txt`) | MIT / Apache 2.0 / BSD-3-Clause | Optional self-hosted Telegram Control Hub |

## Upstream / Optional Components (Fetched On Demand)

| Component | Source | License | Installer |
|---|---|---|---|
| Ubuntu Touch 24.04 for Pixel 5 (`redfin`) | [dadrickpistachio/google-redfin](https://gitlab.com/dadrickpistachio/google-redfin) | Open-source OS components | `tools/images` (upstream Pixel 5 flow) |
| Google Sans / Material Symbols Rounded | [google/fonts](https://github.com/google/fonts), [google/material-design-icons](https://github.com/google/material-design-icons) | SIL OFL 1.1 / Apache 2.0 | `tools/naomi fonts` |
| FrequencyWords (keyboard dictionaries) | [hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords) | CC BY-SA 4.0 (data) / MIT (code) | `tools/naomi dict` |
| EasyList & EasyPrivacy | [easylist.to](https://easylist.to) | GPL-3.0 / CC BY-SA 3.0 | `tools/naomi web-filters` |
| Waydroid & LineageOS images | [waydro.id](https://waydro.id) | GPL-3.0 / Apache 2.0 | `device/android-setup.sh` |
| Libretro cores (`Mesen`, `PCSX-ReARMed`) | [libretro/Mesen](https://github.com/libretro/Mesen), [libretro/pcsx_rearmed](https://github.com/libretro/pcsx_rearmed) | GPL-3.0 / GPL-2.0 | `device/cores.sh` |
| Wine, Box64, DXVK, Mesa Turnip | Kron4ek, ptitSeb, doitsujin, Mesa | LGPL-2.1 / MIT / zlib | `device/wine.sh` |
| Open-Meteo weather data | [open-meteo.com](https://open-meteo.com) | CC BY 4.0 | Optional shell weather layer |

## Trademarks

Xiaomi, Pad, Google, Android, Qualcomm, Snapdragon, Adreno, Ubuntu, Canonical, Microsoft, Windows, Apple, macOS, Raspberry Pi, Samsung, and Telegram are trademarks of their respective owners. This is an independent community open-source project and is not affiliated with or endorsed by any of these companies.
