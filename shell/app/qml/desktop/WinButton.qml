// Кнопка-значок окна НаомиОС (button.naomi-winbtn): 30 × 30, скругление 3, значок приглушённо; под мышью — янтарь .16
// и светлый значок, нажата — .22; danger (крестик) — красный .18 и красный значок.
import QtQuick

Item {
    id: b
    property string glyph
    property bool danger: false
    property bool active: false
    property bool enabled_: true
    property real unit: 1                 // размер значка (у браузера крестик ±5,25 и «в панель» — свои)
    property real line: 1.4
    signal clicked(int modifiers)
    width: DeskStyle.windowButton
    height: width
    opacity: enabled_ ? 1 : 0.45

    Rectangle {
        anchors.fill: parent
        radius: 3
        visible: (area.containsMouse || b.active) && b.enabled_
        color: b.danger ? Qt.rgba(217 / 255, 112 / 255, 95 / 255, area.pressed ? 0.26 : 0.18)
                        : DeskStyle.amberA(area.pressed ? 0.22 : b.active ? 0.18 : 0.16)
    }
    Glyph {
        x: b.width / 2; y: b.height / 2
        kind: b.glyph
        unit: b.unit
        line: b.line
        color: !b.enabled_ ? DeskStyle.dim : area.containsMouse ? (b.danger ? DeskStyle.bad : DeskStyle.text) : b.active ? DeskStyle.amber : DeskStyle.muted
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: (m) => { if (b.enabled_) b.clicked(m.modifiers) }
    }
}
