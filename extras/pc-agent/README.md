# pipa-pc-agent — Локальный агент управления Windows ПК

Лёгкий HTTP-агент на чистом Python 3 (без внешних pip-зависимостей) для управления Windows ПК (`mypc`, `192.168.1.100`) с планшета через `pipa-tg-bot`.

## Безопасность

- Слушает `0.0.0.0:18765`, но отклоняет любые подключения не из локальной сети / RFC1918 (`403 Forbidden`).
- Требует заголовок `X-Pipa-Token` (сравнение за постоянное время `hmac.compare_digest`) из `%USERPROFILE%\.pipa-pc-agent\token.txt`.
- Скрипт установки добавляет правило Windows Firewall строго для подсети `192.168.1.0/24`.

## Установка на Windows (`mypc`)

Откройте PowerShell (желательно от имени администратора, чтобы сразу создать правило брандмауэра):

```powershell
powershell -ExecutionPolicy Bypass -File .\install_windows_task.ps1
```

Скрипт выведет строку `PC_AGENT_TOKEN=...` — добавьте её в `~/.config/pipa-bot/bot.env` на планшете Xiaomi Pad 6.

## Эндпоинты

- `GET /health` — проверка доступности
- `GET /status` — CPU %, RAM, аптайм, батарея (если есть), активное окно
- `GET /screenshot` — скриншот экрана в PNG (через .NET `System.Windows.Forms` / `System.Drawing`)
- `POST /action` — `lock`, `sleep`, `hibernate`, `reboot`, `shutdown`, `vol_up`, `vol_down`, `vol_mute`, `media_play_pause`, `media_next`, `media_prev`, `open_url`, `run_cmd`
