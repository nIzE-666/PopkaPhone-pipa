# Roadmap — PopkaPhone for Xiaomi Pad 6 (`pipa`)

This roadmap outlines planned milestones for evolving `PopkaPhone-pipa` into a reusable, well-tested desktop Linux shell and hardware-enablement toolkit for `SM8250` / Halium 13 tablets.

## Completed in v0.2.0-pipa
- **System audit & hardening:** clean shutdown sync hook for dual-boot (`naomi-pipa-sync`), `WebSurface` redraw fix, external IME fix, `.desktop` `TryExec`/executable validation & icon paths, `code_pin` `ENOMEM` safety, 4 GB ZRAM, unified `zz-90-naomi.conf` journald limit, and audio jack udev permissions.
- **Travel security & DNS-leak-free VPN:** Home/Away/Auto firewall (`device/pipa-mode`, `pipa-firewall.service`, `95-pipa-mode`, `zz-pipa-security.conf`), sing-box TUN VPN with DoH + NTP bypass (`device/pipa-vpn`), and Telegram Control Hub `/mode` panel.

## Near-Term (v0.2.x)
- **Camera UX & media pipeline (`shell/apps/camera/`):**
  - Mux PulseAudio microphone input into MP4 H.264 video recordings (`ffmpeg` audio+video pipeline).
  - Add an optional one-tap `CAMERA` tile in the `N` → `02 APP` launcher alongside the `03 LINUX` menu entry.
  - Add gallery thumbnail strip and EXIF orientation metadata on JPEG capture.
- **Display & power polish:**
  - Validate full `30 / 48 / 50 / 60 / 90 / 120 / 144 Hz` present-fence cadence across sleep/wake cycles.
  - Improve idle power draw measurement and publish reproducible thermal/battery benchmark scripts for `pipa`.
- **CI & static analysis:**
  - Add automated C++ header/syntax checks and QML lint (`qmllint`) in GitHub Actions where Qt 6 packages are available on `ubuntu-latest`.

## Mid-Term (v0.3.x)
- **External display over USB-C DP-Alt (`card0-DP-1`):**
  - Move beyond connection detection (`Laptop Auto-Mode`) to dual-display / extended desktop output over USB-C HDMI/DisplayPort adapters.
- **Packaging & easier onboarding:**
  - Automated `.deb` or tarball artifacts for `arm64` Ubuntu Touch 26.04 so users can install or update without waiting for a full on-device `devroot` compile.
  - Automated English/Russian UI localization toggle inside `InterfacePage.qml` so international users can run the entire desktop UI in English without rebuilding.
- **Windows Mirror & companion polish:**
  - Prebuilt unsigned Windows 11 Mirror zip artifacts on GitHub Releases alongside checksums.
  - Optional mDNS / zero-conf discovery for LAN pairing between tablet and Windows Mirror / PC Agent.

## Long-Term (v0.4+)
- **Multi-tablet `SM8250` / Halium 13 portability:**
  - Abstract `shell/device_profile.cpp`, backlight paths, HWC2 quirks, and `device/shell-prepare` into declarative device profiles so community contributors can port other Snapdragon 870 / Halium 13 tablets (e.g., Lenovo Xiaoxin Pad Pro, Xiaomi Pad 5 / `nabu`) cleanly without duplicating shell code.
- **Upstream collaboration:**
  - Share reusable Halium 13 `libcamera.so.1` offscreen EGL patterns, HWC2 multi-refresh enumeration, and Qt 6 Wayland tablet ergonomics back with the broader Ubuntu Touch / UBports and mobile Linux communities.
