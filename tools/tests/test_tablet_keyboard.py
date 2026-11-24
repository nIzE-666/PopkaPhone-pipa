import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def strip_qml_comments_and_strings(source: str) -> str:
    out = []
    i = 0
    n = len(source)
    last_sig = ""
    while i < n:
        c = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            i += 2
            while i < n and source[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                i += 1
            i += 2
        elif c == "/" and last_sig in ("", "(", "[", "{", ",", "=", ":", ";", "!", "&", "|", "?", "\n"):
            i += 1
            in_class = False
            while i < n and (in_class or source[i] != "/"):
                if source[i] == "\\":
                    i += 2
                else:
                    if source[i] == "[":
                        in_class = True
                    elif source[i] == "]":
                        in_class = False
                    i += 1
            i += 1
            last_sig = "r"
        elif c in ('"', "'"):
            quote = c
            i += 1
            while i < n and source[i] != quote:
                if source[i] == "\\":
                    i += 2
                else:
                    i += 1
            i += 1
            last_sig = "s"
        else:
            out.append(c)
            if not c.isspace():
                last_sig = c
            i += 1
    return "".join(out)


def check_balanced_delimiters(source: str) -> bool:
    stripped = strip_qml_comments_and_strings(source)
    stack = []
    pairs = {")": "(", "]": "[", "}": "{"}
    for ch in stripped:
        if ch in "([{":
            stack.append(ch)
        elif ch in ")]}":
            if not stack or stack[-1] != pairs[ch]:
                return False
            stack.pop()
    return not stack


CPP_LOGIC_TEST = r"""
#include "keyboard_logic.h"
#include <cassert>

int main() {
    using namespace naomi::keyboard;

    // Layout normalization and toggling
    assert(normalizeLayout("en") == "EN");
    assert(normalizeLayout("EN") == "EN");
    assert(normalizeLayout("ru") == "RU");
    assert(normalizeLayout("RU") == "RU");
    assert(normalizeLayout("").empty());
    assert(normalizeLayout("DE").empty());
    assert(toggledLayout("EN") == "RU");
    assert(toggledLayout("RU") == "EN");
    assert(toggledLayout("ru") == "EN");
    assert(toggledLayout("") == "RU");

    // Automatic show requests vs tablet manual-only mode
    assert(shouldShowOnAutoRequest(false, true));
    assert(!shouldShowOnAutoRequest(false, false));
    assert(!shouldShowOnAutoRequest(true, true));
    assert(!shouldShowOnAutoRequest(true, false));

    // Automatic vs manual dismissal on focus changes
    assert(!shouldDismissOnFocusChange(false, false, true, false, true));
    assert(shouldDismissOnFocusChange(true, false, true, false, false));
    assert(!shouldDismissOnFocusChange(true, false, true, true, true));
    assert(!shouldDismissOnFocusChange(true, false, false, false, false));
    assert(shouldDismissOnFocusChange(true, false, false, false, true));
    // Manual keyboard stays open across non-IM focus (desktop, naomi-web, Waydroid) and focus loss
    assert(!shouldDismissOnFocusChange(true, true, true, false, true));
    assert(!shouldDismissOnFocusChange(true, true, false, false, true));

    // Automatic vs manual dismissal on ImEnabled updates
    assert(shouldDismissOnImUpdate(true, false, true, false));
    assert(!shouldDismissOnImUpdate(true, false, true, true));
    assert(!shouldDismissOnImUpdate(true, true, true, false));
    assert(!shouldDismissOnImUpdate(false, false, true, false));

    // Direct input vs fallback routing
    // QML TextInput / TermView -> direct
    assert(shouldUseDirectInput(true, true, true, false));
    // Wayland client (naomi-web, Waydroid, Linux app with text-input) on tablet -> fallback
    assert(!shouldUseDirectInput(true, false, true, true));
    assert(!shouldUseDirectInput(true, true, true, true));
    // Desktop non-IM item on tablet -> fallback
    assert(!shouldUseDirectInput(true, false, true, false));
    // No focus -> fallback
    assert(!shouldUseDirectInput(false, false, true, false));
    // Phone / client process without fallback preserves ImEnabled direct path even for Wayland items
    assert(shouldUseDirectInput(true, true, false, true));
    assert(!shouldUseDirectInput(true, false, false, false));
    return 0;
}
"""


class TabletKeyboardTest(unittest.TestCase):
    def test_qml_files_balanced(self):
        qml_files = [
            ROOT / "shell/app/qml/desktop/DeskBar.qml",
            ROOT / "shell/app/qml/desktop/Desktop.qml",
            ROOT / "shell/app/qml/desktop/term/TermWindow.qml",
            ROOT / "shell/ui/Naomi/Ui/Keyboard.qml",
            ROOT / "shell/ui/Naomi/Ui/KeyboardPanel.qml",
        ]
        for path in qml_files:
            with self.subTest(path=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertTrue(check_balanced_delimiters(text), f"Unbalanced delimiters in {path}")

    def test_desktop_loader_has_no_binding_loop(self):
        desktop = (ROOT / "shell/app/qml/desktop/Desktop.qml").read_text(encoding="utf-8")
        self.assertNotIn("keyboardUsed", desktop)
        self.assertIn("property bool keyboardBuilt: false", desktop)
        self.assertIn("active: DeskStyle.tablet && desk.keyboardBuilt", desktop)
        self.assertIn("onKeyboardClicked: desk.toggleKeyboard()", desktop)
        self.assertIn("naomiKeyboard.showManual()", desktop)

    def test_tablet_keyboard_is_manual_only(self):
        main_cpp = (ROOT / "shell/app/main.cpp").read_text(encoding="utf-8")
        self.assertIn("keyboard->setManualOnly(true);", main_cpp)

        ctx_h = (ROOT / "shell/input/keyboard_context.h").read_text(encoding="utf-8")
        self.assertIn("void setManualOnly(bool enabled)", ctx_h)
        self.assertIn("bool m_manualOnly = false;", ctx_h)

        ctx_cpp = (ROOT / "shell/input/keyboard_context.cpp").read_text(encoding="utf-8")
        self.assertIn("naomi::keyboard::shouldShowOnAutoRequest(m_manualOnly", ctx_cpp)

    def test_deskbar_language_and_keyboard_wiring(self):
        deskbar = (ROOT / "shell/app/qml/desktop/DeskBar.qml").read_text(encoding="utf-8")
        self.assertIn("signal keyboardClicked()", deskbar)
        self.assertIn("naomiKeyboard.layout", deskbar)
        self.assertIn("naomiKeyboard.setLayout(next)", deskbar)
        self.assertIn("bar.keyboardClicked()", deskbar)
        self.assertNotIn("Qt.inputMethod.show()", deskbar)

    def test_keyboard_qml_syncs_language_and_dynamic_prediction(self):
        kb = (ROOT / "shell/ui/Naomi/Ui/Keyboard.qml").read_text(encoding="utf-8")
        self.assertIn("function syncFromContext()", kb)
        self.assertIn("function syncToContext(code)", kb)
        self.assertIn("function onLayoutChanged()", kb)
        self.assertIn("function onFocusObjectChanged()", kb)
        self.assertIn("function canPredict()", kb)
        self.assertNotIn("readonly property bool predict:", kb)

    def test_cpp_keyboard_logic(self):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if not compiler:
            self.skipTest("No C++ compiler available on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            src = tmp_path / "test_keyboard_logic.cpp"
            exe = tmp_path / "test_keyboard_logic.exe"
            src.write_text(CPP_LOGIC_TEST, encoding="utf-8")
            compile_res = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{ROOT / 'shell/input'}", str(src), "-o", str(exe)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(compile_res.returncode, 0, compile_res.stderr)
            run_res = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
            self.assertEqual(run_res.returncode, 0, run_res.stderr)


if __name__ == "__main__":
    unittest.main()
