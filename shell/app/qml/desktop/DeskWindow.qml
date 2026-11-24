// Окно рабочего стола — как окна НаомиОС (DESIGN «Кнопки окна», naomi-ui.c naomi_bar_*): полоса 40 с волосяной
// линией, справа кнопки 30 × 30 сверху 5, между ними 2, от крестика до края 6 — свернуть, развернуть, закрыть
// (под мышью — янтарь .16, у крестика — красный). Полосу тянут — окно едет (курсор — «тащу»); двойной щелчок —
// развернуть; края и углы — размер: курсор ↔ ↕ ⤡ ⤢ (EdgeGrip, docs/DESKTOP.md, «Курсор»). Форма — как у окон bcdesk
// (window_shape): углы «непрерывные», как у модулей, по краю — волосяная рамка
// янтарём .16 поверх содержимого (ring_set); мягких теней нет. Содержимое у краёв фона не рисует — его фон и есть
// фон окна, иначе вылезло бы за контур. Появляется и уходит как окна НаомиОС: растёт от 0,96 и проявляется; parked —
// окно в запасе (собрано заранее, ждёт невидимым), появляется, когда запас берут.
// Нажатие где угодно в окне — окно наверх (activated). tool — окно своей программы НаомиОС («Настройки»): только
// крестик, размер не тянут и не разворачивают. closeOnly — только крестик, но размер тянут и разворачивают двойным
// щелчком по полосе («Просмотр»); chromeless — без полосы, углов, рамки и краёв (окно на весь стол).
//   barContent — слева на полосе (путь, назад/вперёд), barTools — справа перед кнопками окна; содержимое — внутри
import QtQuick
import QtQuick.Shapes

Item {
    id: win
    property bool active: false
    property bool pinned: false
    property bool maximized: false
    property bool minimized: false
    property bool tool: false
    property bool closeOnly: tool
    property bool chromeless: false
    property Item area                       // где окна живут (стол без панели): развернуть — во всю
    property size minimumSize: Qt.size(520, 340)
    default property alias content: body.data
    property alias barContent: barArea.data
    property alias barTools: tools.data
    readonly property alias body: body
    signal activated()
    signal closed()          // окно ушло — его можно уничтожить

    readonly property int barH: chromeless ? 0 : DeskStyle.webBarH
    readonly property real corner: chromeless ? 0 : DeskStyle.corner(width, height)
    property rect normal                     // где было до «развернуть»

    function activate() { activated() }
    function close() {
        if (closing)
            return
        closing = true
        shown = false
    }
    function toggleMaximize() {
        if (!maximized) {
            normal = Qt.rect(x, y, width, height)
            x = 0; y = 0; width = area.width; height = area.height
        } else {
            x = normal.x; y = normal.y; width = normal.width; height = normal.height
        }
        maximized = !maximized
    }
    function snapHalf(right) {
        if (!area)
            return
        maximized = false
        const half = Math.floor(area.width / 2)
        x = right ? area.width - half : 0
        y = 0
        width = half
        height = area.height
        normal = Qt.rect(x, y, width, height)
    }

    property bool closing: false
    property bool shown: false
    // Запас (Desktop.qml, «Файлы»): окно собрано заранее и ждёт за краем стола прозрачным, но видимым для Qt —
    // элементы разложены, узлы сцены готовы; unpark() — появляется, как только что созданное
    property bool parked: false
    Component.onCompleted: if (!parked) shown = true
    // из запаса — без мига «невидимо» (shown раньше, чем parked): невидимое и снова видимое окно Qt раскладывал
    // заново — фигуры Shape пересчитывались, ~7 мс (замер 17 ноября 2026)
    function unpark() {
        shown = true
        parked = false
    }
    visible: parked || shown && !minimized || opacity > 0
    // warm — запас только что собран: один кадр он рисуется почти прозрачным (за краем стола — не видно). Первое
    // открытие после запуска оболочки без этого стояло 35–45 мс (Qt впервые готовит отрисовку окна), с ним — 17–23 мс
    property bool warm: false
    opacity: shown && !minimized ? 1 : parked && warm ? 0.002 : 0
    scale: shown && !minimized ? 1 : 0.96
    Behavior on opacity {   // прогрев запаса (warm) — сразу, без анимации
        enabled: !win.parked || win.shown
        NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic }
    }
    Behavior on scale { NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic } }
    onOpacityChanged: if (closing && opacity === 0) closed()

    // ---------- фон: контур окна ----------
    Shape {
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: DeskStyle.bg
            strokeColor: "transparent"
            PathPolyline { path: DeskStyle.outline(0, 0, win.width, win.height, win.corner) }
        }
    }

    // ---------- полоса ----------
    Item {
        id: bar
        visible: !win.chromeless
        width: parent.width
        height: win.barH
        MouseArea {   // пустое место полосы: тянут — окно едет (не выше панели), двойной щелчок — развернуть
            anchors.fill: parent
            cursorShape: pressed && !win.maximized ? Qt.ClosedHandCursor : Qt.ArrowCursor
            property point from
            onPressed: (m) => { from = Qt.point(m.x, m.y) }
            onPositionChanged: (m) => {
                if (win.maximized)
                    return
                win.x += m.x - from.x
                win.y = Math.max(0, win.y + m.y - from.y)
            }
            onDoubleClicked: if (!win.tool) win.toggleMaximize()
        }
        Item {
            id: barArea
            anchors.left: parent.left
            anchors.right: tools.left
            height: win.barH
        }
        Row {
            id: tools
            anchors.right: buttons.left
            height: win.barH
        }
        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 6
            y: (win.barH - DeskStyle.windowButton) / 2
            spacing: 2
            WinButton { visible: DeskStyle.tablet && !win.closeOnly; glyph: "pin"; active: win.pinned; onClicked: { win.pinned = !win.pinned; win.activate() } }
            WinButton { visible: DeskStyle.tablet && !win.closeOnly; glyph: "split-left"; onClicked: win.snapHalf(false) }
            WinButton { visible: DeskStyle.tablet && !win.closeOnly; glyph: "split-right"; onClicked: win.snapHalf(true) }
            WinButton { visible: !win.closeOnly; glyph: "minimize"; onClicked: win.minimized = true }
            WinButton { visible: !win.closeOnly; glyph: win.maximized ? "restore" : "maximize"; onClicked: win.toggleMaximize() }
            WinButton { glyph: "close"; danger: true; onClicked: win.close() }
        }
        Rectangle {
            y: win.barH - 1
            width: parent.width; height: 1
            color: DeskStyle.amberA(DeskStyle.line)
        }
    }

    Item {
        id: body
        anchors.top: bar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    // рамка — поверх содержимого: его ничто не закроет
    Shape {
        z: 999
        visible: !win.chromeless
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: DeskStyle.amberA(DeskStyle.line)
            strokeWidth: 1
            PathPolyline { path: DeskStyle.outline(0.5, 0.5, win.width - 1, win.height - 1, win.corner - 0.5) }
        }
    }

    // ---------- размер: края и углы (EdgeGrip: 12 точек снаружи, 4 внутри, угол — 32 вдоль сторон) ----------
    component Edge: EdgeGrip {
        property rect start
        property point from
        visible: !win.maximized && !win.tool && !win.chromeless
        onPressed: (m) => {
            win.activate()
            start = Qt.rect(win.x, win.y, win.width, win.height)
            from = mapToItem(win.parent, m.x, m.y)
        }
        onPositionChanged: (m) => {
            if (!pressed)
                return
            const p = mapToItem(win.parent, m.x, m.y)
            const dx = p.x - from.x, dy = p.y - from.y
            if (pull.x > 0) win.width = Math.max(win.minimumSize.width, start.width + dx)
            if (pull.y > 0) win.height = Math.max(win.minimumSize.height, start.height + dy)
            if (pull.x < 0) {
                const w = Math.max(win.minimumSize.width, start.width - dx)
                win.x = start.x + start.width - w; win.width = w
            }
            if (pull.y < 0) {
                const h = Math.max(win.minimumSize.height, start.height - dy)
                win.y = Math.max(0, start.y + start.height - h); win.height = start.y + start.height - win.y
            }
        }
    }
    Edge { sx: -1 }
    Edge { sx: 1 }
    Edge { sy: -1 }
    Edge { sy: 1 }

    // нажатие в окне — окно наверх (и дальше — тому, на что нажали)
    MouseArea {
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.AllButtons
        Component.onCompleted: cursorShape = undefined   // без формы: MouseArea ставит стрелку сама и закрыла бы формы под собой
        onPressed: (m) => { win.activate(); m.accepted = false }
    }
}
