# Step-by-Step Installation & Recovery Guide — Xiaomi Pad 6 (`pipa`)

**English** · [Русский](PIPA-INSTALL.md)

This guide walks through installing **Ubuntu Touch 26.04 LTS (Halium 13) + PopkaPhone** on a **Xiaomi Pad 6 (`pipa`, Qualcomm Snapdragon 870 / `SM8250`)** in **A/B slot `_b`** while keeping your existing Android installation untouched on **slot `_a`**.

> **Important safety rules before starting**
> - Double-check every `fastboot` partition name and slot suffix.
> - **Never flash slot `_a` or `super`** if you want to preserve your Android system and user data.
> - Ensure the bootloader is unlocked and battery charge is at least 50%.

---

## Prerequisites

- **Xiaomi Pad 6 (`pipa`)** with an unlocked bootloader.
- **Host PC** (Windows, Linux, or macOS) with `adb`, `fastboot`, `git`, `python3`, and `ssh`.
- USB cable and Wi-Fi access on the same local network.
- Ubuntu Touch Halium 13 base release for `pipa` (`ubuntu-touch-pipa-recovery-flashable.zip`, `boot.img`, `dtbo.img`) from [thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa).

---

## Step 1. Back Up Android Slot `_a` (Mandatory)

Before modifying slot `_b`, save backups of your current slot `_a` images on your PC:
- `boot_a.img`
- `dtbo_a.img`
- `vendor_boot_a.img`
- `vbmeta_a.img`
- `vbmeta_system_a.img`

If your Android installation has root (`su`) or custom recovery (TWRP / OrangeFox), dump them via `dd if=/dev/block/by-name/<partition> of=...` and verify SHA-256 checksums. This guarantees a clean rollback path.

---

## Step 2. Install Ubuntu Touch 26.04 (Halium 13) Base on Slot `_b`

1. Confirm your active slot is `_a`: `fastboot getvar current-slot`.
2. Deploy the Ubuntu Touch ext4 container (`userdata.img` / `ubuntu.img`) following the storage layout in [docs/PIPA.md](PIPA.md) and [thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa).
3. Flash **only** `boot_b` and `dtbo_b`, then switch the active slot to `b`:
   ```sh
   fastboot flash boot_b boot.img
   fastboot flash dtbo_b dtbo.img
   fastboot set_active b
   fastboot reboot
   ```
4. After Ubuntu Touch boots into Lomiri, connect to Wi-Fi, enable Developer Mode, and configure SSH key authentication for `phablet`.

Verify from your PC that `tools/pipa.py` detects `pipa`, Ubuntu Touch 26.04 (`arm64`), and slot `_b` (it refuses to touch `redfin`, slot `_a`, or mismatched systems):

```sh
python3 tools/pipa.py check --ssh <TABLET_IP>
```

---

## Step 3. Set Up the On-Device Build Environment (`devroot`)

PopkaPhone compiles natively on the tablet's Snapdragon 870 inside an isolated `devroot` (~2–3 minutes at `--jobs 4`) so that headers and Qt 6.10.2 / `libhybris` ABIs match the running Ubuntu Touch image exactly.

From your PC, stage the repository sources:

```sh
python3 tools/pipa.py stage --ssh <TABLET_IP>
```

On the tablet (one-time setup):

```sh
SRC=$(ls -dt ~/naomi/src/pipa-* | head -n 1)
sudo sh "$SRC/device/devroot.sh" create
sudo sh "$SRC/device/devroot.sh" packages
```

---

## Step 4. Build and Install PopkaPhone & Native Camera

From your PC, compile all components (`naomi-shell`, `naomid`, `naomi-web`, `naomi-term`, `naomi-game`, `naomi-camera`):

```sh
python3 tools/pipa.py build --ssh <TABLET_IP> --jobs 4
```

When the build finishes (`Build: /home/phablet/naomi/build-pipa-<SHA>`), install and activate the systemd services on the tablet:

```sh
REV=<SHA>
ln -sfn "/home/phablet/naomi/build-pipa-${REV}" /home/phablet/naomi/build
sudo sh "/home/phablet/naomi/src/pipa-${REV}/device/install-shell.sh" install

# Replace Lomiri (lightdm) with PopkaPhone
sudo systemctl stop lightdm.service || true
sudo systemctl mask lightdm.service
sudo systemctl enable --now naomid.service naomi-shell.service
```

> **Note on read-only rootfs (`/`):** Ubuntu Touch mounts `/` read-only by default. `install-shell.sh` handles remounting `/` read-write during installation and restoring `ro` afterward. If you install extra CLI packages manually via `apt-get`, wrap them cleanly:
> ```sh
> sudo mount -o remount,rw /
> sudo apt-get -o Acquire::ForceIPv4=true update
> sudo apt-get -o Acquire::ForceIPv4=true install -y tmux btop fzf ripgrep fd-find bat wl-clipboard jq neovim mpv adb ffmpeg freerdp-wayland python3-venv python3-pip
> sync && sudo mount -o remount,ro /
> ```

---

## Step 5. Enable Dual Cameras (`naomi-camera`)

On Halium 13, Android Camera HAL access requires Ubuntu Touch's `cameraservice-trust-stored.service`. Enable it as a user service so it starts automatically without Lomiri:

```sh
mkdir -p ~/.config/systemd/user/default.target.wants ~/Pictures/Camera ~/Videos ~/.local/share/applications
ln -sf /usr/lib/systemd/user/cameraservice-trust-stored.service \
       ~/.config/systemd/user/default.target.wants/cameraservice-trust-stored.service
systemctl --user daemon-reload
systemctl --user start cameraservice-trust-stored.service
```

Create a launcher entry for the **`03 LINUX`** menu:

```sh
cat > ~/.local/share/applications/naomi-camera.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Camera (13 MP / 8 MP)
Name[ru]=Камера (13 МП / 8 МП)
Comment=Native Xiaomi Pad 6 camera (photo, video, zoom, autofocus, torch)
Exec=/usr/local/lib/naomi/naomi-camera
Icon=camera-photo
Terminal=false
Categories=AudioVideo;Graphics;
EOF
```

Verify both rear (13 MP) and front (8 MP) sensors from the shell:

```sh
/usr/local/lib/naomi/naomi-camera --snap back --out /tmp/back.jpg
/usr/local/lib/naomi/naomi-camera --snap front --out /tmp/front.jpg
```

---

## Step 6. Optional Companion Tools

- **Windows 11 Mirror Client (`mirror/`):** See [docs/WINDOWS-MIRROR.md](WINDOWS-MIRROR.md) for USB (`adb forward`) and Wi-Fi (`ssh -W`) setup.
- **Self-Hosted Telegram Control Hub (`extras/tg-bot/`):** Copy `extras/tg-bot/` to the tablet and run `bash install.sh`, populate `~/.config/pipa-bot/bot.env` (`chmod 600`) with your own `@BotFather` token, start `pipa-tg-bot.service`, and pair your Telegram account using the one-time 6-digit code in `~/.config/pipa-bot/pair_code.txt`.
- **Windows LAN PC Companion Agent (`extras/pc-agent/`):** Run `extras/pc-agent/install_windows_task.ps1` on Windows (binds on LAN port `18765`, rejects non-RFC1918 clients, requires `X-Pipa-Token`).

---

## Step 7. Dual-Boot Switching & Recovery / Rollback

- **Linux (`_b`) → Android (`_a`):** Open the **`N`** menu → **Reboot to Android / One UI** (runs `naomi-pipa-slot-switch` via Boot Control HAL 1.1/1.2 with pre-reboot verification).
- **Android (`_a`) → Linux (`_b`):** Build and install `tablet/boot-switch/` (`PopkaPhone-Boot-Switch.apk`, requires root `su` on Android) for an app icon and Quick Settings tile.
- **Emergency rollback to Android from a PC (Fastboot):**
  ```sh
  fastboot set_active a
  fastboot reboot
  ```
- **Uninstall PopkaPhone and restore stock Lomiri on slot `_b`:**
  ```sh
  sudo sh /home/phablet/naomi/src/pipa-<SHA>/device/install-shell.sh uninstall
  sudo systemctl unmask lightdm.service
  sudo systemctl reboot
  ```
