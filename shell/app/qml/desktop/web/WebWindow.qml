// Окно браузера на столе (docs/WEB.md; webframe_* в bcdesk.c). Страница — окно программы naomi-web (клиент Wayland),
// она рисует в пикселях кадра стола (1,5 на точку) — показываем её буфер 1:1, уменьшенным в 1,5 раза; размер просим
// «полным экраном» нужного размера (как bcdesk у Chromium с --kiosk). Над страницей — своя полоса 40: звезда
// «Избранного», вкладки и «+» (WebStrip), загрузки, «в панель» и крестик (сворачивает; с Shift — закрыть браузер).
// Полоса и страница — одно целое: углы «непрерывные», верхние — у полосы, нижние — у страницы, рамка .16 поверх.
//   Тянут за пустое место полосы, двойной щелчок — развернуть; развёрнутое тянут дальше 5 точек — возвращается к
//   прежнему размеру под курсором. Размер — за невидимую рамку 12 точек снаружи (EdgeGrip), не меньше 640 × 400.
//   Режим панели — без полосы, страница во всю свободную область, вкладки — на панели.
//   Видео на весь экран — весь стол, поверх панели, прямые углы.
import QtQuick
import QtQuick.Shapes
import QtWayland.Compositor
import Naomi.Desk
import ".."

Item {
    id: win
    property var st                       // WebState
    property var toplevel
    property var xdgSurface
    property Item area                    // стол без панели
    property bool active: false
    property bool pinned: false
    property bool minimized: false
    property bool maximized: false
    property bool panelMode: false
    property rect box                     // рамка окна с полосой (не развёрнутого)
    property bool bookmarksOpen: false
    signal activated()
    signal closed()
    signal panelToggled()
    signal starClicked(real x, real y)    // где звезда — в координатах стола (список «Избранного» под ней)
    signal downloadActivated(var info)

    readonly property real k: 1.5         // пикселей кадра в точке стола
    readonly property bool fullscreen: naomiWeb.fullscreen
    readonly property bool barShown: !panelMode && !fullscreen
    readonly property real barH: barShown ? DeskStyle.webBarH : 0
    readonly property rect frame: fullscreen ? Qt.rect(0, -area.y, area.width, area.height + area.y)
                                : panelMode || maximized ? Qt.rect(0, 0, area.width, area.height) : box
    readonly property real e: fullscreen ? 0 : DeskStyle.corner(width, height)
    x: frame.x; y: frame.y; width: frame.width; height: frame.height

    function activate() { activated() }
    // для стола — как у окон «Файлов»: перетаскивание файлов в браузер не принимаем
    property bool closing: false
    function dragOver(p) {}
    function dropAt(p, paths) {}
    function takeFocus() { if (page.surface) page.takeFocus() }
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

    // свернуть и вернуть — как окна НаомиОС: тает и чуть уменьшается
    visible: opacity > 0
    opacity: minimized ? 0 : 1
    scale: minimized ? 0.96 : 1
    Behavior on opacity { NumberAnimation { duration: win.minimized ? 140 : 180; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: win.minimized ? 140 : 180; easing.type: Easing.OutCubic } }

    // ---------- размер страницы → браузеру ----------
    readonly property size pagePixels: Qt.size(Math.round(width * k), Math.round((height - barH) * k))
    onPagePixelsChanged: fitLater.restart()
    Timer { id: fitLater; interval: 0; onTriggered: win.fit() }
    function fit() {   // браузер перезапускается (стол сменил масштаб) — старого окна уже нет, а таймер успел: мимо
        if (toplevel && toplevel.sendFullscreen && pagePixels.width > 0 && pagePixels.height > 0)
            toplevel.sendFullscreen(pagePixels)
    }
    onToplevelChanged: fit()

    // ---------- фон (полоса) ----------
    Shape {
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: DeskStyle.bg
            strokeColor: "transparent"
            PathPolyline { path: DeskStyle.outline(0, 0, win.width, win.height, win.e) }
        }
    }

    // ---------- страница ----------
    Item {
        id: pageBox
        y: win.barH
        width: win.width
        height: win.height - win.barH
        // углы страницы: нижние (и верхние, когда полосы нет) — по контуру окна, геометрией (web_surface.h), а не
        // слоем с маской: тот стоил видеочипу четверти загрузки на ролике и пересоздавался на каждом шаге
        // растягивания окна (docs/AUDIT.md, этап 1)
        WebSurface {
            id: page
            outline: Qt.rect(0, -win.barH * win.k, win.width * win.k, win.height * win.k)
            corner: win.e * win.k
            pixel: win.k / naomiDesk.scale
            shellSurface: win.xdgSurface
            moveItem: Item {}
            width: surface ? surface.destinationSize.width : 0
            height: surface ? surface.destinationSize.height : 0
            scale: 1 / win.k
            transformOrigin: Item.TopLeft
            onSurfaceDestroyed: win.closed()
            // форма курсора над страницей — та, что поставила страница (текст, ссылка, «занято»): браузер говорит её
            // столу (naomiWeb.cursorShape); стол — зрителю, курсор рисует он (docs/DESKTOP.md, «Курсор»)
            HoverHandler { cursorShape: naomiWeb.cursorShape }
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
            cursorShape: pressed && dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor   // окно едет — «тащу»
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
                    // развёрнутое: вернуть прежний размер под курсором — та же доля ширины, тот же отступ сверху
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

        WebStrip {
            id: strip
            x: 8; y: 5
            width: Math.max(0, downloads.x - DeskStyle.webGrabGap - x)
            st: win.st
            withStar: true
            bookmarksOpen: win.bookmarksOpen
            onStarClicked: {
                const p = strip.mapToItem(win.area.parent, 0, DeskStyle.appH + 5)
                win.starClicked(p.x, p.y + 6)
            }
        }

        // загрузки — справа налево от «в панель», через 6; новая — левее всех; не ставится, если строке тесно
        Row {
            id: downloads
            layoutDirection: Qt.RightToLeft
            spacing: 6
            x: pinBtn.x - 6 - width
            y: 5
            Repeater {
                model: win.st.downloads
                delegate: WebDownload {
                    required property var modelData
                    info: modelData
                    visible: pinBtn.x - 6 - x - width >= 8 + DeskStyle.webGhostW + DeskStyle.itemGap + DeskStyle.webActiveMin
                    onActivated: win.downloadActivated(modelData)
                }
            }
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
            onClicked: (mods) => {
                if (mods & Qt.ShiftModifier)
                    naomiWeb.send({ type: "quit" })
                else
                    win.minimized = true
            }
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

    // ---------- размер: невидимая рамка снаружи (только окно с полосой, не развёрнутое) ----------
    // Внутрь окна край не заходит: там страница (окно naomi-web) — до самого края, со своей полосой прокрутки
    signal saveRequested()
    component Edge: EdgeGrip {
        inner: 0
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
            if (pull.x > 0) w = Math.max(DeskStyle.webMin.width, from.width + dx)
            if (pull.x < 0) { w = Math.max(DeskStyle.webMin.width, from.width - dx); x = from.x + from.width - w }
            if (pull.y > 0) h = Math.max(DeskStyle.webMin.height, from.height + dy)
            if (pull.y < 0) {
                h = Math.max(DeskStyle.webMin.height, from.height - dy)
                y = Math.max(0, from.y + from.height - h)
                h = from.y + from.height - y
            }
            win.box = Qt.rect(x, y, w, h)
        }
        onReleased: win.saveRequested()
    }
    Edge { id: edgeL; sx: -1 }
    Edge { id: edgeR; sx: 1 }
    Edge { id: edgeT; sy: -1 }
    Edge { id: edgeB; sy: 1 }

    // нажатие в окне — окно наверх (и дальше — тому, на что нажали)
    MouseArea {
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.AllButtons
        Component.onCompleted: cursorShape = undefined   // без формы: MouseArea ставит стрелку сама и закрыла бы формы под собой
        onPressed: (m) => { win.activate(); m.accepted = false }
    }
}
