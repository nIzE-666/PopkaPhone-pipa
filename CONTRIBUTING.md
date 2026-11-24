# Contributing to PopkaPhone-pipa

Contributions, bug reports, hardware measurements, and ports to additional Halium 13 / `SM8250` tablets are very welcome!

## Ground Rules

1. **Protect user data & device safety:**
   - Never commit real LAN IP addresses, MAC addresses, ADB serial numbers, Wi-Fi SSIDs, bot tokens, SSH keys, or personal paths. Use RFC 5737 / `192.168.1.x` examples and placeholder tokens (`<YOUR_TOKEN>`) in docs and tests.
   - Any script touching A/B partitions or `/data` must preserve slot `_a` and `super` and verify device identity (`pipa`) before writing.
2. **Preserve Linux line endings (LF):**
   - All scripts and systemd units under `device/`, `extras/tg-bot/`, and `*.sh` / `*.service` must use **LF (`\n`)**, never CRLF (`\r\n`). `.gitattributes` enforces this automatically; CI verifies it on every PR.
3. **Keep tests passing:**
   - Do not fabricate hardware test results. Clearly state in your PR what was verified in automated CI vs. on real Xiaomi Pad 6 hardware.

## Local Development & Running Tests

### Host-side automated tests (Linux, macOS, Windows)

```sh
# 1. Python & C++ harness unit tests (91 tests; C++ harnesses run when g++/clang++ is present)
python3 -m pip install -r extras/tg-bot/requirements.txt
python3 -m unittest discover -s tools/tests -v

# 2. Windows/macOS Electron Mirror protocol & keyboard tests (8 tests, Node.js 20+)
node --test mirror/tests/keyboard.test.js mirror/tests/stream.test.js
```

### Building natively on Xiaomi Pad 6 (`pipa`)

Full `naomi-shell`, `naomid`, `naomi-web`, `naomi-term`, `naomi-game`, and `naomi-camera` compilation runs inside the tablet's `arm64` Ubuntu Touch `devroot` so Qt 6.10.2 private headers and `libhybris` ABIs match the target OS:

```sh
python3 tools/pipa.py check --ssh <TABLET_IP>
python3 tools/pipa.py stage --ssh <TABLET_IP>
python3 tools/pipa.py build --ssh <TABLET_IP> --jobs 4
```

See [docs/PIPA-INSTALL.en.md](docs/PIPA-INSTALL.en.md) for initial `devroot` setup and rollback instructions.

## Submitting a Pull Request

1. Fork the repository and create a focused feature or fix branch (`git checkout -b fix/my-change`).
2. Add or update unit tests in `tools/tests/` or `mirror/tests/` when changing logic, parsers, CLI tooling, or protocol framing.
3. Run the test suite locally and verify no personal data or secrets appear in `git diff`.
4. Open a pull request describing:
   - What problem it solves or hardware behavior it improves.
   - Which automated tests passed and whether it was tested on physical `pipa` hardware.
   - Any breaking configuration or install steps.

## Reporting Issues

Use the structured templates in `.github/ISSUE_TEMPLATE/` for:
- **Bug reports** (include Ubuntu Touch build, `naomid --device`, `journalctl -u naomi-shell -u naomid` excerpts with private tokens/IPs redacted).
- **Feature requests**.
- **New device / tablet port proposals**.

## Licensing

By contributing to this repository, you agree that your contributions will be licensed under the project's **[MIT License](LICENSE)**.
