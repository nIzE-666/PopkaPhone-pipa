# pipa-tg-bot — Центр управления Xiaomi Pad 6 в Telegram

Асинхронный русскоязычный Telegram-бот (`aiogram>=3.13`) для круглосуточного управления планшетом Xiaomi Pad 6 (`pipa`), домашними компьютерами (`pipa-pc-agent` + Wake-on-LAN), телевизорами (Samsung Smart TV и Android TV) и домашней сетью.

## Установка на планшет (`pipa`)

```bash
bash install.sh
```

1. Заполните `~/.config/pipa-bot/bot.env` (`BOT_TOKEN` от `@BotFather` и `PC_AGENT_TOKEN` от `pipa-pc-agent`).
2. Запустите службу:
   ```bash
   systemctl --user start pipa-tg-bot.service
   ```
3. Если `OWNER_ID` не указан, бот сгенерирует одноразовый 6-значный код в `~/.config/pipa-bot/pair_code.txt` и в `journalctl --user -u pipa-tg-bot`:
   ```bash
   cat ~/.config/pipa-bot/pair_code.txt
   ```
   Отправьте боту `/pair <КОД>` — ваш Telegram ID навсегда сохранится как единственный владелец.

## Основные команды

- `/start` — главное меню и панель управления
- `/status` — сводка по планшету, ПК и телевизорам
- `/tablet` — яркость, громкость, экран, перезагрузка, переход в One UI
- `/screen` — скриншот планшета
- `/vpn` — переключение VPN-профилей (`/usr/local/bin/vpn`)
- `/services` — управление службами (`Waydroid`, `DeepSeek Web`, `Desktop Commander`, `naomi-shell`, `naomid`)
- `/pc` — управление ПК (WoL, сон, блокировка, скриншот, громкость, медиа)
- `/tv` — пульт Samsung TV и Android TV (включая скриншот и ввод текста)
- `/net` — сканирование локальной сети и статус Wi-Fi
- `/run <cmd>` — выполнение команды в терминале планшета
- `/open [pc|tv] <url>` — открытие ссылки на планшете, ПК или Android TV
- `/get <path>` — скачать файл с планшета (любые отправленные боту файлы сохраняются в `~/Downloads/tg-bot/`)
- `/alerts` — настройка уведомлений о заряде, перегреве и запуске
- `/help` — подробная справка
