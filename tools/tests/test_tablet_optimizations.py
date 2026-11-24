import unittest
from pathlib import Path

from tools.tests.test_tablet_keyboard import check_balanced_delimiters

ROOT = Path(__file__).resolve().parents[2]


class TabletOptimizationsTest(unittest.TestCase):
    def test_shell_prepare_has_pipa_post_boot_and_zram(self):
        script = (ROOT / "device/shell-prepare").read_text(encoding="utf-8")
        self.assertNotIn("\r\n", script)
        self.assertIn("pipa_post_boot()", script)
        self.assertIn("lpm_levels/parameters/sleep_disabled N", script)
        self.assertIn("/proc/sys/kernel/sched_boost 0", script)
        self.assertIn("scaling_governor\" schedutil", script)
        self.assertIn("input_boost_freq \"0:1344000\"", script)
        self.assertIn("/dev/zram0", script)
        self.assertIn("zstd", script)
        self.assertIn("read_ahead_kb", script)

    def test_naomid_fixes_audio_destructor_and_wireless_autodetect(self):
        audio_h = (ROOT / "shell/naomid/audio.h").read_text(encoding="utf-8")
        audio_cpp = (ROOT / "shell/naomid/audio.cpp").read_text(encoding="utf-8")
        self.assertIn("~Audio() override;", audio_h)
        self.assertIn("Audio::~Audio()", audio_cpp)
        self.assertIn("m_events.disconnect(this);", audio_cpp)

        charge_cpp = (ROOT / "shell/naomid/charge.cpp").read_text(encoding="utf-8")
        self.assertIn("findWirelessDevice()", charge_cpp)
        self.assertNotIn('const QString dev = QStringLiteral("wlan0");', charge_cpp)

    def test_hwc_idle_hysteresis_and_present_timer_filter(self):
        hwc_cpp = (ROOT / "shell/display/naomi_hwc.cpp").read_text(encoding="utf-8")
        self.assertIn("++m_busyStreak >= 2", hwc_cpp)
        pt_cpp = (ROOT / "shell/display/present_timer.cpp").read_text(encoding="utf-8")
        self.assertIn("s.fps <= 2 && m_drops == m_loggedDrops", pt_cpp)

    def test_waydroid_idle_auto_stop_and_clipboard_screenshot(self):
        aa_cpp = (ROOT / "shell/app/android_apps.cpp").read_text(encoding="utf-8")
        self.assertIn("m_idleStopTimer", aa_cpp)
        self.assertIn("NAOMI_ANDROID_IDLE_STOP_SEC", aa_cpp)

        shot_cpp = (ROOT / "shell/app/desk_shot.cpp").read_text(encoding="utf-8")
        self.assertIn("QGuiApplication::clipboard()", shot_cpp)
        self.assertIn("cb->setImage(full)", shot_cpp)

    def test_terminal_wayland_touch_scroll_extra_keys_and_zoom(self):
        term_main = (ROOT / "shell/term/main.cpp").read_text(encoding="utf-8")
        self.assertIn('setenv("WAYLAND_DISPLAY", "wayland-naomi", 1);', term_main)

        tv_cpp = (ROOT / "shell/app/term_view.cpp").read_text(encoding="utf-8")
        self.assertIn("setAcceptTouchEvents(true);", tv_cpp)
        self.assertIn("void TermView::touchEvent(", tv_cpp)
        self.assertIn('shortcut(QStringLiteral("zoom")', tv_cpp)

        tw_qml = (ROOT / "shell/app/qml/desktop/term/TermWindow.qml").read_text(encoding="utf-8")
        self.assertTrue(check_balanced_delimiters(tw_qml))
        self.assertIn("extraKeys", tw_qml)
        self.assertIn("snapHalf(", tw_qml)
        self.assertIn("setZoom(", tw_qml)

    def test_split_screen_linux_launcher_settings_and_qml_balance(self):
        files = [
            "shell/app/qml/desktop/BarItem.qml",
            "shell/app/qml/desktop/ClipboardPopover.qml",
            "shell/app/qml/desktop/Glyph.qml",
            "shell/app/qml/desktop/WinButton.qml",
            "shell/app/qml/desktop/DeskBar.qml",
            "shell/app/qml/desktop/DeskWindow.qml",
            "shell/app/qml/desktop/Desktop.qml",
            "shell/app/qml/desktop/NMenu.qml",
            "shell/app/qml/desktop/game/GameWindow.qml",
            "shell/app/qml/desktop/web/WebWindow.qml",
            "shell/app/qml/desktop/files/FilesWindow.qml",
            "shell/app/qml/desktop/settings/NavScale.qml",
            "shell/app/qml/desktop/settings/InterfacePage.qml",
            "shell/app/qml/desktop/settings/SystemPage.qml",
            "shell/app/qml/desktop/settings/TabletScreenPage.qml",
            "shell/app/qml/desktop/settings/SettingsWindow.qml",
            "shell/app/qml/desktop/BatteryPopover.qml",
            "shell/apps/web/qml/Main.qml",
            "shell/apps/web/qml/Page.qml",
        ]
        for rel in files:
            with self.subTest(file=rel):
                txt = (ROOT / rel).read_text(encoding="utf-8")
                self.assertTrue(check_balanced_delimiters(txt), f"Unbalanced delimiters in {rel}")

        nav = (ROOT / "shell/app/qml/desktop/settings/NavScale.qml").read_text(encoding="utf-8")
        self.assertNotIn('"battery"', nav.split("tabletItems", 1)[1].split("return tabletItems", 1)[0])

        nmenu = (ROOT / "shell/app/qml/desktop/NMenu.qml").read_text(encoding="utf-8")
        self.assertIn("linuxModule", nmenu)
        self.assertIn("filteredLinuxApps", nmenu)

        deskbar = (ROOT / "shell/app/qml/desktop/DeskBar.qml").read_text(encoding="utf-8")
        self.assertIn("signal screenshotClicked()", deskbar)
        self.assertIn('kind: "camera"', deskbar)
        self.assertIn("signal clipClicked()", deskbar)
        self.assertIn("onLongPressed:", deskbar)
        self.assertIn("screen_lock_rotation", deskbar)

        files_win = (ROOT / "shell/app/qml/desktop/files/FilesWindow.qml").read_text(encoding="utf-8")
        self.assertIn("openTerminalAt", files_win)

    def test_wave5_pip_clipboard_sync_and_laptop_mode(self):
        st_h = (ROOT / "shell/app/system_tools.h").read_text(encoding="utf-8")
        st_cpp = (ROOT / "shell/app/system_tools.cpp").read_text(encoding="utf-8")
        for sym in ("clipboardHistory", "pullPcClipboard", "pushPcClipboard", "hardwareKeyboard", "externalDisplay"):
            self.assertIn(sym, st_h)
            self.assertIn(sym, st_cpp)

        desk = (ROOT / "shell/app/qml/desktop/Desktop.qml").read_text(encoding="utf-8")
        self.assertIn("property int pinZ:", desk)
        self.assertIn("w.pinned ? ++pinZ : ++topZ", desk)
        self.assertIn("ClipboardPopover {", desk)

        for rel in (
            "shell/app/qml/desktop/DeskWindow.qml",
            "shell/app/qml/desktop/web/WebWindow.qml",
            "shell/app/qml/desktop/term/TermWindow.qml",
            "shell/app/qml/desktop/game/GameWindow.qml",
        ):
            txt = (ROOT / rel).read_text(encoding="utf-8")
            self.assertIn("property bool pinned: false", txt, rel)
            self.assertIn('glyph: "pin"', txt, rel)

        tw = (ROOT / "shell/app/qml/desktop/term/TermWindow.qml").read_text(encoding="utf-8")
        self.assertIn("naomiSystemTools.hardwareKeyboard", tw)
        self.assertIn("extraKeysShown", tw)

    def test_wave6_clock24h_and_pro_settings_pages(self):
        bar = (ROOT / "shell/app/qml/desktop/DeskBar.qml").read_text(encoding="utf-8")
        self.assertIn("property bool clock24h: true", bar)
        self.assertIn("property bool clockSeconds: false", bar)
        self.assertIn("property int dateFormat: 1", bar)

        desk = (ROOT / "shell/app/qml/desktop/Desktop.qml").read_text(encoding="utf-8")
        self.assertIn("id: uiSettings", desk)
        self.assertIn("property bool clock24h: true", desk)
        self.assertIn("uiSettings.nightFilter", desk)

        nav = (ROOT / "shell/app/qml/desktop/settings/NavScale.qml").read_text(encoding="utf-8")
        self.assertIn('"interface"', nav)
        self.assertIn('"system"', nav)
        # Mouse should no longer be excluded on tablet
        self.assertNotIn('"mouse"', nav.split("tabletItems =", 1)[1].split("return tabletItems", 1)[0])

        st_h = (ROOT / "shell/app/system_tools.h").read_text(encoding="utf-8")
        st_cpp = (ROOT / "shell/app/system_tools.cpp").read_text(encoding="utf-8")
        for sym in ("powerProfile", "setPowerProfile", "services", "toggleService", "cleanJournal", "setTimezone", "syncTime"):
            self.assertIn(sym, st_h)
            self.assertIn(sym, st_cpp)


if __name__ == "__main__":
    unittest.main()
