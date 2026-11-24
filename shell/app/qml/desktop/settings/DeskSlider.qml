// Ползунок (веб Наоми .home-range): дорожка 2 точки — янтарь до значения, дальше линия .16; бегунок 10 × 16 со
// скруглением 2, фон #1b1b19, рамка янтарём. Тянут или щёлкают по дорожке — значение по сетке stepSize.
import QtQuick
import ".."

Item {
    id: s
    property real from: 0
    property real to: 100
    property real stepSize: 1
    property real value: 0
    property real touchMargin: 6
    signal moved(real value)
    height: 16

    readonly property real t: Math.max(0, Math.min(1, (value - from) / (to - from)))
    Rectangle { y: 7; width: s.width; height: 2; color: DeskStyle.amberA(DeskStyle.line) }
    Rectangle { y: 7; width: s.t * s.width; height: 2; color: DeskStyle.amber }
    Rectangle {
        x: s.t * s.width - 5
        width: 10; height: 16
        radius: 2
        color: "#1b1b19"
        border.width: 1
        border.color: DeskStyle.amber
    }
    MouseArea {
        anchors.fill: parent
        anchors.margins: -s.touchMargin
        function at(mx) {
            const u = Math.max(0, Math.min(1, (mx - s.touchMargin) / s.width))
            const v = Math.round((s.from + u * (s.to - s.from)) / s.stepSize) * s.stepSize
            if (v !== s.value)
                s.moved(v)
        }
        onPressed: (m) => at(m.x)
        onPositionChanged: (m) => { if (pressed) at(m.x) }
    }
}
