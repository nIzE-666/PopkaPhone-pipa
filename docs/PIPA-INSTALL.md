# Пошаговая установка PopkaPhone Linux на Xiaomi Pad 6 (`pipa`)

[English](PIPA-INSTALL.en.md) · **Русский**

Эта инструкция описывает полный путь установки **Ubuntu Touch 26.04 LTS (Halium 13) + оптимизированного PopkaPhone** на планшет **Xiaomi Pad 6 (`pipa`, SM8250)** второй системой на слоте `_b` с сохранением Android на слоте `_a`.

> ⚠️ **Важно перед началом**
> - Все действия с `fastboot` и слотами выполняйте внимательно.
> - **Никогда не прошивайте слот `_a` и раздел `super`**, если хотите сохранить ваш текущий Android.
> - Убедитесь, что загрузчик (Bootloader) разблокирован, а батарея заряжена минимум на 50%.

---

## Что понадобится

- Планшет **Xiaomi Pad 6 (`pipa`)** с разблокированным загрузчиком.
- ПК (Windows / Linux / macOS) с установленными `adb`, `fastboot`, `git`, `python3`, `ssh`.
- USB-кабель и Wi-Fi в одной локальной сети.
- Релиз Ubuntu Touch Halium 13 для `pipa` (`ubuntu-touch-pipa-recovery-flashable.zip`, `boot.img`, `dtbo.img`) из репозитория [thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa).

---

## Шаг 1. Обязательный бэкап Android (слот `_a`)

Перед прошивкой слота `_b` сохраните на ПК образы вашего текущего слота `_a`:
- `boot_a.img`
- `dtbo_a.img`
- `vendor_boot_a.img`
- `vbmeta_a.img`
- `vbmeta_system_a.img`

Если на Android есть root (`su`) или кастомное рекавери (TWRP/OrangeFox), снимите их через `dd if=/dev/block/by-name/<раздел> of=...` и проверьте контрольные суммы SHA-256. Это гарантирует 100% откат к исходному состоянию.

---

## Шаг 2. Установка базы Ubuntu Touch 26.04 (Halium 13) на слот `_b`

1. Убедитесь, что активный слот сейчас `_a` (`fastboot getvar current-slot`).
2. Разверните образ Ubuntu Touch (`ubuntu.img` / ext4-контейнер `userdata.img`) согласно схеме в [docs/PIPA.md](PIPA.md) и инструкции порта [thespider2/ubuntu-touch-xiaomi-pipa](https://github.com/thespider2/ubuntu-touch-xiaomi-pipa).
3. Прошейте **только** `boot_b` и `dtbo_b`, затем переключите активный слот на `b`:
   ```sh
   fastboot flash boot_b boot.img
   fastboot flash dtbo_b dtbo.img
   fastboot set_active b
   fastboot reboot
   ```
4. После первой загрузки Ubuntu Touch подключитесь к Wi-Fi, включите режим разработчика и настройте SSH-доступ для пользователя `phablet` (рекомендуется вход по SSH-ключу).

Проверьте, что планшет определяется безопасно (подтверждённый `pipa`, Ubuntu Touch 26.04 arm64, слот `_b`):
```sh
python3 tools/pipa.py check --ssh <IP_ПЛАНШЕТА>
```

---

## Шаг 3. Подготовка среды сборки (`devroot`) на планшете

PopkaPhone собирается нативно прямо на процессоре Snapdragon 870 планшета (сборка занимает ~2–3 минуты в 4 потока).

Подключитесь по SSH к планшету и разверните `devroot` (однократно):
```sh
ssh phablet@<IP_ПЛАНШЕТА>
# Передайте исходники текущего коммита:
```
С ПК:
```sh
python3 tools/pipa.py stage --ssh <IP_ПЛАНШЕТА>
```
На планшете:
```sh
SRC=$(ls -dt ~/naomi/src/pipa-* | head -n 1)
sudo sh "$SRC/device/devroot.sh" create
sudo sh "$SRC/device/devroot.sh" packages
```

---

## Шаг 4. Сборка и установка оболочки PopkaPhone + Камеры

С ПК запустите сборку всех компонентов (`naomi-shell`, `naomid`, `naomi-web`, `naomi-term`, `naomi-game`, `naomi-camera`):
```sh
python3 tools/pipa.py build --ssh <IP_ПЛАНШЕТА> --jobs 4
```

После сообщения `Сборка: /home/phablet/naomi/build-pipa-<HASH>` выполните на планшете:
```sh
REV=<HASH>
ln -sfn "/home/phablet/naomi/build-pipa-${REV}" /home/phablet/naomi/build
sudo sh "/home/phablet/naomi/src/pipa-${REV}/device/install-shell.sh" install

# Отключаем Lomiri (lightdm) и включаем прямой запуск PopkaPhone
sudo systemctl stop lightdm.service || true
sudo systemctl mask lightdm.service
sudo systemctl enable --now naomid.service naomi-shell.service
```

> **Примечание про rootfs `ro`:** На Ubuntu Touch корневой раздел `/` смонтирован только на чтение. Скрипт `install-shell.sh` сам аккуратно перемонтирует `/` в `rw` на время копирования и возвращает в `ro`. При ручной установке пакетов через `apt-get` используйте:
> ```sh
> sudo mount -o remount,rw /
> sudo apt-get -o Acquire::ForceIPv4=true update
> sudo apt-get -o Acquire::ForceIPv4=true install -y tmux btop fzf ripgrep fd-find bat wl-clipboard jq neovim mpv adb ffmpeg freerdp-wayland python3-venv python3-pip
> sync && sudo mount -o remount,ro /
> ```

---

## Шаг 5. Активация обеих камер (`naomi-camera`)

На Halium 13 доступ к Android Camera HAL выдаётся через штатную службу Ubuntu Touch `cameraservice-trust-stored.service`. Чтобы она запускалась без Lomiri автоматически при старте:

```sh
mkdir -p ~/.config/systemd/user/default.target.wants ~/Pictures/Camera ~/Videos ~/.local/share/applications
ln -sf /usr/lib/systemd/user/cameraservice-trust-stored.service \
       ~/.config/systemd/user/default.target.wants/cameraservice-trust-stored.service
systemctl --user daemon-reload
systemctl --user start cameraservice-trust-stored.service
```

Добавьте ярлык в раздел **`03 LINUX`** меню `N`:
```sh
cat > ~/.local/share/applications/naomi-camera.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Camera
Name[ru]=Камера (13 МП / 8 МП)
Comment=Нативная камера Xiaomi Pad 6 (фото, видео, зум, автофокус, фонарик)
Exec=/usr/local/lib/naomi/naomi-camera
Icon=camera-photo
Terminal=false
Categories=AudioVideo;Graphics;
EOF
```

Быстрый тест из консоли (задняя и фронтальная камеры):
```sh
/usr/local/lib/naomi/naomi-camera --snap back --out /tmp/back.jpg
/usr/local/lib/naomi/naomi-camera --snap front --out /tmp/front.jpg
```

---

## Шаг 6. Установка Telegram-бота центра управления (`extras/tg-bot`)

1. Создайте нового бота у **@BotFather** (`/newbot`) и получите токен.
2. Скопируйте папку `extras/tg-bot/` на планшет и запустите установщик:
   ```sh
   scp -r extras/tg-bot phablet@<IP_ПЛАНШЕТА>:~/pipa-tg-bot-src
   ssh phablet@<IP_ПЛАНШЕТА> "bash ~/pipa-tg-bot-src/install.sh"
   ```
3. Откройте `~/.config/pipa-bot/bot.env` (права `chmod 600`) и заполните:
   ```ini
   BOT_TOKEN=<ВАШ_ТОКЕН_ОТ_BOTFATHER>
   OWNER_ID=
   PC_AGENT_TOKEN=<ТОКЕН_ПК_АГЕНТА_ЕСЛИ_НУЖНО>
   # Опционально, если Telegram API требует локальный SOCKS5-прокси:
   # TELEGRAM_PROXY=socks5://127.0.0.1:1080
   ```
4. Отредактируйте `~/.config/pipa-bot/config.json`, указав IP/MAC ваших устройств в локальной сети (ПК, Samsung TV, Android TV, роутер).
5. Запустите службу и получите 6-значный код привязки владельца:
   ```sh
   systemctl --user enable --now pipa-tg-bot.service
   cat ~/.config/pipa-bot/pair_code.txt
   ```
6. Отправьте вашему боту в Telegram `/pair <КОД>`, затем `/start`. После привязки бот отвечает **только вашему Telegram ID**.

---

## Шаг 7. Подключение Windows ПК (`extras/pc-agent`)

Для управления Windows ПК из Telegram-бота и обмена буфером обмена `ПК ↔ Планшет`:
1. На Windows-ПК запустите скрипт установки задачи автозапуска:
   ```powershell
   powershell -ExecutionPolicy Bypass -File extras\pc-agent\install_windows_task.ps1
   ```
2. Агент запустится на порту `18765` (принимает запросы **только из приватных подсетей RFC1918** и требует заголовок `X-Pipa-Token`).
3. Сгенерированный токен лежит в `%USERPROFILE%\.pipa-pc-agent\token.txt` — перенесите его в `PC_AGENT_TOKEN` в `~/.config/pipa-bot/bot.env` на планшете.
4. При необходимости откройте порт в Брандмауэре Windows для вашей локальной подсети (из PowerShell от администратора):
   ```powershell
   New-NetFirewallRule -DisplayName "Pipa PC Agent LAN" -Direction Inbound -Protocol TCP -LocalPort 18765 -RemoteAddress LocalSubnet -Action Allow
   ```

---

## Шаг 8. Переключение обратно в Android и откат

- **Из PopkaPhone в Android**: меню `N` → **«Перезагрузить в One UI»** (выполняет безопасную смену слота `_b` → `_a` через Boot Control 1.1 с проверкой перед перезагрузкой).
- **Из Android в Ubuntu Touch**: соберите и установите `tablet/boot-switch/` (`PopkaPhone-Boot-Switch.apk`, требуется root `su` в Android) — появится значок и плитка быстрых настроек.
- **Аварийный возврат через ПК (fastboot)**:
  ```sh
  fastboot set_active a
  fastboot reboot
  ```
- **Удаление PopkaPhone с возвратом Lomiri**:
  ```sh
  sudo sh /home/phablet/naomi/src/pipa-<HASH>/device/install-shell.sh uninstall
  sudo systemctl unmask lightdm.service
  sudo systemctl reboot
  ```
