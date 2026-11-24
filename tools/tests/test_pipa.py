import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.pipa import GUARD

PIPA_V020_OS_RELEASE = """\
PRETTY_NAME="Ubuntu 26.04 LTS"
VERSION_ID="26.04"
VERSION_CODENAME=resolute
ID=ubuntu
VARIANT=Touch
VARIANT_ID=touch
"""
PIPA_CHANNEL = "[service]\nchannel: utnext/arm64/android9plus/daily\ndevice: pipa\n"


def find_sh():
    shell = shutil.which("sh")
    if shell is None and sys.platform == "win32":
        git = shutil.which("git")
        if git:
            git_shell = Path(git).parents[1] / "bin" / "sh.exe"
            if git_shell.is_file():
                shell = str(git_shell)
    return shell


SH = find_sh()


@unittest.skipUnless(SH, "requires sh (Git for Windows provides it)")
class PipaGuardTest(unittest.TestCase):
    def run_guard(self, os_release, *, release_path="usr/lib/os-release", lomiri=False,
                  device="pipa", slot="_b", arch="aarch64", files=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for path, contents in {release_path: os_release, **(files or {})}.items():
                fixture = root / path
                fixture.parent.mkdir(parents=True, exist_ok=True)
                fixture.write_bytes(contents.encode("ascii"))
            guard = GUARD
            for path in (
                "etc/os-release", "usr/lib/os-release", "etc/deviceinfo", "etc/system-image/channel.ini",
                "proc/device-tree/model",
                "sys/firmware/devicetree/base/model", "proc/cmdline", "proc/bootconfig",
            ):
                guard = guard.replace(f"/{path}", shlex.quote((root / path).as_posix()))
            stubs = rf"""
getprop() {{
    case "$1" in
        ro.product.device) printf '%s\n' {shlex.quote(device)} ;;
        ro.boot.slot_suffix) printf '%s\n' {shlex.quote(slot)} ;;
    esac
}}
uname() {{ printf '%s\n' {shlex.quote(arch)}; }}
"""
            if lomiri:
                stubs += "lomiri() { :; }\n"
            return subprocess.run([SH, "-s"], input=stubs + guard, capture_output=True,
                                  encoding="utf-8", timeout=10, check=False)

    def assert_release(self, result, release):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Ubuntu Touch {release} arm64", result.stdout)

    def test_resolute_image_v020(self):
        self.assert_release(self.run_guard(PIPA_V020_OS_RELEASE), "26.04")

    def test_deviceinfo_directory_uses_device_tree_or_channel(self):
        for path, contents in (
            ("proc/device-tree/model", "Qualcomm Technologies, Inc. xiaomi pipa\0"),
            ("etc/system-image/channel.ini", PIPA_CHANNEL),
        ):
            with self.subTest(path=path):
                result = self.run_guard(PIPA_V020_OS_RELEASE, device="", slot="", files={
                    "etc/deviceinfo/default.yaml": "Name: generic\n",
                    "proc/cmdline": "androidboot.slot_suffix=_b\n",
                    path: contents,
                })
                self.assert_release(result, "26.04")
                self.assertEqual(result.stderr, "")

    def test_pipa_channel_cannot_override_redfin_property(self):
        result = self.run_guard(PIPA_V020_OS_RELEASE, device="redfin",
                                files={"etc/system-image/channel.ini": PIPA_CHANNEL})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Обнаружен redfin", result.stderr)

    def test_resolute_accepts_either_touch_marker(self):
        for marker in ('VARIANT_ID="touch"\n', "VARIANT='Touch'\n"):
            with self.subTest(marker=marker):
                os_release = "ID=ubuntu\nVERSION_ID=26.04\nVERSION_CODENAME=resolute\n" + marker
                self.assert_release(self.run_guard(os_release, release_path="etc/os-release"), "26.04")

    def test_noble_legacy_lomiri(self):
        os_release = 'ID=ubuntu\nVERSION_ID="24.04"\nVERSION_CODENAME=noble\n'
        self.assert_release(self.run_guard(os_release, release_path="etc/os-release", lomiri=True), "24.04")

    def test_resolute_desktop_with_lomiri_is_rejected(self):
        os_release = PIPA_V020_OS_RELEASE.replace("VARIANT=Touch\nVARIANT_ID=touch", "VARIANT_ID=desktop")
        result = self.run_guard(os_release, lomiri=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Ubuntu Touch 24.04 или 26.04 arm64", result.stderr)

    def test_device_slot_and_arch_guards(self):
        for overrides, message in (
            ({"device": "pipa", "slot": "_a"}, "только подтверждённый активный слот _b"),
            ({"device": "redfin"}, "Устройство pipa не подтверждено"),
            ({"arch": "x86_64"}, "Ubuntu Touch 24.04 или 26.04 arm64"),
        ):
            with self.subTest(overrides=overrides):
                result = self.run_guard(PIPA_V020_OS_RELEASE, **overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)


@unittest.skipUnless(SH, "requires sh (Git for Windows provides it)")
class PipaBootSwitchTest(unittest.TestCase):
    def run_boot(self, direction="one-ui", **overrides):
        script = Path(__file__).resolve().parents[2] / "device" / "naomi-pipa-slot-switch"
        to_ubuntu = direction.endswith("ubuntu-touch")
        current, target = ("0", "1") if to_ubuntu else ("1", "0")
        without_bootctl = overrides.pop("WITHOUT_BOOTCTL", False)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "commands"
            active = root / "active"
            active.write_text(overrides.pop("ACTIVE", current) + "\n", encoding="ascii")
            mocks = {
                "id": "#!/bin/sh\n[ \"$*\" = -u ] && printf '%s\\n' \"$MOCK_UID\"\n",
                "timeout": """#!/bin/sh
[ "$1 $2 $3" = '-k 1s 3s' ] || exit 94
shift 3
exec "$@"
""",
                "lxc-attach": """#!/bin/sh
[ "$1 $2 $3 $4 $5" = '-n android --clear-env -- /system/bin/bootctl' ] || exit 90
shift 5
export MOCK_INSIDE_LXC=1
exec ./bootctl "$@"
""",
                "getprop": "#!/bin/sh\n[ \"$*\" != ro.product.vendor.device ] || echo \"$MOCK_DEVICE\"\n",
                "bootctl": """#!/bin/sh
[ "${MOCK_INSIDE_LXC:-0}" = "$MOCK_LXC" ] || exit 95
printf 'bootctl %s\\n' "$*" >> "$MOCK_LOG"
case "$*" in
    get-number-slots) echo "$MOCK_SLOTS" ;;
    get-current-slot) echo "$MOCK_CURRENT" ;;
    "get-suffix $MOCK_TARGET") echo "$MOCK_SUFFIX" ;;
    "is-slot-bootable $MOCK_TARGET") exit "$MOCK_BOOTABLE_RESULT" ;;
    get-snapshot-merge-status) echo "$MOCK_MERGE" ;;
    get-active-boot-slot)
        [ "$MOCK_HAL" = 1.2 ] || exit 70
        read -r slot < "$MOCK_ACTIVE" && echo "$slot"
        ;;
    hal-info) echo "HAL Version: android.hardware.boot@$MOCK_HAL::IBootControl" ;;
    "set-active-boot-slot $MOCK_TARGET")
        [ "$MOCK_SET_RESULT" = 0 ] || exit "$MOCK_SET_RESULT"
        printf '%s\\n' "$MOCK_READBACK" > "$MOCK_ACTIVE"
        ;;
    *) exit 91 ;;
esac
""",
                "systemctl": """#!/bin/sh
[ "$*" = '--no-block reboot' ] || exit 92
[ "$SYSTEMCTL_SKIP_AUTO_KEXEC $SYSTEMCTL_SKIP_AUTO_SOFT_REBOOT" = '1 1' ] || exit 93
[ "$MOCK_LXC" = 1 ] || exit 96
printf 'reboot\\n' >> "$MOCK_LOG"
exit "$MOCK_REBOOT_RESULT"
""",
                "reboot": """#!/bin/sh
[ "$#" = 0 ] && [ "$MOCK_LXC" = 0 ] || exit 97
printf 'reboot\\n' >> "$MOCK_LOG"
exit "$MOCK_REBOOT_RESULT"
""",
            }
            for name, source in mocks.items():
                if name == "bootctl" and without_bootctl:
                    continue
                helper = root / name
                helper.write_bytes(source.encode("ascii"))
                helper.chmod(0o755)
            env = {
                **os.environ,
                "MOCK_LOG": log.name,
                "MOCK_ACTIVE": active.name,
                "MOCK_UID": "0",
                "MOCK_LXC": "0" if to_ubuntu else "1",
                "MOCK_HAL": "1.2",
                "MOCK_DEVICE": "pipa",
                "MOCK_SLOTS": "2",
                "MOCK_CURRENT": current,
                "MOCK_TARGET": target,
                "MOCK_SUFFIX": "_b" if to_ubuntu else "_a",
                "MOCK_BOOTABLE_RESULT": "0",
                "MOCK_MERGE": "none",
                "MOCK_SET_RESULT": "0",
                "MOCK_READBACK": target,
                "MOCK_REBOOT_RESULT": "0",
                **{f"MOCK_{name}": value for name, value in overrides.items()},
            }
            result = subprocess.run([SH, "-s", "--", direction], input="export PATH=.\n" + script.read_text(encoding="utf-8"),
                                    cwd=root, capture_output=True, encoding="utf-8", env=env, timeout=10, check=False)
            commands = log.read_text(encoding="ascii").splitlines() if log.is_file() else []
            return result, commands

    def test_reboots_only_after_selecting_and_reading_back_slot_a(self):
        result, commands = self.run_boot()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands, [
            "bootctl get-number-slots", "bootctl get-current-slot", "bootctl get-suffix 0",
            "bootctl is-slot-bootable 0", "bootctl get-snapshot-merge-status", "bootctl get-active-boot-slot",
            "bootctl set-active-boot-slot 0", "bootctl get-active-boot-slot", "reboot",
        ])

    def test_preflight_failures_never_change_the_slot_or_reboot(self):
        for invalid in (
            {"UID": "32011"}, {"SLOTS": "1"}, {"CURRENT": "0"}, {"SUFFIX": "_b"},
            {"BOOTABLE_RESULT": "1"}, {"MERGE": "merging"}, {"MERGE": "snapshotted"},
            {"MERGE": "unknown"}, {"ACTIVE": "4294967295"},
        ):
            with self.subTest(invalid=invalid):
                result, commands = self.run_boot(**invalid)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertNotIn("bootctl set-active-boot-slot 0", commands)
                self.assertNotIn("reboot", commands)

    def test_failed_selection_or_readback_never_reboots(self):
        for failure in ({"SET_RESULT": "1"}, {"READBACK": "1"}, {"READBACK": "4294967295"}):
            with self.subTest(failure=failure):
                result, commands = self.run_boot(**failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("_a", result.stderr)
                self.assertIn("bootctl set-active-boot-slot 0", commands)
                self.assertNotIn("reboot", commands)

    def test_reboot_failure_reports_pending_slot_and_retry_skips_slot_write(self):
        result, commands = self.run_boot(REBOOT_RESULT="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Слот _a с One UI уже выбран", result.stderr)
        self.assertEqual(commands[-1], "reboot")

        result, commands = self.run_boot(ACTIVE="0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("bootctl set-active-boot-slot 0", commands)
        self.assertEqual(commands[-1], "reboot")

    def test_android_reboots_to_b_after_verifying_the_slot_and_reports_success(self):
        result, commands = self.run_boot("ubuntu-touch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "UBUNTU_TOUCH_REBOOT_REQUESTED")
        self.assertEqual(commands, [
            "bootctl get-number-slots", "bootctl get-current-slot", "bootctl get-suffix 1",
            "bootctl is-slot-bootable 1", "bootctl get-snapshot-merge-status", "bootctl get-active-boot-slot",
            "bootctl set-active-boot-slot 1", "bootctl get-active-boot-slot", "reboot",
        ])

    def test_android_declines_unsupported_device_or_unavailable_boot_control(self):
        for invalid in ({"DEVICE": "redfin"}, {"WITHOUT_BOOTCTL": True}, {"UID": "10245"}):
            with self.subTest(invalid=invalid):
                result, commands = self.run_boot("ubuntu-touch", **invalid)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertEqual(commands, [])

    def test_android_never_reboots_after_unsafe_selection_or_pending_update(self):
        for invalid in (
            {"CURRENT": "1"}, {"SUFFIX": "_a"}, {"BOOTABLE_RESULT": "1"},
            {"MERGE": "merging"}, {"SET_RESULT": "7"}, {"READBACK": "0"},
        ):
            with self.subTest(invalid=invalid):
                result, commands = self.run_boot("ubuntu-touch", **invalid)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("reboot", commands)

    def test_android_boot_control_1_1_requires_successful_slot_selection(self):
        result, commands = self.run_boot("ubuntu-touch", HAL="1.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands[-4:], [
            "bootctl get-active-boot-slot", "bootctl hal-info", "bootctl set-active-boot-slot 1", "reboot",
        ])

        for hal, set_result in (("1.1", "7"), ("1.0", "0")):
            result, commands = self.run_boot("ubuntu-touch", HAL=hal, SET_RESULT=set_result)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("reboot", commands)
        self.assertNotIn("bootctl set-active-boot-slot 1", commands)

        result, commands = self.run_boot(HAL="1.1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("bootctl set-active-boot-slot 0", commands)
        self.assertNotIn("reboot", commands)

    def test_android_check_mode_leaves_the_slot_unchanged(self):
        result, commands = self.run_boot("check-ubuntu-touch", HAL="1.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands, [
            "bootctl get-number-slots", "bootctl get-current-slot", "bootctl get-suffix 1",
            "bootctl is-slot-bootable 1", "bootctl get-snapshot-merge-status",
        ])

    def test_android_reboot_failure_keeps_b_confirmed_and_allows_retry_without_writing(self):
        result, commands = self.run_boot("ubuntu-touch", REBOOT_RESULT="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Слот _b с Ubuntu Touch уже выбран", result.stderr)
        self.assertEqual(commands[-1], "reboot")

        result, commands = self.run_boot("ubuntu-touch", ACTIVE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("bootctl set-active-boot-slot 1", commands)
        self.assertEqual(commands[-1], "reboot")
