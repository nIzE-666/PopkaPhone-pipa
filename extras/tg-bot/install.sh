#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${HOME}/.local/share/pipa-bot/app"
VENV_DIR="${HOME}/.local/share/pipa-bot/venv"
CFG_DIR="${HOME}/.config/pipa-bot"
UNIT_DIR="${HOME}/.config/systemd/user"
DL_DIR="${HOME}/Downloads/tg-bot"

echo "==> Installing pipa-tg-bot to ${APP_DIR}"
mkdir -p "${APP_DIR}/drivers" "${CFG_DIR}" "${UNIT_DIR}" "${DL_DIR}"

cp "${SCRIPT_DIR}/__init__.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/bot.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/callbacks.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/commands.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/config.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/helpers.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/ui.py" "${APP_DIR}/"
cp "${SCRIPT_DIR}/requirements.txt" "${APP_DIR}/"
cp "${SCRIPT_DIR}/drivers/"*.py "${APP_DIR}/drivers/"

if [ ! -d "${VENV_DIR}" ]; then
    echo "==> Creating Python virtual environment at ${VENV_DIR}"
    python3 -m venv "${VENV_DIR}"
fi

echo "==> Installing Python dependencies into ${VENV_DIR}"
"${VENV_DIR}/bin/pip" install --upgrade pip
"${VENV_DIR}/bin/pip" install -r "${APP_DIR}/requirements.txt"

if [ ! -f "${CFG_DIR}/config.json" ]; then
    echo "==> Generating default ${CFG_DIR}/config.json"
    "${VENV_DIR}/bin/python" -c "
import sys
sys.path.insert(0, '${APP_DIR}')
from config import load_config
cfg = load_config()
cfg.save()
print('Saved default config to', cfg.config_path)
"
else
    echo "==> Keeping existing ${CFG_DIR}/config.json"
fi

if [ ! -f "${CFG_DIR}/bot.env" ]; then
    echo "==> Creating template ${CFG_DIR}/bot.env"
    cat > "${CFG_DIR}/bot.env" <<'EOF'
# Telegram Bot token from @BotFather (required)
BOT_TOKEN=

# Optional: leave empty to use zero-friction 6-digit /pair <CODE> on first launch
OWNER_ID=

# Shared X-Pipa-Token from Windows pipa-pc-agent (%USERPROFILE%\.pipa-pc-agent\token.txt)
PC_AGENT_TOKEN=
EOF
    chmod 600 "${CFG_DIR}/bot.env"
else
    echo "==> Keeping existing ${CFG_DIR}/bot.env"
fi

cp "${SCRIPT_DIR}/pipa-tg-bot.service" "${UNIT_DIR}/pipa-tg-bot.service"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"

systemctl --user daemon-reload
systemctl --user enable pipa-tg-bot.service

# Enable lingering so user service runs 24/7 even without an active login session
if command -v loginctl >/dev/null 2>&1; then
    sudo -n loginctl enable-linger "${USER}" >/dev/null 2>&1 || true
fi

if grep -qE '^BOT_TOKEN=.+' "${CFG_DIR}/bot.env"; then
    echo "==> Starting pipa-tg-bot.service"
    systemctl --user restart pipa-tg-bot.service
    systemctl --user status --no-pager pipa-tg-bot.service || true
else
    echo ""
    echo "==> Almost ready!"
    echo "1. Edit ${CFG_DIR}/bot.env and set BOT_TOKEN (and PC_AGENT_TOKEN)."
    echo "2. Start the bot:"
    echo "     systemctl --user start pipa-tg-bot.service"
    echo "3. View the 6-digit pairing code (if OWNER_ID is empty):"
    echo "     cat ${CFG_DIR}/pair_code.txt"
    echo "   Then send '/pair <CODE>' to your bot in Telegram."
fi
