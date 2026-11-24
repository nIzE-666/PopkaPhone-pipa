// Окно игры на столе (docs/GAMES.md). Картинка — окно программы naomi-game (клиент Wayland): кадр игры, уже увеличенный
// ею в целое число раз; стол дотягивает его до места в окне — по высоте или ширине, с пропорциями системы (у NES пиксель
// шире, чем выше: 8 к 7; у PS1 экран — 4 к 3), поля — чёрные. Над картинкой — полоса 40, как у «Терминала» и браузера: значок, имя игры и
// система, справа «в док» (режим панели: полосы нет, игра во всю свободную область, кнопки — на панели стола) и
// крестик — закрыть игру (память картриджа она сохранит). Больше ничего: ни меню, ни настроек эмулятора.
// Тянут за полосу, двойной щелчок — развернуть; края — размер (EdgeGrip), не меньше 320 × 280. Свернули (щелчок по
// имени игры на панели) — игра на паузе.
//   То же окно — у программ Windows (система win, docs/WINDOWS.md: размер знает сама программа, рамка идёт за ним) и у
// программ Linux (система app — Claude, docs/CLAUDE.md): размер окну задаёт стол — место под полосой в пикселях
// кадра (программа рисует пиксель в пиксель с кадром), развёрнутое и в доке — «развёрнуто» для неё (с alwaysMaximized —
// и не развёрнутое); видна только объявленная ею рамка окна (тень вокруг — нет); клавиши — как у окна Windows, а буквы
// не с английской клавиатуры — текстом (textCommit, stream_input.cpp).
import QtQuick
import QtQuick.Shapes
import QtWayland.Compositor
import Naomi.Desk
import ".."

Item {
    id: win
    property var toplevel
    property var xdgSurface
    property Item area                    // стол без панели
    property string title
    property string system                // nes, ps1; win — окно программы Windows (docs/WINDOWS.md); app — программы Linux
    property string label                 // NES
    property string glyph: "gamepad"      // значок на полосе
    property bool active: false
    property bool pinned: false
    property bool minimized: false
    property bool maximized: false
    property bool panelMode: false
    // Программе (система app) — «развёрнуто» и в окне не во весь стол: размер развёрнутого окна по протоколу задаёт
    // стол, это место окна. Окну Claude (docs/CLAUDE.md, «Скорость»): Chromium развёрнутому окну не рисует вокруг свою
    // тень и не скругляет углы — рисует он процессором, и на это уходила треть кадра (два лишних прохода по всему окну).
    // Тень стол и так не показывал, углы скругляет сам (WebSurface: outline, corner)
    property bool alwaysMaximized: false
    property bool naomiInput: false       // программа со своим модулем ввода naomi (naomi-web --app: org.naomi.Input.p<pid>)
    property rect box                     // рамка окна с полосой (не развёрнутого)
    readonly property bool game: true     // для стола: кнопки режима дока — на панели (Desktop.qml, panelTop)
    signal activated()
    signal closed()
    signal panelToggled()
    signal closeClicked()
    signal saveRequested()

    readonly property bool barShown: !panelMode
    readonly property real barH: barShown ? DeskStyle.webBarH : 0
    readonly property rect frame: panelMode || maximized ? Qt.rect(0, 0, area.width, area.height) : box
    readonly property real e: panelMode ? 0 : DeskStyle.corner(width, height)   // в доке — во всю область, углы прямые
    readonly property size minSize: Qt.size(320, 280)
    x: frame.x; y: frame.y; width: frame.width; height: frame.height

    function activate() { activated() }
    function takeFocus() { if (view.surface) view.takeFocus() }
    property alias surfaceItem: view      // поверхность: ей клавиши кнопок геймпада (naomiPadKeys, Desktop.qml)
    property alias touchScrollSpeed: view.touchScrollSpeed
    function togglePanel() { panelToggled() }
    function closeButton(mods) { closeClicked() }
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
    property bool closing: false
    function dragOver(p) {}
    function dropAt(p, paths) {}

    // появляется, сворачивается и возвращается — как окна НаомиОС: растёт от 0,96 и проявляется
    property bool shown: false
    Component.onCompleted: {
        shown = true
        if (system === "app")
            fitLater.restart()   // размер — сразу, программа ждёт его, чтобы нарисовать первый кадр
    }
    visible: shown && !minimized || opacity > 0
    opacity: shown && !minimized ? 1 : 0
    scale: shown && !minimized ? 1 : 0.96
    Behavior on opacity { NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: win.shown && !win.minimized ? 180 : 140; easing.type: Easing.OutCubic } }
    onActiveChanged: if (active && !minimized) takeFocus()

    // ---------- высота картинки → игре (по ней она выбирает, во сколько раз увеличить кадр) ----------
    readonly property size viewPixels: Qt.size(Math.round(screen.fitW * naomiDesk.scale), Math.round(screen.fitH * naomiDesk.scale))
    onViewPixelsChanged: fitLater.restart()
    Timer { id: fitLater; interval: 120; onTriggered: win.fit() }
    function fit() {
        if (system === "win")
            return   // окну Windows размер не навязываем: оно само знает свой
        if (system === "app") {
            if (toplevel && appSize.width > 0 && appSize.height > 0)
                maximized || panelMode || alwaysMaximized ? toplevel.sendMaximized(appSize) : toplevel.sendUnmaximized(appSize)
            return
        }
        if (toplevel && toplevel.sendFullscreen && viewPixels.width > 0 && viewPixels.height > 0)
            toplevel.sendFullscreen(viewPixels)
    }
    // программе Linux — место под полосой в пикселях кадра стола (развёрнуто или нет — тоже ей)
    readonly property size appSize: system === "app" ? Qt.size(Math.round(screen.width * naomiDesk.scale),
                                                               Math.round(screen.height * naomiDesk.scale))
                                                     : Qt.size(0, 0)
    onAppSizeChanged: fitLater.restart()
    onMaximizedChanged: if (system === "app") fitLater.restart()
    onPanelModeChanged: if (system === "app") fitLater.restart()

    // Окно Windows — пиксель в пиксель: рамка окна стола идёт за размером окна программы (в доке и развёрнутое —
    // растянуто до области, с полями; больше стола — уменьшено до него)
    readonly property size natural: system === "win" && screen.source.width > 0
                                    ? Qt.size(screen.source.width / naomiDesk.scale, screen.source.height / naomiDesk.scale)
                                    : Qt.size(0, 0)
    onNaturalChanged: {
        if (natural.width <= 0)
            return
        // не помещается в стол (игра во весь экран: Wine тянет её кадр на весь вывод стола) — меньше, с теми же
        // пропорциями: картинка под полосой видна целиком
        const k = Math.min(1, area.width / natural.width, (area.height - DeskStyle.webBarH) / natural.height)
        const w = Math.max(minSize.width, Math.round(natural.width * k))
        const h = Math.max(minSize.height, Math.round(natural.height * k) + DeskStyle.webBarH)
        const cx = box.x + box.width / 2, cy = box.y + box.height / 2   // вокруг прежней середины
        box = Qt.rect(Math.round(Math.max(0, Math.min(area.width - w, cx - w / 2))),
                      Math.round(Math.max(0, Math.min(area.height - h, cy - h / 2))), w, h)
    }

    // ---------- фон (полоса) ----------
    Shape {
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: DeskStyle.bg
            strokeColor: "transparent"
            PathPolyline { path: DeskStyle.outline(0, 0, win.width, win.height, win.e) }
        }
    }

    // ---------- экран ----------
    Item {
        id: screen
        y: win.barH
        width: win.width
        height: win.height - win.barH
        clip: true   // чёрное поле — контур всего окна, от полосы вниз
        Shape {
            y: -win.barH
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: "black"
                strokeColor: "transparent"
                PathPolyline { path: DeskStyle.outline(0, 0, win.width, win.height, win.e) }
            }
        }
        // место картинки в окне: по высоте или ширине, с пропорциями экрана системы, по сетке пикселей кадра стола
        // у окна Windows — видимая часть кадра (буфер у Wine больше окна); у программы Linux — рамка окна, которую она
        // объявила (xdg window geometry): вокруг Chromium рисует свою тень, её стол не показывает
        readonly property bool program: win.system === "win" || win.system === "app"
        // единиц элемента в единице поверхности: размер элементу Qt задаёт сам (поверхность × масштаб вывода / DPR окна)
        readonly property real unit: view.surface && view.surface.destinationSize.width > 0
                                     ? view.width / view.surface.destinationSize.width : 1
        readonly property rect geo: win.xdgSurface && win.xdgSurface.windowGeometry ? win.xdgSurface.windowGeometry
                                                                                    : Qt.rect(0, 0, 0, 0)
        readonly property rect part: win.system === "app" && geo.width > 0
                                     ? Qt.rect(geo.x * unit, geo.y * unit, geo.width * unit, geo.height * unit)
                                     : Qt.rect(0, 0, view.width, view.height)
        readonly property size source: !view.surface ? Qt.size(0, 0)
                                       : win.system === "app" ? Qt.size(part.width, part.height)
                                       : program ? view.surface.destinationSize : view.surface.bufferSize
        // NES — кадр с пикселем 8 к 7; PS1 — всегда 4 к 3, каким бы ни был кадр (320, 512, 640 в ширину)
        // окно Windows и Linux — как есть
        readonly property real aspect: win.system === "nes" && source.height > 0 ? source.width / source.height * 8 / 7
                                     : program && source.height > 0 ? source.width / source.height : 4 / 3
        readonly property real px: naomiDesk.pixelScale
        readonly property real fitH: Math.round(Math.min(height, width / aspect) * px) / px
        readonly property real fitW: Math.round(Math.min(width, fitH * aspect) * px) / px
        WebSurface {   // поверхность, обрезанная по контуру окна (web_surface.h): картинка во всё окно — углы скруглены
            id: view
            // клавиши — клавиатурой Wayland (stream_input.cpp): игре приставки — по месту клавиши, кнопками
            // геймпада; программе Windows — вся клавиатура с Shift и прочим, тем же путём, что окну Android
            property bool gameKeys: !screen.program
            property bool androidKeys: screen.program && !win.naomiInput
            property bool textCommit: win.system === "app" && !win.naomiInput
            // Linux desktop apps receive touch taps as pointer clicks and swipes as wheel events.
            touchToPointer: win.system === "app"
            pointerFrames: win.system === "app"   // колесо Chromium — с wl_pointer.frame (web_surface.h)
            onPointerPressed: win.activate()
            opaque: screen.program   // кадры Wine и Chromium — без прозрачности (web_surface.h); до shellSurface
            shellSurface: win.xdgSurface
            moveItem: Item {}
            // размер элемента — по буферу игры; до места в окне — растяжением (по ширине и высоте — своим); видна
            // часть part (у игр и окон Windows — вся поверхность)
            readonly property real sx: screen.part.width > 0 ? screen.fitW / screen.part.width : 1
            readonly property real sy: screen.part.height > 0 ? screen.fitH / screen.part.height : 1
            x: Math.round((screen.width - screen.fitW) / 2 * screen.px) / screen.px - screen.part.x * sx
            y: Math.round((screen.height - screen.fitH) / 2 * screen.px) / screen.px - screen.part.y * sy
            transform: Scale { xScale: view.sx; yScale: view.sy }
            // контур окна — в единицах элемента (до растяжения)
            outline: Qt.rect(-x / sx, (-y - win.barH) / sy, win.width / sx, win.height / sy)
            corner: win.e / sy
            pixel: 1 / (naomiDesk.scale * sy)
            onSurfaceDestroyed: win.closed()
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
        // тянут — окно едет (не выше панели); двойной щелчок — развернуть / вернуть
        MouseArea {
            anchors.fill: parent
            cursorShape: pressed && dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
            property point start
            property rect from
            property bool dragging: false
            onPressed: (m) => {
                win.activate()
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

        Glyph {
            kind: win.glyph
            x: 26; y: DeskStyle.webBarH / 2
            color: win.active ? DeskStyle.amber : DeskStyle.muted
        }
        Text {
            id: name
            x: 46
            width: Math.min(implicitWidth, Math.max(0, pinBtn.x - DeskStyle.webGrabGap - x - tag.implicitWidth - 12))
            height: DeskStyle.webBarH
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: win.title
            font.family: DeskStyle.sans
            font.pixelSize: 14
            color: win.active ? DeskStyle.text : DeskStyle.muted
        }
        Text {
            id: tag
            x: name.x + name.width + 12
            visible: x + implicitWidth <= pinBtn.x - DeskStyle.webGrabGap
            height: DeskStyle.webBarH
            verticalAlignment: Text.AlignVCenter
            text: win.label
            font.family: DeskStyle.mono
            font.pixelSize: DeskStyle.caps - 2
            font.letterSpacing: 1.0
            color: DeskStyle.dim
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
            onClicked: win.closeClicked()
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
        Component.onCompleted: cursorShape = undefined   // без формы: MouseArea ставит стрелку сама
        onPressed: (m) => { win.activate(); m.accepted = false }
    }
}
