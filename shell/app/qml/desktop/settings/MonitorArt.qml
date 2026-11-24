// Монитор приставки в долях стола («Экран», «Обои», «Без дока»): экран — в пропорциях монитора, на нём обои и то, что
// на столе такого размера, в его настоящих долях: панель 40 точек и окно «Файлов» 1000 × 600 в (120, 24) под панелью
// (Desktop.qml) — меньше точек на столе, крупнее окно. lock — экран без дока: часы 150 точек (pi/dock/clock.cpp) там,
// где их поставили (по умолчанию слева 44, сверху 34; draggable — тащат мышью, отпустили — новое место в настройки),
// выключены — одни обои. Рамка 5 точек, подставка.
import QtQuick
import ".."

Item {
    id: m
    property var sw
    property bool lock: false
    property bool draggable: false   // без дока: часы можно перетащить мышью
    readonly property int deskW: sw.screen.deskWidth
    readonly property real screenW: width - 10
    readonly property real screenH: Math.round(screenW * sw.monitor.h / sw.monitor.w)
    readonly property real k: screenW / deskW   // точек рисунка в точке стола
    height: screenH + 10 + 12

    Rectangle {   // рамка
        width: m.width; height: m.screenH + 10
        radius: 3
        color: "#050505"
        border.width: 1
        border.color: DeskStyle.amberA(DeskStyle.line)
    }
    Item {
        x: 5; y: 5
        width: m.screenW; height: m.screenH
        clip: true

        Rectangle {   // без картинки — градиент стола
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0; color: "#1b1424" }
                GradientStop { position: 1; color: "#08070b" }
            }
        }
        Image {
            anchors.fill: parent
            visible: status === Image.Ready
            source: naomiFiles.desktopWallpaper
            fillMode: Image.PreserveAspectCrop
            sourceSize.width: 640
            asynchronous: true
            cache: false
        }

        Item {   // стол: панель и окно «Файлов»
            anchors.fill: parent
            visible: !m.lock
            Rectangle {
                width: parent.width; height: Math.max(2, 40 * m.k)
                color: DeskStyle.bg
                Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: DeskStyle.amberA(DeskStyle.line) }
                Rectangle { x: 16 * m.k; y: parent.height * 0.33; width: 14 * m.k; height: parent.height * 0.34; color: DeskStyle.amber }
                Rectangle { x: 44 * m.k; y: parent.height * 0.33; width: 34 * m.k; height: parent.height * 0.34; color: DeskStyle.muted; opacity: 0.55 }
                Rectangle { x: 92 * m.k; y: parent.height * 0.33; width: 30 * m.k; height: parent.height * 0.34; color: DeskStyle.muted; opacity: 0.55 }
                Rectangle {
                    x: parent.width - 60 * m.k; y: parent.height * 0.33
                    width: 44 * m.k; height: parent.height * 0.34
                    color: DeskStyle.muted; opacity: 0.55
                }
            }
            Rectangle {
                x: 120 * m.k; y: (40 + 24) * m.k
                width: 1000 * m.k; height: 600 * m.k
                radius: 2
                color: DeskStyle.bg
                border.width: 1
                border.color: DeskStyle.amberA(DeskStyle.line)
                clip: true
                Rectangle { y: 40 * m.k; width: parent.width; height: 1; color: DeskStyle.amberA(DeskStyle.line) }
                Rectangle { x: 22 * m.k; y: 15 * m.k; width: 70 * m.k; height: 10 * m.k; color: DeskStyle.amber; opacity: 0.8 }
                Rectangle { x: 200 * m.k; y: 40 * m.k; width: 1; height: parent.height; color: DeskStyle.amberA(DeskStyle.line) }
                Repeater {
                    model: [52, 40, 58, 34, 48, 56, 30, 44, 50]
                    delegate: Rectangle {
                        required property int index
                        required property int modelData
                        x: 232 * m.k; y: (70 + index * 52) * m.k
                        width: 760 * m.k * modelData / 60; height: Math.max(1, 8 * m.k)
                        color: DeskStyle.muted
                        opacity: 0.32
                    }
                }
            }
        }

        Column {   // без дока: часы — где поставили (левый верх надписи — доля экрана); draggable — их можно тащить
            id: clock
            visible: m.lock && m.sw.screen.clock
            // верх цифр ниже верха строки: надпись встаёт на место верхом чернил, как у приставки (pi/dock/clock.cpp)
            readonly property real inkTop: hhmm.baselineOffset - 0.72 * hhmm.font.pixelSize
            readonly property real inkH: height - inkTop
            readonly property real pad: 28 * m.k   // поле тени у приставки: ближе к краю часы не подходят
            readonly property real minX: pad
            readonly property real maxX: Math.max(pad, parent.width - pad - width)
            readonly property real minY: pad - inkTop
            readonly property real maxY: Math.max(minY, parent.height - pad - inkH - inkTop)
            function placed(fx, fy) { return [Math.min(maxX, Math.max(minX, fx * parent.width)),
                                              Math.min(maxY, Math.max(minY, fy * parent.height - inkTop))] }
            x: placed(m.sw.screen.clockX / 10000, m.sw.screen.clockY / 10000)[0]
            y: placed(m.sw.screen.clockX / 10000, m.sw.screen.clockY / 10000)[1]
            Text {
                id: hhmm
                text: Qt.formatTime(clockTick.now, "hh:mm")
                font.family: DeskStyle.clock
                font.weight: Font.Light
                font.pixelSize: Math.max(1, Math.round(150 * m.k))
                color: "#f2ede4"
            }
            Text {
                text: (clockTick.now.toLocaleDateString(Qt.locale("ru_RU"), "dddd") + " · "
                       + clockTick.now.toLocaleDateString(Qt.locale("ru_RU"), "d MMMM")).toUpperCase()
                font.family: DeskStyle.sans
                font.weight: Font.Medium
                font.pixelSize: Math.max(1, Math.round(19 * m.k))
                font.letterSpacing: 19 * m.k * 0.1
                color: Qt.rgba(242 / 255, 237 / 255, 228 / 255, 0.85)
            }
        }
        Rectangle {   // рамка под мышью и пока тащат
            visible: clock.visible && m.draggable && (grab.containsMouse || grab.drag.active)
            x: clock.x - 4; y: clock.y + clock.inkTop - 4
            width: clock.width + 8; height: clock.inkH + 8
            radius: 2
            color: grab.drag.active ? DeskStyle.amberA(0.08) : "transparent"
            border.width: 1
            border.color: DeskStyle.amberA(0.6)
        }
        MouseArea {   // тащить часы; отпустили — новое место в настройки (приставка перерисует свои)
            id: grab
            enabled: m.draggable && clock.visible
            x: clock.x; y: clock.y + clock.inkTop
            width: clock.width; height: clock.inkH
            hoverEnabled: true
            property bool moved: false
            onPressed: moved = false
            onPositionChanged: if (grab.drag.active) moved = true
            drag.target: clock
            drag.threshold: 2
            drag.minimumX: clock.minX; drag.maximumX: clock.maxX
            drag.minimumY: clock.minY; drag.maximumY: clock.maxY
            onReleased: {
                if (!moved)
                    return
                // оба сразу: запись X пересчитывает место часов (y — тоже) раньше, чем прочитали бы Y
                const s = m.sw.screen
                const nx = Math.round(clock.x / clock.parent.width * 10000)
                const ny = Math.round((clock.y + clock.inkTop) / clock.parent.height * 10000)
                s.clockX = nx
                s.clockY = ny
                clock.x = Qt.binding(() => clock.placed(s.clockX / 10000, s.clockY / 10000)[0])
                clock.y = Qt.binding(() => clock.placed(s.clockX / 10000, s.clockY / 10000)[1])
            }
        }
        Timer {
            id: clockTick
            property date now: new Date()
            running: m.lock && m.visible
            interval: 10000
            repeat: true
            triggeredOnStart: true
            onTriggered: now = new Date()
        }
    }
    Rectangle {   // подставка
        x: (m.width - 56) / 2; y: m.screenH + 10
        width: 56; height: 12
        color: "transparent"
        border.width: 1
        border.color: DeskStyle.amberA(DeskStyle.line)
    }
}
