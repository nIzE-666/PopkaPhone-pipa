import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.tests.test_pipa import SH

ROOT = Path(__file__).resolve().parents[2]
CPP = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")


class TabletRotationTest(unittest.TestCase):
    @unittest.skipUnless(CPP, "requires a C++ compiler")
    def test_orientation_validation_and_quick_toggle(self):
        source = r"""
#include "scene_rotation.h"
using namespace SceneRotation;
static_assert(isValid(0) && isValid(90) && isValid(180) && isValid(270));
static_assert(!isValid(-90) && !isValid(45) && !isValid(360));
static_assert(!isLandscape(0) && isLandscape(90) && !isLandscape(180) && isLandscape(270));
static_assert(toggle(0) == 90 && toggle(90) == 0);
static_assert(toggle(180) == 270 && toggle(270) == 180);
"""
        result = subprocess.run(
            [CPP, "-std=c++17", "-I", str(ROOT / "shell/app"), "-x", "c++", "-fsyntax-only", "-"],
            input=source, capture_output=True, text=True, timeout=20, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(SH, "requires sh")
class TabletBacklightTest(unittest.TestCase):
    def prepare(self, selected, brightness):
        script = (ROOT / "device/shell-prepare").read_text(encoding="utf-8")
        script = script.replace("/usr/local/lib/naomi/device.env", "device.env")
        script = script.replace("/usr/local/lib/naomi/naomid", "mock_naomid")
        script = script.replace("/sys/class/backlight", "backlight")
        script = script.replace("/proc/sys/vm", "vm")
        stubs = f"""
getprop() {{ :; }}
mock_naomid() {{ printf '%s\\n' 'backlight/{selected}/'; }}
runuser() {{ return 1; }}
chgrp() {{ printf '%s\\n' "$*"; }}
chmod() {{ printf '%s\\n' "$*"; }}
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "vm").mkdir()
            (root / "device.env").write_bytes(b"NAOMI_DEVICE=pipa\n")
            for node, maximum in (("4-0011", 4095), ("panel0-backlight", 255)):
                path = root / "backlight" / node
                path.mkdir(parents=True)
                (path / "brightness").write_bytes(f"{brightness}\n".encode())
                (path / "max_brightness").write_bytes(f"{maximum}\n".encode())
            result = subprocess.run(
                [SH, "-s"], cwd=root, input=stubs + script, capture_output=True,
                encoding="utf-8", timeout=10, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            if selected:
                self.assertEqual(result.stdout.splitlines(), [
                    f"video backlight/{selected}/brightness", f"g+w backlight/{selected}/brightness",
                ])
            return (root / "backlight/4-0011/brightness").read_text().strip()

    def test_zero_hardware_brightness_gets_restored(self):
        self.assertEqual(self.prepare("4-0011", 0), "2047")

    def test_nonzero_hardware_brightness_is_preserved(self):
        self.assertEqual(self.prepare("4-0011", 712), "712")

    def test_virtual_backlight_does_not_trigger_restore(self):
        self.assertEqual(self.prepare("panel0-backlight", 0), "0")
