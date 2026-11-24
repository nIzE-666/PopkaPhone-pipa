// Окно «Терминала» на столе (docs/TERMINAL.md) — как окно браузера (WebWindow): полоса 40 с вкладками и «+»
// (TermStrip), справа «в док» (режим панели) и крестик (сворачивает; с Shift — закрыть терминал), под ней — текст
// открытой вкладки (TermView). Полоса и текст — одно целое: углы «непрерывные», рамка .16 поверх; фон текста — уголь
// окна, поэтому углы ничем не срезаются. Тянут за пустое место полосы, двойной щелчок — развернуть; развёрнутое тянут
// дальше 5 точек — прежний размер под курсором; размер — за невидимую рамку (EdgeGrip), не меньше 480 × 280.
// Режим дока — без полосы, текст во всю свободную область, вкладки — на панели. Появляется и уходит как окна НаомиОС:
// растёт от 0,96 и проявляется. Перетащили файлы из «Файлов» — их пути в строку, в кавычках.
import QtQuick
import QtQuick.Shapes
import Naomi.Desk
import ".."

Item {
    id: win
    property var st                       // TermState
    property Item area                    // стол без панели
    property bool active: false
    property bool pinned: false
    property bool minimized: false
    property bool maximized: false
    property bool panelMode: false
    property int extraKeysMode: 0         // 0 — авто (скрывается при физ. клавиатуре), 1 — всегда показать, 2 — скрыть
    property int fontSize: 15
    property rect box                     // рамка окна с полосой (не развёрнутого)
    signal activated()
    signal closed()
    signal panelToggled()
    signal closeClicked(int modifiers)
    signal saveRequested()
    signal fontSizePicked(int size)
    signal textMenu()                     // правая кнопка по тексту — «Копировать / Вставить»

    readonly property alias view: view
    readonly property bool barShown: !panelMode
    readonly property real barH: barShown ? DeskStyle.webBarH : 0
    readonly property bool hwKeyboard: typeof naomiSystemTools !== "undefined" && naomiSystemTools !== null && naomiSystemTools.hardwareKeyboard
    readonly property bool extraKeysShown: DeskStyle.tablet && (extraKeysMode === 1 || (extraKeysMode === 0 && !hwKeyboard))
    readonly property real extraBarH: extraKeysShown ? 38 : 0
    readonly property rect frame: panelMode || maximized ? Qt.rect(0, 0, area.width, area.height) : box
    readonly property real e: DeskStyle.corner(width, height)
    readonly property size minSize: Qt.size(480, 280)
    x: frame.x; y: frame.y; width: frame.width; height: frame.height

    function activate() { activated() }
    function takeFocus() { view.forceActiveFocus() }
    function snapHalf(right) {
        if (!area)
            return
        if (panelMode)
            panelToggled()
        maximized = false
        const half = Math.floor(area.width / 2)
        box = Qt.rect(right ? area.width - half : 0, 0, half, area.height)
        saveRequested()
    }
    function setZoom(nextSize) {
        const clamped = Math.max(10, Math.min(26, nextSize))
        if (clamped !== fontSize) {
            fontSize = clamped
            fontSizePicked(clamped)
        }
    }
    property bool closing: false
    property bool shown: false
    Component.onCompleted: shown = true
    function close() {
        if (closing)
            return
        closing = true
        shown = false
    }
    // перетаскивание файлов (Desktop.qml): пути — в строку, каждый в кавычках, как их вставил бы терминал Linux
    function dragOver(p) {}
    function dropAt(p, paths) {
        if (!paths.length)
            return
        view.insert(paths.map(s => "'" + s.replace(/'/g, "'\\''") + "'").join(" ") + " ")
        activate()
    }

    visible: shown && !minimized || opacity > 0
    opacity: shown && !minimized ? 1 : 0
    scale: shown && !minimized ? 1 : 0.96
    Behavior on opacity { NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic } }
    onOpacityChanged: if (closing && opacity === 0) closed()
    onActiveChanged: if (active && !minimized && !(st && st.renaming >= 0)) takeFocus()

    // ---------- фон ----------
    Shape {
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: DeskStyle.bg
            strokeColor: "transparent"
            PathPolyline { path: DeskStyle.outline(0, 0, win.width, win.height, win.e) }
        }
    }

    // ---------- текст ----------
    TermView {
        id: view
        y: win.barH
        width: win.width
        height: Math.max(40, win.height - win.barH - win.extraBarH)
        session: naomiTerm.activeSession
        fontFamily: DeskStyle.mono
        fontSize: win.fontSize
        pixel: naomiDesk.pixelScale
        // мигает, только пока окно в фокусе и стол кто-то смотрит
        blink: win.active && !win.minimized && (DeskStyle.tablet ? naomiPower.displayOn
                                                : naomiStream.connected && naomiStream.desktop && !naomiStream.deskAway)
        onGridChanged: naomiTerm.setSize(cols, rows)
        Component.onCompleted: naomiTerm.setSize(cols, rows)
        onMenuRequested: win.textMenu()
        onShortcut: (name, arg) => {
            if (name === "new")
                win.st.newHere()
            else if (name === "close")
                win.st.close(naomiTerm.active)
            else if (name === "cycle")
                naomiTerm.cycle(arg)
            else if (name === "select")
                naomiTerm.select(arg)
            else if (name === "zoom")
                win.setZoom(arg === 0 ? 15 : win.fontSize + arg)
        }
    }

    // ---------- сенсорная панель спецклавиш (планшет) ----------
    Item {
        id: extraKeys
        visible: win.extraBarH > 0
        y: win.height - win.extraBarH
        width: win.width
        height: win.extraBarH
        Rectangle {
            width: parent.width; height: 1
            color: DeskStyle.amberA(DeskStyle.line)
        }
        Flickable {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.topMargin: 5
            anchors.bottomMargin: 5
            contentWidth: keyRow.width
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Row {
                id: keyRow
                spacing: 6
                height: parent.height
                Repeater {
                    model: [
                        { label: "ESC", seq: "\x1b" },
                        { label: "TAB", seq: "\t" },
                        { label: "^C", seq: "\x03", danger: true },
                        { label: "^D", seq: "\x04" },
                        { label: "^R", seq: "\x12" },
                        { label: "←", seq: "\x1b[D" },
                        { label: "↓", seq: "\x1b[B" },
                        { label: "↑", seq: "\x1b[A" },
                        { label: "→", seq: "\x1b[C" },
                        { label: "|", seq: "|" },
                        { label: "/", seq: "/" },
                        { label: "~", seq: "~" },
                        { label: "-", seq: "-" },
                        { label: "COPY", act: "copy" },
                        { label: "PASTE", act: "paste" },
                        { label: "A−", act: "zoom-" },
                        { label: "A+", act: "zoom+" }
                    ]
                    delegate: Rectangle {
                        id: kbtn
                        required property var modelData
                        height: keyRow.height
                        width: Math.max(40, ktxt.implicitWidth + 16)
                        radius: 4
                        color: ktap.pressed ? DeskStyle.amberA(0.22) : DeskStyle.surface
                        border.width: 1
                        border.color: modelData.danger ? DeskStyle.badA(0.45) : DeskStyle.amberA(DeskStyle.line)
                        Text {
                            id: ktxt
                            anchors.centerIn: parent
                            text: kbtn.modelData.label
                            font.family: DeskStyle.mono
                            font.pixelSize: 12
                            color: kbtn.modelData.danger ? DeskStyle.bad : DeskStyle.text
                        }
                        TapHandler {
                            id: ktap
                            onTapped: {
                                if (typeof naomiHaptics !== "undefined" && naomiHaptics)
                                    naomiHaptics.tap(10)
                                win.activate()
                                win.takeFocus()
                                if (kbtn.modelData.seq !== undefined)
                                    view.sendRaw(kbtn.modelData.seq)
                                else if (kbtn.modelData.act === "copy")
                                    view.copy()
                                else if (kbtn.modelData.act === "paste")
                                    view.paste()
                                else if (kbtn.modelData.act === "zoom-")
                                    win.setZoom(win.fontSize - 1)
                                else if (kbtn.modelData.act === "zoom+")
                                    win.setZoom(win.fontSize + 1)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---------- полоса ----------
    Item {
        id: bar
        visible: win.barShown
        width: win.width
        height: DeskStyle.webBarH

        Rectangle {   // линия снизу
            y: DeskStyle.webBarH - 1
            width: parent.width; height: 1
            color: DeskStyle.amberA(DeskStyle.line)
        }
        // пустое место: тянут — окно едет (не выше панели); двойной щелчок — развернуть / вернуть
        MouseArea {
            anchors.fill: parent
            cursorShape: pressed && dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
            property point start
            property rect from
            property bool dragging: false
            onPressed: (m) => {
                win.activate()
                if (!(win.st && win.st.renaming >= 0))
                    win.takeFocus()
                start = mapToItem(win.area, m.x, m.y)
                from = win.box
                dragging = !win.maximized
            }
            onPositionChanged: (m) => {
                const p = mapToItem(win.area, m.x, m.y)
                if (!dragging) {
                    if (Math.hypot(p.x - start.x, p.y - start.y) < 5)
                        return
                    const fx = start.x / win.area.width
                    win.box = Qt.rect(p.x - fx * from.width, p.y - start.y, from.width, from.height)
                    win.maximized = false
                    from = win.box
                    start = p
                    dragging = true
                }
                win.box = Qt.rect(from.x + p.x - start.x, Math.max(0, from.y + p.y - start.y), from.width, from.height)
            }
            onReleased: if (dragging) win.saveRequested()
            onDoubleClicked: {
                win.maximized = !win.maximized
                win.saveRequested()
            }
        }

        TermStrip {
            x: 8; y: 5
            width: Math.max(0, pinBtn.x - DeskStyle.webGrabGap - x)
            tabs: naomiTerm.tabs
            active: naomiTerm.active
            renaming: win.st.renaming
            onTabPressed: (id, button) => win.st.pressed(id, button)
            onTabDoubleClicked: (id) => win.st.startRename(id)
            onTabClosed: (id) => win.st.close(id)
            onTabRenamed: (id, name) => win.st.rename(id, name)
            onRenameCancelled: win.st.cancelRename()
            onPlusClicked: win.st.plus()
        }

        WinButton {
            id: pinBtn
            visible: DeskStyle.tablet
            x: DeskStyle.tablet ? splitLeftBtn.x - 2 - width : panelButton.x
            y: (DeskStyle.webBarH - height) / 2
            glyph: "pin"
            active: win.pinned
            onClicked: { win.pinned = !win.pinned; win.activate() }
        }
        WinButton {
            id: splitLeftBtn
            visible: DeskStyle.tablet
            x: DeskStyle.tablet ? splitRightBtn.x - 2 - width : panelButton.x
            y: (DeskStyle.webBarH - height) / 2
            glyph: "split-left"
            onClicked: win.snapHalf(false)
        }
        WinButton {
            id: splitRightBtn
            visible: DeskStyle.tablet
            x: DeskStyle.tablet ? panelButton.x - 2 - width : panelButton.x
            y: (DeskStyle.webBarH - height) / 2
            glyph: "split-right"
            onClicked: win.snapHalf(true)
        }
        WinButton {
            id: panelButton
            x: win.width - 8 - 2 * width; y: (DeskStyle.webBarH - height) / 2
            glyph: "panel"
            unit: 0.9545
            line: 1.5
            onClicked: win.panelToggled()
        }
        WinButton {
            x: win.width - 6 - width; y: (DeskStyle.webBarH - height) / 2
            glyph: "close"
            danger: true
            unit: 5.25 / 4.5
            line: 1.5
            onClicked: (mods) => win.closeClicked(mods)
        }
    }

    // ---------- рамка ----------
    Shape {
        z: 999
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: DeskStyle.amberA(DeskStyle.line)
            strokeWidth: 1
            PathPolyline { path: DeskStyle.outline(0.5, 0.5, win.width - 1, win.height - 1, Math.max(0, win.e - 0.5)) }
        }
    }

    // ---------- размер: невидимая рамка (окно с полосой, не развёрнутое) ----------
    component Edge: EdgeGrip {
        visible: win.barShown && !win.maximized
        property point start
        property rect from
        onPressed: (m) => {
            win.activate()
            start = mapToItem(win.area, m.x, m.y)
            from = win.box
        }
        onPositionChanged: (m) => {
            if (!pressed)
                return
            const p = mapToItem(win.area, m.x, m.y), dx = p.x - start.x, dy = p.y - start.y
            let x = from.x, y = from.y, w = from.width, h = from.height
            if (pull.x > 0) w = Math.max(win.minSize.width, from.width + dx)
            if (pull.x < 0) { w = Math.max(win.minSize.width, from.width - dx); x = from.x + from.width - w }
            if (pull.y > 0) h = Math.max(win.minSize.height, from.height + dy)
            if (pull.y < 0) {
                h = Math.max(win.minSize.height, from.height - dy)
                y = Math.max(0, from.y + from.height - h)
                h = from.y + from.height - y
            }
            win.box = Qt.rect(x, y, w, h)
        }
        onReleased: win.saveRequested()
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
        Component.onCompleted: cursorShape = undefined   // без формы: иначе стрелка закрыла бы курсор текста
        onPressed: (m) => { win.activate(); m.accepted = false }
    }
}
