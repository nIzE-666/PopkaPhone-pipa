import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "shell/app/web_surface.cpp").read_text(encoding="utf-8")
TOUCH = SOURCE.split("void WebSurface::touchEvent(", 1)[1].split("void WebSurface::resetTouch()", 1)[0]


class WebSurfaceTouchTest(unittest.TestCase):
    def test_begin_positions_pointer_without_pressing(self):
        begin = TOUCH.split("if (type == QEvent::TouchBegin) {", 1)[1].split("if (m_touchId < 0)", 1)[0]
        self.assertIn("m_touchId = primary.id();", begin)
        self.assertIn("m_touchStart = m_touchLast = primary.position();", begin)
        self.assertIn("sendMouseMoveEvent(view(), mapToSurface(m_touchStart)", begin)
        self.assertIn("sendPointerFrameForSeat(seat);", begin)
        self.assertNotIn("sendMousePressEvent", begin)

    def test_tap_is_the_only_branch_that_clicks(self):
        end = TOUCH.split("if (type == QEvent::TouchEnd || primary->state()", 1)[1].split(
            "const QPointF step", 1
        )[0]
        tap = end.split("if (!m_touchScrolling) {", 1)[1].split("\n        }", 1)[0]
        self.assertIn("sendMouseMoveEvent(view(), mapToSurface(m_touchStart)", tap)
        self.assertRegex(tap, r"sendMousePressEvent\(Qt::LeftButton\);\s+sendPointerFrameForSeat\(seat\);")
        self.assertRegex(tap, r"sendMouseReleaseEvent\(Qt::LeftButton\);\s+sendPointerFrameForSeat\(seat\);")
        self.assertEqual(TOUCH.count("sendMousePressEvent("), 1)
        self.assertEqual(TOUCH.count("sendMouseReleaseEvent("), 1)
        self.assertIn("resetTouch();", end)

    def test_swipe_latches_and_sends_signed_wheel_steps(self):
        self.assertIn("const QPointF fromStart = mapToScene(pos) - mapToScene(m_touchStart);", TOUCH)
        self.assertIn("m_touchScrolling |= QPointF::dotProduct(fromStart, fromStart)", TOUCH)
        self.assertIn("const QPointF step = pos - m_touchLast;", TOUCH)
        self.assertIn("m_touchLast = pos;", TOUCH)
        self.assertIn("if (!m_touchScrolling || step.isNull())", TOUCH)
        # Qt 6.10 sends -angleDelta / 12 to Wayland on both axes directly via seat.
        self.assertIn("seat->sendMouseWheelEvent(Qt::Horizontal, delta.x() * 12);", TOUCH)
        self.assertIn("seat->sendMouseWheelEvent(Qt::Vertical, delta.y() * 12);", TOUCH)

    def test_wheel_sends_directly_to_wayland_seat_without_qwheelevent(self):
        self.assertNotIn("QWheelEvent wheel(", TOUCH)
        self.assertNotIn("wheelEvent(", TOUCH)

    def test_scroll_speed_scales_both_axes_and_carries_fractional_steps(self):
        self.assertIn("qBound(10, m_touchScrollSpeed, 100) / 100.0", TOUCH)
        self.assertIn("step * speed + m_touchScrollRemainder", TOUCH)
        self.assertIn("m_touchScrollRemainder = scaledStep - QPointF(delta);", TOUCH)
        reset = SOURCE.split("void WebSurface::resetTouch()", 1)[1].split("void WebSurface::touchUngrabEvent()", 1)[0]
        self.assertIn("m_touchScrollRemainder = {};", reset)

    def test_tablet_scroll_speed_is_saved_and_bound_to_app_windows(self):
        desktop_dir = ROOT / "shell/app/qml/desktop"
        desktop = (desktop_dir / "Desktop.qml").read_text(encoding="utf-8")
        settings = desktop.split("id: screenSettings", 1)[1].split("\n    }", 1)[0]
        self.assertIn("property int touchScrollSpeed: 35", settings)
        self.assertIn("touchScrollSpeed: DeskStyle.tablet ? screenSettings.touchScrollSpeed : 100", desktop)
        window = (desktop_dir / "game/GameWindow.qml").read_text(encoding="utf-8")
        self.assertIn("property alias touchScrollSpeed: view.touchScrollSpeed", window)
        page = (desktop_dir / "settings/TabletScreenPage.qml").read_text(encoding="utf-8")
        self.assertIn("value: sw.screen.touchScrollSpeed", page)
        self.assertIn("sw.screen.touchScrollSpeed = value", page)

    def test_cancel_and_ungrab_clear_the_gesture(self):
        cancel = TOUCH.split("if (type == QEvent::TouchCancel) {", 1)[1].split("}", 1)[0]
        ungrab = SOURCE.split("void WebSurface::touchUngrabEvent()", 1)[1].split("}", 1)[0]
        for block in (cancel, ungrab):
            self.assertIn("resetTouch();", block)
            self.assertNotIn("sendMousePressEvent", block)
            self.assertNotIn("sendMouseReleaseEvent", block)
