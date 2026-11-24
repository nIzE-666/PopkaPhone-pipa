import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.tests.test_pipa import SH
from tools.tests.test_tablet_keyboard import check_balanced_delimiters

ROOT = Path(__file__).resolve().parents[2]
CPP = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")

GOOGLE_AUTH_CPP_TEST = r"""
#include "google_auth.h"
#include <cassert>
#include <string>

int main() {
    using namespace naomi::web;

    // User-Agent cleaning
    const std::string raw =
        "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "QtWebEngine/6.10.2 Chrome/134.0.6998.208 Safari/537.36";
    const std::string clean = cleanChromeUserAgent(raw);
    assert(clean == "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/134.0.6998.208 Safari/537.36");
    assert(clean.find("QtWebEngine") == std::string::npos);
    assert(clean.find("aarch64") == std::string::npos);

    // Auth host matching
    assert(isGoogleAuthHost("accounts.google.com"));
    assert(isGoogleAuthHost("ACCOUNTS.GOOGLE.COM."));
    assert(isGoogleAuthHost("sub.accounts.google.com"));
    assert(isGoogleAuthHost("accounts.youtube.com"));
    assert(isGoogleAuthHost("gds.google.com"));
    assert(!isGoogleAuthHost("evilaccounts.google.com"));
    assert(!isGoogleAuthHost("accounts.google.com.evil.com"));
    assert(!isGoogleAuthHost("www.google.com"));
    assert(!isGoogleAuthHost("example.com"));

    // Adblock bypass on Google auth pages/resources
    assert(shouldBypassBlockForGoogleAuth("csi.gstatic.com", "accounts.google.com"));
    assert(shouldBypassBlockForGoogleAuth("google-analytics.com", "accounts.google.com"));
    assert(shouldBypassBlockForGoogleAuth("accounts.google.com", "github.com"));
    assert(shouldBypassBlockForGoogleAuth("csi.gstatic.com", "myaccount.google.com"));
    assert(!shouldBypassBlockForGoogleAuth("google-analytics.com", "example.com"));
    assert(!shouldBypassBlockForGoogleAuth("doubleclick.net", "www.youtube.com"));

    // Header override scoping
    assert(shouldOverrideAuthHeaders("accounts.google.com", "accounts.google.com"));
    assert(shouldOverrideAuthHeaders("accounts.google.com", "github.com"));
    assert(shouldOverrideAuthHeaders("ssl.gstatic.com", "accounts.google.com"));
    assert(shouldOverrideAuthHeaders("lh3.googleusercontent.com", "accounts.google.com"));
    assert(!shouldOverrideAuthHeaders("ssl.gstatic.com", "www.youtube.com"));
    assert(!shouldOverrideAuthHeaders("www.youtube.com", "www.youtube.com"));
    assert(!shouldOverrideAuthHeaders("www.google.com", "www.google.com"));

    // Auth User-Agent sanity
    const std::string authUa(kGoogleAuthUserAgent);
    assert(authUa.find("Firefox/") != std::string::npos);
    assert(authUa.find("QtWebEngine") == std::string::npos);
    assert(kClientHintHeaders.size() >= 3);
    return 0;
}
"""


class DeepseekDesktopTest(unittest.TestCase):
    def test_modified_qml_files_are_balanced(self):
        qml_files = [
            ROOT / "shell/app/qml/desktop/Glyph.qml",
            ROOT / "shell/app/qml/desktop/NMenu.qml",
            ROOT / "shell/app/qml/desktop/Desktop.qml",
            ROOT / "shell/app/qml/desktop/game/GameWindow.qml",
            ROOT / "shell/apps/web/qml/Main.qml",
            ROOT / "shell/apps/web/qml/App.qml",
        ]
        for path in qml_files:
            with self.subTest(path=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertTrue(check_balanced_delimiters(text), f"Unbalanced delimiters in {path}")

    def test_glyph_and_nmenu_deepseek_tile_fit_cleanly(self):
        glyph = (ROOT / "shell/app/qml/desktop/Glyph.qml").read_text(encoding="utf-8")
        self.assertGreaterEqual(glyph.count('case "deepseek":'), 2)

        nmenu = (ROOT / "shell/app/qml/desktop/NMenu.qml").read_text(encoding="utf-8")
        self.assertIn("property real deepseekOpen: 0", nmenu)
        self.assertIn("signal deepseekClicked()", nmenu)
        self.assertIn('{ label: "DEEPSEEK", glyph: "deepseek", deepseek: true }', nmenu)
        self.assertIn("menu.deepseekClicked()", nmenu)

        labels = re.findall(r'label:\s*"([A-Z]+)"', nmenu)
        self.assertEqual(labels, ["GAME", "WEB", "FILES", "ANDROID", "TERMINAL", "CLAUDE", "TELEGRAM", "DEEPSEEK"])

        # Verify 2 rows x 4 columns geometry fits cleanly inside 360dp menu in both 1440x900 and 900x1440 dp
        per_row = int(re.search(r"readonly property int perRow:\s*(\d+)", nmenu).group(1))
        self.assertEqual(per_row, 4)
        self.assertEqual(len(labels) % per_row, 0)
        sys_w = 360
        inner_w = sys_w - 2 * 16
        tile_s = (inner_w - (per_row - 1) * 6) // per_row
        self.assertGreaterEqual(tile_s, 72)
        menu_h = (2 * 14 + 44 + 8 + 44) + 6 + (14 + 18 + 12 + 2 * (tile_s + 6) + 6 + 14)
        for desk_w, desk_h in ((1440, 900), (900, 1440)):
            self.assertLess(sys_w + 12, desk_w)
            self.assertLess(menu_h, desk_h - 60 - 12)

    def test_desktop_wiring_and_install_scripts(self):
        desktop = (ROOT / "shell/app/qml/desktop/Desktop.qml").read_text(encoding="utf-8")
        self.assertIn("/naomi/desktop/deepseek.conf", desktop)
        self.assertIn('category: "deepseek"', desktop)
        self.assertIn('naomiApps.run("deepseek", "/usr/local/lib/naomi/naomi-deepseek", naomiDesk.scale, [], false)', desktop)
        self.assertIn('if (a === "deepseek")\n            return "DeepSeek"', desktop.replace("\r\n", "\n"))
        self.assertIn('[deepseekPid, "deepseek"]', desktop)
        self.assertIn('name: "DEEPSEEK"', desktop)
        self.assertIn("naomiInput: app === \"deepseek\"", desktop)
        self.assertEqual(desktop.count("naomiApps.pause(desk.deepseekPid, w.minimized)"), 1)
        self.assertIn("desk.deepseekPanel", desktop)
        self.assertIn("pkg.startsWith(\"deepseek:\")", desktop)

        install_sh = (ROOT / "device/install-shell.sh").read_text(encoding="utf-8")
        self.assertIn('install -m 755 "$SRC/naomi-deepseek" "$LIB/naomi-deepseek"', install_sh)

        naomi_tool = (ROOT / "tools/naomi").read_text(encoding="utf-8")
        self.assertIn('"naomi-deepseek"', naomi_tool)

    def test_linux_app_window_enables_touch_to_pointer(self):
        game_window = (ROOT / "shell/app/qml/desktop/game/GameWindow.qml").read_text(encoding="utf-8")
        self.assertIn('touchToPointer: win.system === "app"', game_window)
        self.assertIn('pointerFrames: win.system === "app"', game_window)
        self.assertIn("onPointerPressed: win.activate()", game_window)

        web_surface_h = (ROOT / "shell/app/web_surface.h").read_text(encoding="utf-8")
        self.assertIn("void touchEvent(QTouchEvent *event) override;", web_surface_h)
        self.assertIn("void mousePressEvent(QMouseEvent *event) override;", web_surface_h)
        self.assertIn("void mouseReleaseEvent(QMouseEvent *event) override;", web_surface_h)

        web_surface_cpp = (ROOT / "shell/app/web_surface.cpp").read_text(encoding="utf-8")
        self.assertIn("WL_POINTER_FRAME_SINCE_VERSION", web_surface_cpp)


@unittest.skipUnless(SH, "requires sh")
class DeepseekWrapperTest(unittest.TestCase):
    def _run_wrapper(self, systemctl_script: str, journalctl_script: str, extra_env=None):
        script_bytes = (ROOT / "device/naomi-deepseek").read_bytes()
        self.assertNotIn(b"\r\n", script_bytes, "device/naomi-deepseek must use LF line endings")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()

            args_file = root / "web_args.txt"
            calls_file = root / "calls.txt"

            for name, body in (
                ("systemctl", systemctl_script),
                ("journalctl", journalctl_script),
                (
                    "mock-naomi-web",
                    f"#!/bin/sh\nprintf '%s\\n' \"$@\" > {args_file.as_posix()}\n",
                ),
            ):
                p = bin_dir / name
                p.write_bytes(body.encode("utf-8"))
                p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

            env = os.environ.copy()
            env["PATH"] = f"{bin_dir.as_posix()}:{env.get('PATH', '')}"
            env["NAOMI_WEB_BIN"] = (bin_dir / "mock-naomi-web").as_posix()
            env["NAOMI_DSH_POLL_INTERVAL"] = "0.01"
            env["NAOMI_DSH_POLL_MAX"] = "5"
            env["CALLS_FILE"] = calls_file.as_posix()
            env["XDG_RUNTIME_DIR"] = "/run/user/32011"
            env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/run/user/32011/bus"
            if extra_env:
                env.update(extra_env)

            res = subprocess.run(
                [SH, (ROOT / "device/naomi-deepseek").as_posix()],
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            args = args_file.read_text(encoding="utf-8").splitlines() if args_file.exists() else []
            calls = calls_file.read_text(encoding="utf-8").splitlines() if calls_file.exists() else []
            return res, args, calls

    def test_starts_service_uses_current_invocation_and_hides_token(self):
        secret = "tok_live_9876543210"
        systemctl = (
            "#!/bin/sh\n"
            'printf "systemctl %s\\n" "$*" >> "$CALLS_FILE"\n'
            'if [ "$2" = "show" ]; then printf "inv-current-42\\n"; fi\n'
        )
        journalctl = (
            "#!/bin/sh\n"
            'printf "journalctl %s\\n" "$*" >> "$CALLS_FILE"\n'
            'case "$*" in\n'
            f'  *_SYSTEMD_INVOCATION_ID=inv-current-42*) printf "dsh web: http://127.0.0.1:3080/?token={secret}\\n" ;;\n'
            '  *) printf "dsh web: http://127.0.0.1:3080/?token=stale_dead_token\\n" ;;\n'
            "esac\n"
        )
        res, args, calls = self._run_wrapper(systemctl, journalctl)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertNotIn(secret, res.stdout)
        self.assertNotIn(secret, res.stderr)
        self.assertEqual(args, ["--app", f"http://127.0.0.1:3080/?token={secret}", "--name", "DeepSeek"])
        self.assertTrue(any("systemctl --user start dsh-web.service" in c for c in calls))
        self.assertTrue(any("_SYSTEMD_INVOCATION_ID=inv-current-42" in c for c in calls))

    def test_polls_until_url_published(self):
        secret = "tok_delayed_1122"
        with tempfile.TemporaryDirectory() as tmp:
            counter = Path(tmp) / "count"
            systemctl = (
                "#!/bin/sh\n"
                'if [ "$2" = "show" ]; then printf "inv-poll-7\\n"; fi\n'
            )
            journalctl = (
                "#!/bin/sh\n"
                f'n=0; [ -f "{counter.as_posix()}" ] && n=$(cat "{counter.as_posix()}")\n'
                'n=$((n + 1))\n'
                f'printf "%s" "$n" > "{counter.as_posix()}"\n'
                'if [ "$n" -ge 3 ]; then\n'
                f'  printf "dsh web: http://127.0.0.1:3080/?token={secret}\\n"\n'
                "fi\n"
            )
            res, args, _ = self._run_wrapper(systemctl, journalctl)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(args, ["--app", f"http://127.0.0.1:3080/?token={secret}", "--name", "DeepSeek"])

    def test_fails_cleanly_on_timeout(self):
        systemctl = '#!/bin/sh\nif [ "$2" = "show" ]; then printf "inv-empty\\n"; fi\n'
        journalctl = "#!/bin/sh\nexit 0\n"
        res, args, _ = self._run_wrapper(systemctl, journalctl, extra_env={"NAOMI_DSH_POLL_MAX": "2"})
        self.assertNotEqual(res.returncode, 0)
        self.assertEqual(args, [])
        self.assertIn("session URL", res.stderr)


class WebGoogleAuthTest(unittest.TestCase):
    @unittest.skipUnless(CPP, "requires a C++ compiler")
    def test_cpp_google_auth_helpers(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            src = tmp_path / "test_google_auth.cpp"
            exe = tmp_path / "test_google_auth.exe"
            src.write_text(GOOGLE_AUTH_CPP_TEST, encoding="utf-8")
            compile_res = subprocess.run(
                [CPP, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{ROOT / 'shell/apps/web'}", str(src), "-o", str(exe)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(compile_res.returncode, 0, compile_res.stderr)
            run_res = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
            self.assertEqual(run_res.returncode, 0, run_res.stderr)

    def test_web_qml_and_filter_google_auth_wiring(self):
        main_qml = (ROOT / "shell/apps/web/qml/Main.qml").read_text(encoding="utf-8")
        self.assertIn("function cleanUserAgent(", main_qml)
        self.assertIn("function applyCleanProfileIdentity(", main_qml)
        self.assertIn("p.clientHints", main_qml)
        self.assertIn("/google-auth.js", main_qml)
        self.assertIn("Linux x86_64", main_qml)

        app_qml = (ROOT / "shell/apps/web/qml/App.qml").read_text(encoding="utf-8")
        self.assertIn("QtWebEngine\\/", app_qml)
        self.assertIn("profile.clientHints", app_qml)
        self.assertIn("settings.javascriptCanAccessClipboard: true", app_qml)

        web_filter = (ROOT / "shell/apps/web/web_filter.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "google_auth.h"', web_filter)
        self.assertIn("shouldOverrideAuthHeaders", web_filter)
        self.assertIn("shouldBypassBlockForGoogleAuth", web_filter)
        self.assertIn('info.setHttpHeader("User-Agent"', web_filter)

        auth_js = (ROOT / "shell/apps/web/scripts/google-auth.js").read_text(encoding="utf-8")
        self.assertIn("accounts\\.google\\.com", auth_js)
        self.assertIn("Firefox/", auth_js)
        self.assertIn("userAgentData", auth_js)


if __name__ == "__main__":
    unittest.main()
