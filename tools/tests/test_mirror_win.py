from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import mirror_win


ADB = "mock-adb"
ELECTRON = ["mock-electron", "mock-mirror"]
DEVICES = "List of devices attached\npipa-test\tdevice\n"


def completed(stdout="", returncode=0):
    return subprocess.CompletedProcess([], returncode, stdout=stdout)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.run_process = self.enterContext(patch.object(mirror_win.subprocess, "run"))
        self.locate_adb = self.enterContext(patch.object(mirror_win, "adb_executable", return_value=ADB))
        self.saved_host = self.enterContext(patch.object(mirror_win, "saved_host", return_value=None))
        self.enterContext(patch.object(mirror_win, "launch_command", return_value=ELECTRON))
        self.stderr = self.enterContext(patch("sys.stderr"))

    def commands(self):
        return [call.args[0] for call in self.run_process.call_args_list]

    def test_usb_assigns_local_port_and_removes_only_its_forward_on_exit(self):
        self.run_process.side_effect = [completed(DEVICES), completed("49155\n"), completed(), completed()]
        self.assertEqual(mirror_win.main(["--via", "usb", "--port", "8123"]), 0)
        self.assertEqual(self.commands(), [
            [ADB, "devices"],
            [ADB, "-s", "pipa-test", "forward", "tcp:0", "tcp:8123"],
            ELECTRON,
            [ADB, "-s", "pipa-test", "forward", "--remove", "tcp:49155"],
        ])
        env = self.run_process.call_args_list[2].kwargs["env"]
        self.assertEqual(env["NAOMI_MIRROR_VIA"], "usb")
        self.assertEqual(env["NAOMI_MIRROR_USB_PORT"], "49155")
        self.assertEqual(env["NAOMI_MIRROR_PORT"], "8123")
        self.assertEqual(Path(env["NAOMI_MIRROR_ROOT"]), mirror_win.ROOT)
        self.assertNotIn("NAOMI_MIRROR_HOST", env)

    def test_auto_falls_back_when_only_wireless_adb_or_emulators_are_present(self):
        other = "List of devices attached\nemulator-5554\tdevice\n192.0.2.9:5555\tdevice\n"
        self.run_process.side_effect = [completed(other), completed()]
        self.assertEqual(mirror_win.main(["--host", "192.0.2.42"]), 0)
        self.assertEqual(self.commands(), [[ADB, "devices"], ELECTRON])
        env = self.run_process.call_args.kwargs["env"]
        self.assertEqual(env["NAOMI_MIRROR_HOST"], "192.0.2.42")
        self.assertEqual(env["NAOMI_MIRROR_VIA"], "wifi")
        self.assertNotIn("NAOMI_MIRROR_USB_PORT", env)

    def test_auto_provides_both_routes_when_usb_and_a_wifi_address_are_available(self):
        self.saved_host.return_value = "192.0.2.42"
        self.run_process.side_effect = [completed(DEVICES), completed("49155"), completed(), completed()]
        self.assertEqual(mirror_win.main([]), 0)
        env = self.run_process.call_args_list[2].kwargs["env"]
        self.assertEqual(env["NAOMI_MIRROR_VIA"], "auto")
        self.assertEqual(env["NAOMI_MIRROR_USB_PORT"], "49155")
        self.assertEqual(env["NAOMI_MIRROR_HOST"], "192.0.2.42")

    def test_wifi_uses_cached_address_without_invoking_adb(self):
        self.saved_host.return_value = "192.0.2.42"
        self.run_process.return_value = completed(returncode=7)
        self.assertEqual(mirror_win.main(["--via", "wifi"]), 7)
        self.locate_adb.assert_not_called()
        self.assertEqual(self.commands(), [ELECTRON])
        env = self.run_process.call_args.kwargs["env"]
        self.assertEqual(env["NAOMI_MIRROR_HOST"], "192.0.2.42")
        self.assertEqual(env["NAOMI_MIRROR_PORT"], "8478")

    def test_missing_adb_or_forward_failure_can_fall_back_to_wifi(self):
        self.locate_adb.side_effect = mirror_win.LauncherError("missing adb")
        self.run_process.return_value = completed()
        self.assertEqual(mirror_win.main(["--host", "192.0.2.42"]), 0)
        self.assertEqual(self.commands(), [ELECTRON])

        self.locate_adb.side_effect = None
        self.run_process.reset_mock()
        self.run_process.side_effect = [
            completed(DEVICES), subprocess.CalledProcessError(1, [], stderr="forward failed"), completed(),
        ]
        self.assertEqual(mirror_win.main(["--host", "192.0.2.42"]), 0)
        self.assertEqual(self.commands()[-1], ELECTRON)
        self.assertEqual(self.run_process.call_args.kwargs["env"]["NAOMI_MIRROR_VIA"], "wifi")

    def test_multiple_devices_require_serial_and_explicit_selection_is_honored(self):
        devices = DEVICES + "another\tdevice\n"
        self.run_process.return_value = completed(devices)
        self.assertEqual(mirror_win.main(["--via", "usb"]), 1)
        self.assertEqual(self.commands(), [[ADB, "devices"]])

        self.run_process.reset_mock()
        self.run_process.side_effect = [completed(devices), completed("53321"), completed(), completed()]
        self.assertEqual(mirror_win.main(["--via", "usb", "--adb", "another"]), 0)
        self.assertEqual(self.commands()[1], [ADB, "-s", "another", "forward", "tcp:0", "tcp:8478"])

    def test_forward_is_removed_on_interrupt_and_electron_startup_failure(self):
        for failure, exit_code in [(KeyboardInterrupt(), 130), (OSError("bad exe"), 1)]:
            with self.subTest(exit_code=exit_code):
                self.run_process.reset_mock()
                self.run_process.side_effect = [completed(DEVICES), completed("49155"), failure, completed()]
                self.assertEqual(mirror_win.main(["--via", "usb"]), exit_code)
                self.assertEqual(self.commands()[-1], [ADB, "-s", "pipa-test", "forward", "--remove", "tcp:49155"])

    def test_usb_with_unauthorized_serial_and_invalid_port_fail_before_launch(self):
        self.run_process.return_value = completed("List of devices attached\npipa-test\tunauthorized\n")
        self.assertEqual(mirror_win.main(["--via", "usb", "--adb", "pipa-test"]), 1)
        self.assertEqual(self.commands(), [[ADB, "devices"]])

        self.run_process.reset_mock()
        with self.assertRaises(SystemExit) as error:
            mirror_win.main(["--via", "wifi", "--port", "65536"])
        self.assertEqual(error.exception.code, 2)
        self.run_process.assert_not_called()


class ConfigurationTests(unittest.TestCase):
    def test_launcher_resolves_the_binary_from_npm_after_a_lazy_download(self):
        with tempfile.TemporaryDirectory(prefix="naomi mirror ") as temp:
            mirror = Path(temp)
            electron = mirror / "node_modules" / "electron"
            electron.mkdir(parents=True)
            (electron / "package.json").touch()
            executable = electron / "electron.exe"
            executable.touch()
            result = completed(f"Downloading Electron binary...\n{executable}\n")
            with patch.object(mirror_win, "MIRROR", mirror), patch.object(mirror_win.subprocess, "run", return_value=result) as run:
                self.assertEqual(mirror_win.launch_command(None), [str(executable), str(mirror)])
                self.assertEqual(run.call_args.args[0], ["node", "-p", "require('electron')"])

    def test_saved_wifi_address_accepts_a_windows_utf8_bom(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            private = root / "private"
            private.mkdir()
            (private / "device.json").write_text('{"wifi_ip":"192.0.2.42"}', encoding="utf-8-sig")
            with patch.object(mirror_win, "ROOT", root):
                self.assertEqual(mirror_win.saved_host(), "192.0.2.42")


if __name__ == "__main__":
    unittest.main()
