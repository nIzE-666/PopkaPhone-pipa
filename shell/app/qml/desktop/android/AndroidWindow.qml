// Окно Android-приложения на столе (docs/ANDROID.md). Waydroid в режиме «окно на приложение» даёт на каждую задачу
// Android окно Wayland размером со весь экран Android: фон прозрачный и мимо ввода, а слои приложения — подповерхности
// там, где задача лежит на экране Android. Экран Android — это область стола под панелью пиксель в пиксель (размер
// ему задаёт стол, Desktop.qml), поэтому окно Waydroid лежит на всю эту область как есть: приложение — там, где его
// держит Android, со своей полосой Android (назад, свернуть, развернуть, закрыть), двигают и тянут его средствами
// Android. Стол — только порядок окон и фокус: нажали в приложение — оно наверх и с клавиатурой.
//   Поверхность — простым элементом Qt Wayland (AndroidSurface — WaylandQuickItem, держащий картинку, пока Waydroid
//   меняет буферы), без ShellSurfaceItem: его обработка xdg («развернуть» по выводу) меняла бы экран Android сама. Qt Wayland меряет поверхность в точках окна стола (кадр телефона: 1,833 пикселя в точке),
//   а стол рисует 1,5 (naomiDesk.scale) — масштаб элемента это выравнивает.
import QtQuick
import QtQuick.Shapes
import QtWayland.Compositor
import Naomi.Desk
import ".."

FocusScope {
    id: win
    property var toplevel
    property var xdgSurface
    property Item area                                    // стол без панели: видимая часть экрана Android
    // скрытая полоса экрана Android сверху — под панелью стола (Desktop.qml, androidHiddenPx): точки и пиксели
    property real hidden: 0
    property int hiddenPx: 0
    readonly property bool panelMode: mover.panel         // на весь экран: кнопки — на панели стола (Desktop.qml)
    readonly property bool android: true                  // для стола: не окно «Файлов»
    readonly property string appId: toplevel ? toplevel.appId : ""
    readonly property string pkg: appId.startsWith("waydroid.") ? appId.substring(9) : appId
    property bool active: false
    property bool minimized: false
    property bool closing: false
    readonly property bool parked: false
    property bool shown: false                            // Android положил слои — окно на виду
    property bool closeAsked: false                       // закрыли сами (Shift + крестик) — с панели прочь
    // Android спал, пока это окно жило (Desktop.qml, «Сон Android»): картинка в нём — до сна. Проснувшись, Waydroid
    // создаёт окна заново — показываем уже новое, а это не показываем, чтобы не мигнуло старым кадром
    property bool stale: false
    readonly property rect frame: mover.window            // окно Android на столе (точки от окна, у всех окон — одно начало)
    signal closed()
    // Окно без картинки: Waydroid кладёт слои сразу, как создал окно (доли секунды). 12 с без них, пока Android не
    // заморожен, — его служба вывода ждёт ответа Wayland, которого нет, и за ней стоит весь Android (замечено 18 ноября
    // 2026 после перезагрузки телефона: Google Play «висит на загрузке», docs/ANDROID.md, «Подводные камни»). Стол
    // перезапускает Android (naomiAndroid.restartStuck)
    signal stuck()
    Timer {
        interval: 12000
        running: !win.shown && !win.closing && !naomiAndroid.frozen && naomiAndroid.state === "on"
        onTriggered: win.stuck()
    }
    y: -hidden
    width: area ? area.width : 0
    height: area ? area.height + hidden : 0

    function takeFocus() { if (app.surface) app.takeFocus() }
    function togglePanel() { mover.togglePanel() }
    // крестик — на полосе окна и на панели стола: щелчок — свернуть (приложение живёт), с Shift — закрыть (xdg close:
    // Waydroid убирает задачу Android), как у браузера
    function closeButton(mods) {
        if (mods & Qt.ShiftModifier) {
            closeAsked = true
            toplevel.sendClose()
        } else {
            minimized = true
        }
    }
    // перетаскивание файлов из «Файлов» — Android не принимает
    function dragOver(p) {}
    function dropAt(p, paths) {}

    // Нажали в приложение — стол решает, это ли окно сверху в этой точке (pressedAt → Desktop.qml), и тогда — окно
    // наверх, клавиатура — окну Waydroid. Не слою: Qt Wayland по нажатию отдаёт фокус элементу под мышью
    // (подповерхности слоя), а Waydroid понимает фокус только на самом окне — по нему делает задачу Android активной
    // (без этого полоса Android бледнела, будто окно не в фокусе). Поэтому у элементов слоёв фокус по нажатию
    // выключен, а нажатие ловит PointHandler — пассивно: событие идёт слою дальше, но его видят и тогда, когда нажали
    // в окно стола над слоем, — отсюда проверка «сверху ли» у стола.
    signal pressedAt(point p)
    // слой приложения под точкой (в координатах окна; верхний) или null: для стола (windowAt) окно Android — только
    // его слои
    function layerAt(p) {
        const q = mapToItem(app, p.x, p.y)
        let top = null
        for (const c of app.children)
            if (c.surface && c.visible && q.x >= c.x && q.y >= c.y && q.x < c.x + c.width && q.y < c.y + c.height)
                top = c   // дети — в порядке слоёв снизу вверх
        return top
    }
    function hits(p) { return layerAt(p) !== null || caption.visible && caption.contains(mapToItem(caption, p.x, p.y)) }
    Component {
        id: pressWatch
        PointHandler {
            acceptedButtons: Qt.AllButtons
            onActiveChanged: if (active) win.pressedAt(win.mapFromItem(parent, point.position.x, point.position.y))
        }
    }
    property var watched: []
    function watchLayers() {
        for (const c of app.children) {
            if (!c.surface || watched.includes(c))
                continue
            c.focusOnClick = false
            pressWatch.createObject(c)
            watched.push(c)
        }
        watched = watched.filter(c => app.children.includes(c))
    }

    visible: opacity > 0
    // mover.fitting — Android открыл окно больше видимой области, стол его переставляет: показать уже на месте
    readonly property bool onView: shown && !minimized && !stale && !mover.fitting
    opacity: onView ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: win.onView ? 180 : 140; easing.type: Easing.OutCubic } }

    // «Развернуть», «вернуть» и «на весь экран» — с движением (docs/ANDROID.md, «Развернуть — с движением»). Android
    // перекладывает приложение в новый размер за 0,13–0,4 с, и всё это время рамка стояла уже новая, а картинка в ней —
    // старая, в углу. Теперь: окно снимает свою картинку (пока рамка и обрезка слоёв прежние — на это два кадра стола),
    // потом рамка едет от прежнего места к новому 200 мс со снимком, растянутым по ней, вместо слоёв Android, а когда
    // Android встал в новый размер, снимок растворяется в новой картинке
    property rect zoomFrom
    property real zoomT: 1
    property bool zoomWait: false   // снимок вместо слоёв: снимают, рамка едет или ждёт Android
    function zoomMix(a, b) { return a + (b - a) * zoomT }
    readonly property rect shownFrame: zoomT < 1
        ? Qt.rect(zoomMix(zoomFrom.x, mover.window.x), zoomMix(zoomFrom.y, mover.window.y),
                  zoomMix(zoomFrom.width, mover.window.width), zoomMix(zoomFrom.height, mover.window.height))
        : mover.window
    NumberAnimation {
        id: zoomAnim
        target: win; property: "zoomT"; from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic
        onRunningChanged: win.zoomCheck()
    }
    FrameAnimation {   // два кадра стола на снимок — и вперёд
        id: zoomGrab
        property int frames: 0
        onTriggered: {
            if (++frames < 2)
                return
            stop()
            mover.zoomNow()
            zoomAnim.restart()
        }
    }
    Connections {
        target: mover
        function onZoomAsked() {
            if (!win.onView || mover.window.width <= 0) {   // окна не видно — без движения
                mover.zoomNow()
            } else if (win.zoomWait) {   // прошлое движение не кончилось — с того места, где рамка сейчас, с тем же снимком
                if (zoomGrab.running)
                    return
                win.zoomFrom = win.shownFrame
                mover.zoomNow()
                zoomAnim.restart()
            } else {
                win.zoomFrom = mover.window
                win.zoomT = 0
                snapFade.stop()
                snap.opacity = 1
                snap.sourceRect = mover.window
                snap.sourceItem = mover
                snap.hideSource = true
                snap.scheduleUpdate()
                win.zoomWait = true
                zoomGrab.frames = 0
                zoomGrab.start()
            }
        }
        function onResizingChanged() { win.zoomCheck() }
    }
    // рамка доехала и Android встал — слои Android снова на виду, уже в новом размере, снимок растворяется
    function zoomCheck() {
        if (!zoomWait || zoomGrab.running || zoomAnim.running || mover.resizing)
            return
        zoomWait = false
        zoomT = 1
        snap.hideSource = false
        snapFade.restart()
    }
    NumberAnimation {
        id: snapFade
        target: snap; property: "opacity"; to: 0; duration: 120
        onFinished: snap.sourceItem = null
    }
    ShaderEffectSource {
        id: snap
        z: 40   // над слоями Android, под полосой окна
        visible: sourceItem !== null
        x: win.shownFrame.x; y: win.shownFrame.y
        width: win.shownFrame.width; height: win.shownFrame.height
        live: false
        textureSize: Qt.size(Math.round(sourceRect.width * naomiDesk.scale), Math.round(sourceRect.height * naomiDesk.scale))
    }

    // Колесо над другим окном Android (жест, android_scroll.h) увело туда фокус ввода Android — вернуть этому окну,
    // если оно на столе активное
    Connections {
        target: naomiAndroid
        function onScrollEnded(layer) {
            if (win.active && !win.minimized && !app.children.includes(layer))
                naomiAndroid.refocus(app)
        }
    }

    // Android свернул окно своей кнопкой — прячем; вернуть — из списка «03 ANDROID»
    Connections {
        target: win.toplevel
        function onSetMinimized() { win.minimized = true }
    }

    // Подложка: окно приложения непрозрачно, а слои Android — нет. Где слоя не оказалось (плеер оставил «дыру» под
    // видео, а декодер не запустился — 19 ноября 2026: чёрные полосы и окно стола сквозь середину), видно чёрное, как у
    // самого Android, а не то, что под окном. Заодно стол не рисует под окном обои: непрозрачное их закрывает. Без
    // угловых квадратов — там контур окна скруглён, а слои режутся по нему (углы закрывает картинка приложения)
    Item {
        visible: win.shown && caption.f.width > 0
        x: caption.f.x; y: caption.f.y
        width: caption.f.width; height: caption.f.height
        readonly property real inset: win.panelMode ? 0 : Math.ceil(win.corner * 1.6)
        Rectangle { x: parent.inset; width: parent.width - 2 * parent.inset; height: parent.height; color: "black" }
        Rectangle { y: parent.inset; width: parent.width; height: parent.height - 2 * parent.inset; color: "black" }
    }

    // Фон окна, пока тянут край (AndroidMove.resizing): рамка уже новая, а картинка Android ещё старая — где её нет,
    // окно тёмное, как у окон стола (DeskStyle.bg), а не прозрачное. Под картинкой Android: её не закрывает
    Item {
        visible: mover.resizing && caption.visible
        x: caption.f.x; y: caption.f.y
        width: caption.f.width; height: caption.f.height
        Shape {
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: DeskStyle.bg
                strokeColor: "transparent"
                PathPolyline { path: DeskStyle.outline(0, 0, caption.f.width, caption.f.height, win.corner) }
            }
        }
    }

    // Перетаскивание за полосу Android — столом (android_move.h): Android только начинает его, дальше окно двигает
    // стол в такт кадрам, как окна «Файлов»; сдвиг остаётся у окна
    AndroidMove {
        id: mover
        anchors.fill: parent
        surface: app
        pkg: win.pkg
        hiddenPx: win.hiddenPx
        tasks: naomiAndroid.tasks   // где окно на самом деле — говорит Android (docs/ANDROID.md, «Рамка окна»)
        onWantTasks: naomiAndroid.refreshTasks()
        onMoveTask: (from, to) => naomiAndroid.moveTask(win.pkg, from, to)
        onResizeTask: (from, to) => naomiAndroid.resizeTask(win.pkg, from, to)

        // поверхность — свой элемент: уничтоженный Waydroid буфер не гасит окно на кадр (android_surface.h)
        AndroidSurface {
            id: app
            property bool androidKeys: true   // клавиши — клавиатурой Wayland (stream_input.cpp)
            surface: win.xdgSurface ? win.xdgSurface.surface : null
            focusOnClick: true   // нажатие в рамку Android вокруг слоёв (за неё тянут размер) — фокус окну
            // пикселей Android в единице элемента (Qt Wayland делит на плотность окна) → в точке стола
            readonly property real px: surface && width > 0 ? surface.destinationSize.width / width : 1
            scale: px / naomiDesk.scale
            transformOrigin: Item.TopLeft
            onSurfaceDestroyed: win.closed()
            // слои пришли — показать (до этого окно прозрачно: у Waydroid сначала пустой фон)
            onChildrenChanged: {
                win.watchLayers()
                if (!win.shown && children.some(c => c.surface))
                    win.shown = true
            }
        }
    }

    // Полоса окна — своя, поверх полосы Android (docs/ANDROID.md, «Полоса окна и панель»): та у Waydroid рисуется в
    // самом окне приложения (DecorCaptionView, 64 пикселя) с «назад», «свернуть», «развернуть» и крестиком, убрать её
    // без пересборки Android нельзя. Здесь — как у окон стола: уголь, черта снизу, справа «на весь экран» (как у
    // браузера: окно — на всю область, кнопки — на панель стола) и крестик: щелчок — свернуть (окно уходит, приложение
    // живёт; вернуть — с панели или из «03 ANDROID»), с Shift — закрыть приложение (xdg close: Waydroid убирает задачу
    // Android), как у браузера. Полоса Android под ней нажатий не получает вовсе:
    // окно тащит стол (AndroidMove), Android переносит задачу, когда отпустили (docs/ANDROID.md, «Перетаскивание и
    // мерцание»)
    // Углы окна скруглены, как у окон стола (DeskStyle.corner): полоса — фигурой со скруглённым верхом, картинку Android
    // режет по контуру окна AndroidSurface (setOutline от AndroidMove); по контуру — волосяная рамка янтарём
    readonly property real corner: DeskStyle.corner(caption.f.width, caption.f.height)
    Item {
        id: caption
        z: 50
        readonly property rect f: win.shownFrame   // рамка окна; «развернуть» — в движении
        readonly property real h: Math.max(43, DeskStyle.webBarH)
        visible: win.shown && f.width > 0 && !win.panelMode   // на весь экран — без полосы (её кнопки — на панели)
        x: f.x; y: f.y
        width: f.width; height: h

        Shape {
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: DeskStyle.bg
                strokeColor: "transparent"
                PathPolyline { path: DeskStyle.outlineTop(0, 0, caption.width, caption.h, win.corner) }
            }
        }
        Rectangle {
            y: caption.h - 1
            width: parent.width; height: 1
            color: DeskStyle.amberA(DeskStyle.line)
        }
        MouseArea {   // нажали — окно наверх; тащат — стол; двойной щелчок — развернуть и обратно
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: (m) => {
                win.pressedAt(mapToItem(win, m.x, m.y))
                mover.press(mapToItem(mover, m.x, m.y))
            }
            onPositionChanged: (m) => mover.drag(mapToItem(mover, m.x, m.y))
            onReleased: mover.release()
            onCanceled: mover.release()
            onDoubleClicked: mover.toggleMaximized()
        }
        WinButton {   // на весь экран — как у браузера: кнопка переезжает на панель стола
            x: caption.width - 8 - 2 * width; y: (caption.h - height) / 2
            glyph: "panel"
            unit: 0.9545
            line: 1.5
            onClicked: win.togglePanel()
        }
        WinButton {
            x: caption.width - 6 - width; y: (caption.h - height) / 2
            glyph: "close"
            danger: true
            unit: 5.25 / 4.5
            line: 1.5
            onClicked: (mods) => win.closeButton(mods)
        }
    }

    // Края и углы — размер, как у окон стола (EdgeGrip: курсор ↔ ↕ ⤡ ⤢, 12 точек снаружи, 4 внутри): тянет стол
    // (AndroidMove.setPreview), Android получает новую рамку, когда отпустили (commitResize). Не меньше 220 точек —
    // меньше Android окно не делает (default_minimal_size_resizable_task, 220 dp); верх — не выше 4 точек (AndroidMove::keep)
    Item {
        id: frameBox
        z: 52
        visible: caption.visible && !mover.full
        x: caption.f.x; y: caption.f.y
        width: caption.f.width; height: caption.f.height
        component Edge: EdgeGrip {
            property rect start
            property point from
            onPressed: (m) => {
                win.pressedAt(mapToItem(win, m.x, m.y))
                start = Qt.rect(frameBox.x, frameBox.y, frameBox.width, frameBox.height)
                from = mapToItem(mover, m.x, m.y)
            }
            onPositionChanged: (m) => {
                if (!pressed)
                    return
                const p = mapToItem(mover, m.x, m.y)
                const dx = p.x - from.x, dy = p.y - from.y, min = 220
                let x = start.x, y = start.y, w = start.width, h = start.height
                if (pull.x > 0) w = Math.max(min, start.width + dx)
                if (pull.y > 0) h = Math.max(min, start.height + dy)
                if (pull.x < 0) {
                    w = Math.max(min, start.width - dx)
                    x = start.x + start.width - w
                }
                if (pull.y < 0) {
                    y = Math.max(4, start.y + start.height - Math.max(min, start.height - dy))
                    h = start.y + start.height - y
                }
                mover.setPreview(Qt.rect(x, y, w, h))
            }
            onReleased: mover.commitResize()
            onCanceled: mover.commitResize()
        }
        Edge { sx: -1 }
        Edge { sx: 1 }
        Edge { sy: -1 }
        Edge { sy: 1 }
    }

    Item {   // рамка по контуру окна — поверх картинки Android, как у окон стола
        z: 51
        visible: caption.visible
        x: caption.f.x; y: caption.f.y
        width: caption.f.width; height: caption.f.height
        Shape {
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: "transparent"
                strokeColor: DeskStyle.amberA(DeskStyle.line)
                strokeWidth: 1
                PathPolyline { path: DeskStyle.outline(0.5, 0.5, caption.f.width - 1, caption.f.height - 1, win.corner - 0.5) }
            }
        }
    }

    // Колесо — жестом прокрутки (naomiAndroid.wheel, android_scroll.h): Android колесо понимает только целыми щелчками и
    // на каждый прыгает, а палец ведёт плавно. Только над слоями: мимо них (прозрачное поле окна) — колесо окну стола
    // ниже. WheelHandler столу колеса не получает (FilesWindow.qml) — MouseArea
    MouseArea {
        anchors.fill: parent
        z: 100
        acceptedButtons: Qt.NoButton
        Component.onCompleted: cursorShape = undefined   // без формы: курсор над Android — его собственный
        onWheel: (e) => {
            const layer = win.layerAt(Qt.point(e.x, e.y))
            if (!layer) {
                e.accepted = false
                return
            }
            const dx = e.pixelDelta.x !== 0 ? e.pixelDelta.x : e.angleDelta.x / 120 * 100
            const dy = e.pixelDelta.y !== 0 ? e.pixelDelta.y : e.angleDelta.y / 120 * 100
            const a = mapToItem(layer, e.x, e.y), b = mapToItem(layer, e.x + dx, e.y + dy)
            naomiAndroid.wheel(layer, a, Qt.point(b.x - a.x, b.y - a.y), e.phase === Qt.ScrollEnd)
        }
    }
}
