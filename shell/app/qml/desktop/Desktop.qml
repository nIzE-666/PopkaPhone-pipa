// Рабочий стол НаомиФона — второй экран оболочки, только для трансляции на Mac (docs/DESKTOP.md): его видно в
// окне NaomiPhone Mirror, а телефон тем временем живёт своим интерфейсом. Вид рабочего стола НаомиОС — обои, панель
// сверху (N, FILE, звук, Bluetooth, интернет, раскладка, часы), меню под N. Работают «Файлы» (docs/FILES.md): окна под
// панелью, сверху — то, в котором щёлкнули; перетаскивание файлов — между окнами (значок с числом у мыши).
// Ширина — 1280 точек (кадр 1920), высота — под окно на Mac, от 720 до 960 («Форма стола»); мышь и клавиатура — с Mac.
import QtQuick
import QtCore
import Naomi.Ui as Ui
import "android"
import "files"
import "game"
import "preview"
import "settings"
import "term"
import "web"

Item {
    id: desk
    width: 1280
    height: 720
    x: DeskStyle.tablet && parent ? (parent.width - width) / 2 : 0
    y: DeskStyle.tablet && parent ? (parent.height - height) / 2 : 0
    rotation: DeskStyle.tablet ? naomiDisplay.sceneRotation : 0
    transformOrigin: Item.Center

    readonly property bool watchOrientation: DeskStyle.tablet && naomiPhone.available
                                             && naomiPower.displayOn && naomiDisplay.autoRotate
    onWatchOrientationChanged: {
        if (DeskStyle.tablet && naomiPhone.available)
            naomiPhone.sensors.call("Watch", [watchOrientation ? ["orientation"] : []])
    }
    readonly property int sensedRotation: {
        if (!watchOrientation)
            return -1
        switch (naomiPhone.sensors.orientation) {
        case "top-up": return 0
        case "right-up": return 90
        case "bottom-up": return 180
        case "left-up": return 270
        default: return -1
        }
    }
    onSensedRotationChanged: {
        // Датчик отсекает диагонали и плоские положения; ждём устойчивого положения.
        if (sensedRotation < 0 || sensedRotation === rotation)
            rotateDelay.stop()
        else
            rotateDelay.restart()
    }
    Timer {
        id: rotateDelay
        interval: 450
        onTriggered: naomiDisplay.sceneRotation = desk.sensedRotation
    }

    property bool keyboardBuilt: false
    Connections {
        target: DeskStyle.tablet ? naomiKeyboard : null
        function onVisibleChanged() {
            if (naomiKeyboard && naomiKeyboard.visible)
                desk.keyboardBuilt = true
        }
    }
    function isDescendant(item, ancestor) {
        for (let p = item; p; p = p.parent)
            if (p === ancestor)
                return true
        return false
    }
    function ensureWindowFocus() {
        if (naomiKeyboard && naomiKeyboard.inputEnabled)
            return
        const active = windows.children.find(w => w.active && !w.minimized && !w.closing && !w.parked)
        if (!active || typeof active.takeFocus !== "function")
            return
        const focused = desk.Window.activeFocusItem
        if (focused && isDescendant(focused, active))
            return
        if (active === webWindow && web.focused)
            return
        if (active === termWindow && term.renaming >= 0)
            return
        active.takeFocus()
    }
    function toggleKeyboard() {
        if (!naomiKeyboard)
            return
        if (naomiKeyboard.visible) {
            Qt.inputMethod.hide()
            return
        }
        ensureWindowFocus()
        naomiKeyboard.showManual()
    }
    Loader {
        id: keyboardLoader
        active: DeskStyle.tablet && desk.keyboardBuilt
        z: 3000
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(900, parent.width)
        sourceComponent: Item {
            Ui.KeyboardPanel {
                tablet: true
                visible: naomiPower.displayOn && visibleHeight > 0
                readonly property rect windowRect: desk.rotation >= 0 && visible
                                                   ? parent.mapToItem(null, Qt.rect(x, y, width, visibleHeight))
                                                   : Qt.rect(0, 0, 0, 0)
                onWindowRectChanged: if (naomiKeyboard)
                    naomiKeyboard.setRect(windowRect.x, windowRect.y, windowRect.width, windowRect.height)
            }
        }
    }

    // обои: ~/.config/naomi/desktop/wallpaper.jpg — копия выбранной в «Настройках» → «Обои» («Сделать фоном» в «Файлах»,
    // tools/naomi desktop-wallpaper); «Тёмные» — без картинки, этот градиент
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: DeskStyle.tablet ? DeskStyle.surface : "#1b1424" }
            GradientStop { position: 1; color: DeskStyle.tablet ? DeskStyle.bg : "#08070b" }
        }
    }
    Image {
        anchors.fill: parent
        visible: status === Image.Ready
        source: naomiFiles.desktopWallpaper
        fillMode: Image.PreserveAspectCrop   // как в bcdesk: вписать с обрезкой
        sourceSize.width: width * naomiDesk.scale
        asynchronous: true
        cache: false
    }

    // ---------- экран без дока у приставки (docs/PI.md, «Без дока») ----------
    // Выход из дока (плитка «трансляция», «закрыть сессию» в меню N) — панель и окна уходят, как закрывается меню N:
    // сжимаются к верху и тают за 140 мс; остаются обои, на них приставка в те же обновления монитора проявляет свои
    // часы. Вход — возвращаются, как оно открывается (180 мс). Сеанс приставки начинается сразу без них
    // (naomiStream.awayAnimated false): её первый кадр — те же обои, что у неё на экране.
    property real chromeIn: naomiStream.deskAway ? 0 : 1
    Behavior on chromeIn {
        enabled: naomiStream.awayAnimated
        NumberAnimation {
            duration: naomiStream.deskAway ? 140 : 180
            easing.type: Easing.OutCubic
            onRunningChanged: if (!running && naomiStream.deskAway) naomiStream.awaySettled()
        }
    }
    Connections {
        target: naomiStream
        function onDeskAwayChanged() {
            if (!naomiStream.deskAway)
                return
            bar.menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.batteryOpen = false
            desk.clipOpen = false
            volumeOsd.hideNow()
        }
    }

    // ---------- окна ----------
    function fitWindowBox(box) {
        const w = Math.min(box.width, windows.width), h = Math.min(box.height, windows.height)
        return Qt.rect(Math.max(0, Math.min(box.x, windows.width - w)),
                       Math.max(0, Math.min(box.y, windows.height - h)), w, h)
    }
    function fitOpenWindows() {
        for (const w of windows.children) {
            if (w.android || w.parked || w.closing)
                continue
            if (w === webWindow || w === termWindow || w.game) {
                w.box = fitWindowBox(w.box)
                continue
            }
            if (w.maximized)
                w.normal = fitWindowBox(w.normal)
            const box = w.maximized ? Qt.rect(0, 0, windows.width, windows.height)
                                    : fitWindowBox(Qt.rect(w.x, w.y, w.width, w.height))
            w.x = box.x
            w.y = box.y
            if (w !== settingsWindow && w !== padKeysWindow) {
                w.width = box.width
                w.height = box.height
            }
        }
    }
    Item {
        id: windows
        z: naomiWeb.fullscreen && desk.webShown ? 1010 : 0   // видео на весь экран — поверх панели
        y: DeskStyle.panelH
        width: parent.width
        height: parent.height - y
        opacity: desk.chromeIn
        scale: 0.96 + 0.04 * desk.chromeIn
        transformOrigin: Item.Top
        onWidthChanged: if (DeskStyle.tablet) Qt.callLater(desk.fitOpenWindows)
        // стол сменил высоту (форма окна на Mac, docs/DESKTOP.md): развёрнутые — во всю новую высоту, остальные — не
        // ниже края (браузер в режиме панели и развёрнутый подстраивается сам)
        onHeightChanged: {
            if (DeskStyle.tablet) {
                Qt.callLater(desk.fitOpenWindows)
                return
            }
            for (const w of children) {
                if (w.android) {
                    continue   // окно Android — на всю область само, экран Android — вслед (androidSize)
                } else if (w === desk.settingsWindow || w === desk.padKeysWindow) {
                    continue   // высота — своя и не выше стола (SettingsWindow)
                } else if (w === desk.webWindow || w === desk.termWindow || w.game) {
                    const b = w.box, h = Math.min(b.height, height)
                    if (b.y + b.height > height)
                        w.box = Qt.rect(b.x, Math.max(0, height - h), b.width, h)
                } else if (w.maximized) {
                    w.width = width
                    w.height = height
                } else if (w.y + w.height > height) {
                    w.height = Math.min(w.height, height)
                    w.y = Math.max(0, height - w.height)
                }
            }
        }
    }
    property int topZ: 0
    property int pinZ: 50000
    readonly property var filesWindows: {
        windowsTick   // пересчёт, когда окна пришли, ушли, свернулись
        return windows.children.filter(w => !w.closing && !w.parked && !w.android && w !== webWindow && w !== settingsWindow
                                            && w !== termWindow && !w.game && w !== padKeysWindow)
    }
    property int windowsTick: 0
    readonly property real filesOpen: {   // для черты под FILE: 1 — на виду, 0,5 — все свёрнуты, 0 — окон нет
        const ws = filesWindows
        return !ws.length ? 0 : ws.some(w => !w.minimized) ? 1 : 0.5
    }
    readonly property bool filesFocused: filesWindows.some(w => w.active && !w.minimized)
    Component { id: filesWindow; FilesWindow {} }

    // Окно «Файлов» открывается там, где закрыли прошлое (просьба пользователя 17 ноября 2026): место, размер и
    // «развёрнуто» — в ~/.config/naomi/desktop/windows.conf, переживают и перезапуск. Другие окна «Файлов» уже открыты —
    // новое лесенкой от этого места (+28, у края стола — −28), чтобы не легло ровно поверх; не влезает (стол другой
    // ширины) — ужимается и сдвигается внутрь.
    Settings {
        id: filesPlace
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/windows.conf"
        category: "files"
        property int x: 120
        property int y: 24
        property int width: 1000
        property int height: 600
        property bool maximized: false
    }
    function filesRect(step) {
        const w = Math.min(Math.max(filesPlace.width, 520), windows.width)
        const h = Math.min(Math.max(filesPlace.height, 340), windows.height)
        const fit = (v, max) => Math.max(0, Math.min(v, max))
        const x = fit(filesPlace.x, windows.width - w), y = fit(filesPlace.y, windows.height - h)
        return Qt.rect(fit(x + step * 28, windows.width - w), fit(y + step * 28, windows.height - h), w, h)
    }
    function openFiles(location) {
        const others = filesWindows
        const taken = r => others.some(o => !o.maximized && Math.abs(o.x - r.x) < 8 && Math.abs(o.y - r.y) < 8)
        let r = filesRect(0)
        for (const k of [1, 2, 3, 4, 5, -1, -2, -3, -4, -5]) {
            if (!taken(r))
                break
            r = filesRect(k)
        }
        const max = filesPlace.maximized && !others.length
        if (!filesSpare && spareIncubator)
            spareIncubator.forceCompletion()   // запас почти собран — досоздать, а не собирать второе окно
        let w = filesSpare
        if (w) {
            filesSpare = null
            w.freshen(location || naomiFiles.home)
            if (max) {
                w.normal = r
                w.maximized = true
                w.x = 0; w.y = 0; w.width = windows.width; w.height = windows.height
            } else {
                w.x = r.x; w.y = r.y; w.width = r.width; w.height = r.height
            }
            w.unpark()
        } else {
            w = filesWindow.createObject(windows, {
                desk: desk, area: windows, start: location || naomiFiles.home,
                x: r.x, y: r.y, width: r.width, height: r.height })
            if (max)
                w.toggleMaximize()
        }
        w.activated.connect(() => raise(w))
        w.openWindow.connect(openFiles)
        w.closed.connect(() => {
            if (desk.preview && desk.preview.source === w)
                desk.preview.detachSource()   // «Просмотр» дальше листает папку сам
            const n = w.maximized ? w.normal : Qt.rect(w.x, w.y, w.width, w.height)
            filesPlace.x = Math.round(n.x)
            filesPlace.y = Math.round(n.y)
            filesPlace.width = Math.round(n.width)
            filesPlace.height = Math.round(n.height)
            filesPlace.maximized = w.maximized
            w.destroy()
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            fitSpare()
            makeFilesSpare()   // окно ушло (анимация кончилась, его не тащат) — запас сразу: «открыл снова» бывает скоро
        })
        w.minimizedChanged.connect(() => {
            ++windowsTick
            if (w.minimized && w.active) {
                w.active = false
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            }
        })
        ++windowsTick
        raise(w)
        spareTimer.restart()
        return w
    }

    // Запас «Файлов» (замер 17 ноября 2026, приставка): окно «Файлов» создаётся ~75 мс и первый кадр с ним — ещё
    // ~25 мс; всё это в потоке интерфейса, стол стоит — анимация появления начиналась с середины, а приставка, получив
    // кадр через ~170 мс после такта, поднимала опережение до 80 мс и, пока оно сходило, выбрасывала кадры (рывки,
    // если окно сразу тащат). Поэтому одно окно собирается заранее — по частям (асинхронно): сразу, как закрыли окно
    // «Файлов», а после появления стола и после того, как запас взяли, — когда стол в покое (не рисовал 0,5 с; не
    // раньше 1,5 с: окно тащат — сборка ждёт; покоя нет 9 с — видео в браузере — всё равно), — и ждёт за краем стола
    // прозрачным (DeskWindow, parked): разложено, узлы сцены готовы. FILE, плитка FILES, «Открыть в новом окне» берут
    // его — окно встаёт на место и появляется без задержки. Размер — как у следующего окна: место сменилось (закрыли
    // окно) — запас подгоняется сразу.
    property Item filesSpare: null
    property var spareIncubator: null
    property int spareWaits: 0
    Timer {
        id: spareTimer
        interval: 1500
        onTriggered: {
            if (naomiDesk.idleMs() < 500 && desk.spareWaits < 5) {
                ++desk.spareWaits
                restart()
                return
            }
            desk.spareWaits = 0
            desk.makeFilesSpare()
        }
    }
    function spareSize() {
        const r = filesRect(0)
        return filesPlace.maximized ? Qt.size(windows.width, windows.height) : Qt.size(r.width, r.height)
    }
    function makeFilesSpare() {
        if (DeskStyle.tablet || filesSpare || spareIncubator)
            return
        const s = spareSize()
        const inc = filesWindow.incubateObject(windows, {
            desk: desk, area: windows, start: naomiFiles.home, parked: true,
            x: -100000, y: 0, width: s.width, height: s.height }, Qt.Asynchronous)
        const done = () => {
            if (inc.status === Component.Ready) {
                filesSpare = inc.object
                filesSpare.warm = true   // один кадр почти прозрачным (DeskWindow, warm)
                spareWarm.restart()
            } else {
                console.warn("стол: запас «Файлов» не собрался")
            }
            spareIncubator = null
        }
        if (inc.status !== Component.Loading) {
            done()
            return
        }
        spareIncubator = inc
        inc.onStatusChanged = () => done()
    }
    Timer { id: spareWarm; interval: 150; onTriggered: if (desk.filesSpare) desk.filesSpare.warm = false }
    function fitSpare() {   // окно закрыли: место, вид и порядок могли смениться — запас под них, пока ничего не открыто
        if (!filesSpare)
            return
        const s = spareSize()
        filesSpare.width = s.width
        filesSpare.height = s.height
        filesSpare.freshen(naomiFiles.home)
    }
    function raise(w) {
        for (const o of windows.children)
            o.active = o === w
        w.minimized = false
        w.z = w.pinned ? ++pinZ : ++topZ
    }
    // FILE на панели и плитка FILES — как в НаомиОС: окон нет — открыть; на виду и в фокусе — свернуть все;
    // иначе — показать все, последнее сверху
    function activateFiles() {
        const ws = filesWindows
        if (!ws.length) {
            openFiles()
            return
        }
        if (filesFocused) {
            for (const w of ws)
                w.minimized = true
            return
        }
        ws.slice().sort((a, b) => a.z - b.z).forEach(w => raise(w))
    }

    // ---------- «Настройки» (docs/DESKTOP.md, «Настройки») ----------
    // Окно одно: шестерёнка в меню N — открыть или поднять. Мышь и экран — в ~/.config/naomi/desktop/mouse.conf и
    // screen.conf; телефон их только хранит: курсор, колесо, размер стола, частоту монитора и экран без дока ведёт
    // зритель со своими мышью и монитором (приставка) — ему сообщениями Mouse и Screen (naomiStream.mouse, .screen).
    // По умолчанию — как было до настроек.
    Settings {
        id: mouseSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/mouse.conf"
        category: "mouse"
        property int speed: 50          // скорость указателя, %
        property int accel: 1           // ускорение: 0 — нет, 1 — мягкое, 2 — сильное
        property bool natural: false    // колесо обратное (к себе — страница вверх)
        property int lines: 3           // строк за щелчок (3 — 100 точек стола)
        property bool smooth: true      // плавная прокрутка
        property int soft: 130          // мягкость: мс до 95 % пути щелчка
        property bool wheelAccel: false // разгон колеса
    }
    // зрителю — когда стол готов: Settings читает файл при завершении создания, раньше в привязке — значения по
    // умолчанию, и приставка на миг получала их (экран — лишний перезапуск браузера стола)
    property bool settingsLoaded: false   // ставит Component.onCompleted (ниже, у звука)
    Binding {   // delayed: набор меняет несколько значений сразу — зрителю одним сообщением
        target: naomiStream
        property: "mouse"
        when: desk.settingsLoaded
        delayed: true
        value: ({ speed: mouseSettings.speed, accel: mouseSettings.accel, natural: mouseSettings.natural,
                  lines: mouseSettings.lines, smooth: mouseSettings.smooth, soft: mouseSettings.soft,
                  wheelAccel: mouseSettings.wheelAccel })
    }
    // Трекпад приставки («Настройки» → «Трекпад»; docs/PI.md, «Трекпад как на Mac»): ~/.config/naomi/desktop/
    // trackpad.conf, зрителю — сообщением Pad (naomiStream.trackpad); по умолчанию — как трекпад на Mac пользователя.
    // НОВОЕ — ТОЛЬКО В КОНЕЦ (см. ниже)
    Settings {
        id: touchSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/trackpad.conf"
        category: "trackpad"
        property int speed: 50          // указатель, %: 50 — ползунок скорости Mac пользователя
        property int accel: 1           // ускорение: 0 — нет, 1 — кривая Mac, 2 — сильнее
        property bool tap: true         // касание — щелчок
        property int right: 1           // правый щелчок: 0 — нет, 1 — два пальца, 2 — правый нижний угол
        property int drag: 1            // перетаскивание: 0 — нажатием, 1 — три пальца, 2 — двойное касание
        property int hold: 700          // три пальца сняли — кнопка держится, мс
        property bool natural: true     // прокрутка: страница за пальцами
        property int sspeed: 50         // скорость прокрутки, %
        property int inertia: 1         // инерция: 0 — нет, 1 — обычная, 2 — длинная
        property bool pinch: true       // щипок — масштаб
        property int jitter: 1          // дрожь пальца: 0 — гасить слабо, 1 — средне, 2 — сильно
        property bool palm: true        // лежащий палец и ладонь не считать
        property bool typing: true      // пауза при печати
        property bool pin: true         // курсор замирает на щелчке
        property int swSide: 1          // смахнуть вбок: 0 — ничего, 1 — «назад» и «вперёд», 2 — вкладки
        property int swUp: 1            // вверх: 0 — ничего, 1 — меню N
        property int swDown: 0          // вниз: то же
    }
    Binding {   // show — раздел «Трекпад» открыт: приставка шлёт пальцы «Проверке» (naomiStream.padTouch)
        target: naomiStream
        property: "trackpad"
        when: desk.settingsLoaded
        delayed: true
        value: ({ speed: touchSettings.speed, accel: touchSettings.accel, tap: touchSettings.tap, right: touchSettings.right,
                  drag: touchSettings.drag, hold: touchSettings.hold, natural: touchSettings.natural,
                  sspeed: touchSettings.sspeed, inertia: touchSettings.inertia, pinch: touchSettings.pinch,
                  jitter: touchSettings.jitter, palm: touchSettings.palm, typing: touchSettings.typing,
                  pin: touchSettings.pin, swSide: touchSettings.swSide, swUp: touchSettings.swUp,
                  swDown: touchSettings.swDown,
                  show: desk.settingsWindow !== null && desk.settingsWindow.page === "touch" })
    }
    Connections {   // смахивание трекпада просит действие стола: 1 — меню N
        target: naomiStream
        function onPadAction(action) {
            if (action === 1 && !naomiStream.deskAway)
                bar.menuOpen = !bar.menuOpen
        }
    }
    // НОВОЕ — ТОЛЬКО В КОНЕЦ (обоих Settings): Qt, читая файл, на свойстве, которого в нём ещё нет, сразу снимает все
    // значения для записи — а свойства ниже ещё не прочитаны, и в файл через 0,5 с ложатся их значения по умолчанию
    // (19 ноября 2026: exitAfter посередине сбросил пользователю приглушение, его долю и время сна)
    Settings {
        id: screenSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/screen.conf"
        category: "screen"
        property int deskWidth: 1600    // ширина стола, точки (высота — по пропорциям монитора)
        property int sharp: 2           // резкость: кадр — ¾, 27/32, 15/16 или вся ширина монитора (0…3)
        property int hz: 60             // частота монитора (ближайшая из его режимов)
        property bool clock: true       // без дока: часы на обоях
        property int clockX: 275        // где они: левый верх надписи, 1/10000 ширины монитора (44 точки стола 1600)
        property int clockY: 378        // и высоты (34 точки стола 900)
        property int sleep: 1800        // без дока: без клавиш столько, с, — монитор спит (0 — никогда)
        property bool dim: false        // или не спит, а приглушается (яркость по DDC/CI)
        property int dimLevel: 10       // до стольких % яркости монитора
        property bool wakeMove: false   // без дока: движение мыши будит монитор
        property string monitor: ""     // монитор приставки (JSON сообщения Monitor) — последний, что она называла
        property int exitAfter: 0       // стол без дела (мышь, клавиши, звук) столько, с, — уходит, как «закрыть сессию»
        property int touchScrollSpeed: 35   // скорость прокрутки пальцем в приложениях Linux на планшете, %
    }
    Settings {
        id: uiSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/interface.conf"
        category: "interface"
        property bool clock24h: true        // 24-часовой формат времени по умолчанию
        property bool clockSeconds: false   // секунды в часах панели
        property int dateFormat: 1          // 0 — скрыть, 1 — ВТ 24 НОЯ, 2 — 24.11, 3 — TUE 24
        property real nightFilter: 0.0      // тёплый ночной фильтр экрана (0..0.60)
        property int extraKeysMode: 0       // 0 — авто по физ. клавиатуре, 1 — всегда, 2 — скрыть
    }
    Binding {
        target: naomiStream
        property: "screen"
        when: desk.settingsLoaded
        delayed: true
        value: ({ width: screenSettings.deskWidth, sharp: screenSettings.sharp, hz: screenSettings.hz,
                  clock: screenSettings.clock, clockX: screenSettings.clockX, clockY: screenSettings.clockY,
                  exit: screenSettings.exitAfter,
                  sleep: screenSettings.sleep, dim: screenSettings.dim, dimLevel: screenSettings.dimLevel,
                  wakeMove: screenSettings.wakeMove })
    }
    // монитор приставки — в настройки, чтобы «Настройки» знали его и без неё; назвала до создания стола — сигнал
    // прошёл мимо, поэтому и при загрузке (settingsLoaded)
    function rememberMonitor() {
        if (naomiStream.monitor.w)
            screenSettings.monitor = JSON.stringify(naomiStream.monitor)
    }
    Connections {
        target: naomiStream
        function onMonitorChanged() { desk.rememberMonitor() }
    }
    property var settingsWindow: null
    Component { id: settingsWindowComponent; SettingsWindow {} }
    // Прогрев: разделы «Настроек» (SettingsWindow грузит их по требованию) — загрузчиком QML фоном, через 4 с после
    // появления стола. Без этого первое открытие после запуска оболочки стояло 82 мс, из них 47 — поток интерфейса
    // ждал, пока загрузчик прочтёт и соберёт файлы (выборки стека: pthread_cond_wait; прогон стола 19 ноября 2026)
    property var warmed: []
    Timer {
        interval: 4000
        running: !DeskStyle.tablet
        onTriggered: desk.warmed = ["ScreenPage", "WallPage", "LockPage", "MousePage", "MouseMorePage", "TrackpadPage",
                                    "BatteryPage", "HealthPage", "GamepadPage"]
            .map(n => Qt.createComponent("settings/" + n + ".qml", Component.Asynchronous))
    }
    function openSettings() {
        if (settingsWindow) {
            raise(settingsWindow)
            return
        }
        const w = settingsWindowComponent.createObject(windows, {
            cfg: mouseSettings, screen: screenSettings, pad: padSettings, touch: touchSettings,
            ui: uiSettings, term: termSettings, desk: desk, area: windows,
            y: 24 })
        w.x = Math.round((windows.width - w.width) / 2)   // ширина — по панели слева (её тянут, SettingsWindow)
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            w.destroy()
            desk.settingsWindow = null
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        settingsWindow = w
        ++windowsTick
        raise(w)
    }

    // ---------- браузер (docs/WEB.md) ----------
    // Программа naomi-web — отдельный процесс: её окно (страница) приходит от композитора (naomiApps.deskWindow),
    // полосу и режим панели рисует стол. WEB на панели: браузера нет — запустить; окно на виду и в фокусе —
    // свернуть; иначе — показать. Место окна, «развёрнуто» и режим панели — в ~/.config/naomi/desktop/web.conf.
    WebState {
        id: web
        onRaiseRequested: if (desk.webWindow) desk.raise(desk.webWindow)
        onBlurRequested: (refocus) => { if (refocus && desk.webWindow) desk.webWindow.takeFocus() }
        onTabMenu: (id) => {
            const t = web.tabs.find(o => o.id === id)
            if (!t)
                return
            webMenu.tabId = id
            webMenu.show(desk.pointer.x, desk.pointer.y, [
                { text: t.pinned ? "Открепить" : "Закрепить", tone: "on", action: "pin" },
                { text: "Дублировать", tone: "off", action: "duplicate" },
                { text: "Закрыть вкладку", tone: "bad", action: "close" }])
        }
    }
    Settings {
        id: webSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/web.conf"
        category: "web"
        property bool panel: false
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
    }
    // рамка по умолчанию — по центру свободной области, не шире 1600 и не выше 1040 (webframe_default_box)
    function webBox() {
        const r = webSettings.window, aw = windows.width, ah = windows.height
        if (r.width >= DeskStyle.webMin.width && r.height >= DeskStyle.webMin.height
                && r.x + r.width > 40 && r.x < aw - 40 && r.y >= 0 && r.y < ah - 40)
            return Qt.rect(r.x, r.y, Math.min(r.width, aw), Math.min(r.height, ah))
        const w = Math.min(aw - 64, 1600), h = Math.min(ah - 64, 1040)
        return Qt.rect(Math.round((aw - w) / 2), Math.round((ah - h) / 2), w, h)
    }
    property real webPid: -1
    property var webWindow: null
    readonly property real webOpen: {   // для черты под WEB: 1 — на виду, 0,5 — свёрнут, 0 — нет
        windowsTick
        return !webWindow ? 0 : webWindow.minimized ? 0.5 : 1
    }
    readonly property bool webShown: webOpen >= 1
    readonly property bool webFocused: webShown && webWindow.active
    // На весь экран — браузер и «Терминал» в режиме панели, окна Android «на весь экран», игра «в доке»: на панели —
    // кнопки верхнего из них («в окно» и крестик; у браузера и «Терминала» ещё вкладки)
    readonly property var panelTop: {
        windowsTick
        let top = null
        for (const w of windows.children) {
            const panel = w.android || w.game ? w.panelMode : w === webWindow ? webSettings.panel && !naomiWeb.fullscreen
                        : w === termWindow && termSettings.panel
            if (panel && !w.closing && !w.minimized && w.visible && (!top || w.z > top.z))
                top = w
        }
        return top
    }
    readonly property bool webPanel: webShown && webSettings.panel && !naomiWeb.fullscreen && panelTop === webWindow
    Component { id: webWindowComponent; WebWindow {} }
    function activateWeb() {
        if (!webWindow) {
            if (webPid < 0)
                webPid = naomiApps.launch("web", naomiDesk.scale)
            return
        }
        if (webFocused) {
            webWindow.minimized = true
            return
        }
        raise(webWindow)
        webWindow.takeFocus()
    }
    function setWebPanel(on) {
        if (web.focused)
            web.blurField(false)
        webSettings.panel = on
        if (webWindow) {
            raise(webWindow)
            webWindow.takeFocus()
        }
    }
    Connections {
        target: naomiApps
        function onDeskWindow(toplevel, xdgSurface) {
            const app = naomiApps.deskApp(xdgSurface.surface.client.processId)
            if (app === "game") {   // окно игры (naomi-game)
                desk.gameArrived(toplevel, xdgSurface)
                return
            }
            if (app === "win") {    // окно программы Windows (Wine, docs/WINDOWS.md)
                desk.winArrived(toplevel, xdgSurface)
                return
            }
            if (app === "claude" || app === "telegram" || app === "deepseek") {
                if ((app === "telegram" && desk.telegramPid > 0 && !desk.telegramWindows.length
                     || app === "deepseek" && desk.deepseekPid > 0 && !desk.deepseekWindows.length)
                    && !nmenu.forceOpen)
                    bar.menuOpen = false
                desk.linuxArrived(toplevel, xdgSurface, app)
                return
            }
            if (DeskStyle.tablet && app !== "web") {
                desk.linuxArrived(toplevel, xdgSurface, "linux")
                return
            }
            const w = webWindowComponent.createObject(windows, {
                st: web, area: windows, toplevel: toplevel, xdgSurface: xdgSurface, box: desk.webBox(),
                maximized: webSettings.maximized, panelMode: Qt.binding(() => webSettings.panel),
                bookmarksOpen: Qt.binding(() => bookmarks.open) })
            w.activated.connect(() => raise(w))
            w.closed.connect(() => { w.destroy(); desk.webWindow = null; ++windowsTick })
            w.panelToggled.connect(() => desk.setWebPanel(!webSettings.panel))
            w.saveRequested.connect(() => {
                webSettings.window = w.box
                webSettings.maximized = w.maximized
            })
            w.starClicked.connect((x, y) => bookmarks.open ? bookmarks.close() : bookmarks.show(x, y))
            w.downloadActivated.connect((d) => {
                if (d.state === "completed")
                    openFiles(d.path.substring(0, d.path.lastIndexOf("/")))
                naomiWeb.send({ type: "dismiss-download", id: d.id })
            })
            w.minimizedChanged.connect(() => {
                ++windowsTick
                if (w.minimized) {
                    if (web.focused)
                        web.blurField(false)
                    if (w.active) {
                        w.active = false
                        const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                        if (rest.length)
                            raise(rest.reduce((a, b) => a.z > b.z ? a : b))
                    }
                }
            })
            desk.webWindow = w
            ++windowsTick
            raise(w)
            w.takeFocus()
        }
        function onExited(pid, name, code) {
            if (pid === desk.gamePid) {
                desk.gameExited(code)
                return
            }
            if (pid === desk.claudePid) {
                desk.claudePid = -1
                return
            }
            if (pid === desk.telegramPid) {
                desk.telegramPid = -1
                if (code === 127)
                    nmenu.telegramError = "Telegram Desktop не установлен"
                else if (code !== 0 && !desk.telegramWindows.length)
                    nmenu.telegramError = "Не удалось запустить Telegram Desktop"
                return
            }
            if (pid === desk.deepseekPid) {
                desk.deepseekPid = -1
                if (code !== 0 && !desk.deepseekWindows.length)
                    nmenu.deepseekError = "Не удалось запустить DeepSeek"
                return
            }
            if (pid !== desk.webPid)
                return
            desk.webPid = -1
            if (desk.webRescale) {   // перезапуск под новый масштаб: вкладки он вернёт сам (session.json)
                desk.webRescale = false
                desk.webPid = naomiApps.launch("web", naomiDesk.scale)
            }
        }
    }
    // ---------- «Терминал» (docs/TERMINAL.md) ----------
    // Окно одно, вкладки внутри (naomiTerm). TERM на панели и плитка TERMINAL в меню N: окна нет — открыть; на виду и в
    // фокусе — свернуть; иначе — показать. Крестик — свернуть, с Shift — закрыть терминал: текст вкладок на диске,
    // оболочки закрыты, открыли снова — вкладки на месте. Место окна, «развёрнуто» и режим дока — в
    // ~/.config/naomi/desktop/term.conf (НОВОЕ — ТОЛЬКО В КОНЕЦ, как у screen.conf).
    TermState {
        id: term
        onRaiseRequested: if (desk.termWindow) {
            desk.raise(desk.termWindow)
            desk.termWindow.takeFocus()
        }
        onTabMenu: (id) => {
            const t = naomiTerm.tabs.find(o => o.id === id)
            if (!t)
                return
            termMenu.tabId = id
            termMenu.show(desk.pointer.x, desk.pointer.y, [
                { text: t.pinned ? "Открепить" : "Закрепить", tone: "on", action: "pin" },
                { text: "Переименовать", tone: "off", action: "rename" },
                { text: "Дублировать", tone: "off", action: "duplicate" },
                { text: "Закрыть вкладку", tone: "bad", action: "close" }])
        }
    }
    Settings {
        id: termSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/term.conf"
        category: "term"
        property bool panel: false
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
        property int fontSize: 15
    }
    // рамка по умолчанию — по центру свободной области, 960 × 600 (не больше её)
    function termBox() {
        const r = termSettings.window, aw = windows.width, ah = windows.height
        if (r.width >= 480 && r.height >= 280 && r.x + r.width > 40 && r.x < aw - 40 && r.y >= 0 && r.y < ah - 40)
            return Qt.rect(r.x, r.y, Math.min(r.width, aw), Math.min(r.height, ah))
        const w = Math.min(aw - 64, 960), h = Math.min(ah - 64, 600)
        return Qt.rect(Math.round((aw - w) / 2), Math.round((ah - h) / 2), w, h)
    }
    property var termWindow: null
    readonly property real termOpen: {   // для черты под TERM и TERMINAL: 1 — на виду, 0,5 — свёрнут, 0 — нет
        windowsTick
        return !termWindow || termWindow.closing ? 0 : termWindow.minimized ? 0.5 : 1
    }
    readonly property bool termFocused: termOpen >= 1 && termWindow.active
    readonly property bool termPanel: termOpen >= 1 && termSettings.panel && panelTop === termWindow
    Component { id: termWindowComponent; TermWindow {} }
    function activateTerm() {
        if (!termWindow || termWindow.closing) {
            openTerm()
            return
        }
        if (termFocused) {
            termWindow.minimized = true
            return
        }
        raise(termWindow)
        termWindow.takeFocus()
    }
    function openTerminalAt(dir) {
        if (!termWindow || termWindow.closing)
            openTerm()
        else {
            raise(termWindow)
            termWindow.takeFocus()
        }
        naomiTerm.newTab(-1, dir || "~")
    }
    function openTerm() {
        naomiTerm.start()
        const w = termWindowComponent.createObject(windows, {
            st: term, area: windows, box: desk.termBox(), maximized: termSettings.maximized,
            fontSize: Qt.binding(() => termSettings.fontSize),
            extraKeysMode: Qt.binding(() => uiSettings.extraKeysMode),
            panelMode: Qt.binding(() => termSettings.panel) })
        w.fontSizePicked.connect((sz) => { termSettings.fontSize = sz })
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            w.destroy()
            if (desk.termWindow === w)
                desk.termWindow = null
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        w.panelToggled.connect(() => desk.setTermPanel(!termSettings.panel))
        w.closeClicked.connect((mods) => {
            if (mods & Qt.ShiftModifier)
                desk.quitTerm()
            else
                w.minimized = true
        })
        w.saveRequested.connect(() => {
            termSettings.window = w.box
            termSettings.maximized = w.maximized
        })
        w.textMenu.connect(() => {
            termMenu.tabId = -1
            termMenu.show(desk.pointer.x, desk.pointer.y, [
                { text: "Копировать", tone: w.view.hasSelection ? "on" : "off", action: "copy" },
                { text: "Вставить", tone: "off", action: "paste" }])
        })
        w.minimizedChanged.connect(() => {
            ++windowsTick
            if (w.minimized && w.active) {
                w.active = false
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            }
        })
        termWindow = w
        ++windowsTick
        raise(w)
    }
    function quitTerm() {
        naomiTerm.quit()
        if (termWindow)
            termWindow.close()
        ++windowsTick
    }
    function setTermPanel(on) {
        termSettings.panel = on
        if (termWindow) {
            raise(termWindow)
            termWindow.takeFocus()
        }
    }
    Connections {   // последнюю вкладку закрыли (exit) — окна больше нет
        target: naomiTerm
        function onFinished() {
            term.renaming = -1
            if (desk.termWindow)
                desk.termWindow.close()
            ++windowsTick
        }
    }

    // ---------- «Игры» (docs/GAMES.md) ----------
    // Игра — программа naomi-game с ядром эмулятора (naomiGames.command), её окно приходит от композитора, как окно
    // браузера; полосу рисует стол (GameWindow). Игра одна: выбрали другую — первая закрывается (память картриджа
    // сохранит), вторая запускается следом. Выбрали ту, что идёт, — её окно наверх. Свернули — пауза. Место окна,
    // «развёрнуто» и режим дока — в ~/.config/naomi/desktop/game.conf (НОВОЕ — ТОЛЬКО В КОНЕЦ, как у screen.conf).
    Settings {
        id: gameSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/game.conf"
        category: "game"
        property bool panel: false
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
        property string last: ""   // последняя запущенная игра (путь) — «Последняя игра» у геймпада
    }
    readonly property string lastGame: gameSettings.last
    // рамка по умолчанию — по центру: картинка высотой до 720 точек (NES — пиксель 8 к 7, PS1 — экран 4 к 3) и полоса
    function gameBox(system) {
        const r = gameSettings.window, aw = windows.width, ah = windows.height
        if (r.width >= 320 && r.height >= 280 && r.x + r.width > 40 && r.x < aw - 40 && r.y >= 0 && r.y < ah - 40)
            return Qt.rect(r.x, r.y, Math.min(r.width, aw), Math.min(r.height, ah))
        const sh = Math.min(ah - 64 - DeskStyle.webBarH, 720), w = Math.min(aw - 64, Math.round(sh * (system === "nes" ? 256 / 240 * 8 / 7 : 4 / 3)))
        const h = sh + DeskStyle.webBarH
        return Qt.rect(Math.round((aw - w) / 2), Math.round((ah - h) / 2), w, h)
    }
    property real gamePid: -1
    property var gameNow: null      // идущая игра — строка naomiGames.games
    property var gameNext: null     // выбрана, пока прошлая закрывается
    property var gameWindow: null
    // Игра Windows (файл .win в ~/Games/Windows — её запускалка, docs/WINDOWS.md) — тоже «игра одна», но окна у неё
    // свои, окна Wine (winWindows): gameWindow пуст, на панели она — строкой своего окна
    readonly property bool gameIsWin: !!gameNow && gameNow.system === "win"
    property bool gameShown: false   // игра Windows показала хоть одно окно
    readonly property real gameOpen: {   // для черты под GAME и строки «03 GAMES»: 1 — на виду, 0,5 — свёрнута или запускается
        windowsTick
        if (gamePid >= 0 && gameIsWin)
            return winWindows.some(w => !w.minimized) ? 1 : 0.5
        return gamePid < 0 ? 0 : !gameWindow || gameWindow.minimized ? 0.5 : 1
    }
    readonly property var gamePanel: gameNow && gameWindow ? [{ pkg: "game", name: gameNow.name, open: gameOpen }] : []
    Component {
        id: gameWindowComponent
        GameWindow { touchScrollSpeed: DeskStyle.tablet ? screenSettings.touchScrollSpeed : 100 }
    }
    function activateGame(game) {
        bar.menuOpen = false
        if (gamePid >= 0 && gameNow && gameNow.path === game.path && !gameNext) {
            const w = gameIsWin ? winWindows[winWindows.length - 1] : gameWindow
            if (w) {
                raise(w)
                w.takeFocus()
            }
            return
        }
        if (gamePid >= 0) {
            gameNext = game
            naomiApps.stop(gamePid)
            return
        }
        startGame(game)
    }
    function startGame(game) {
        const args = naomiGames.command(game.path)
        if (!args.length) {
            volumeOsd.showMessage("ИГРЫ", "ЭМУЛЯТОР НЕ СОБРАН", "tools/naomi cores")
            return
        }
        gameNow = game
        gameShown = false
        gamePid = game.system === "win" ? naomiApps.run("win", "/bin/sh", naomiDesk.scale, [args[1]])
                                        : naomiApps.launch("game", naomiDesk.scale, args)
        if (gamePid < 0)
            gameNow = null
        else
            gameSettings.last = game.path
    }
    function toggleGame() {   // имя игры на панели: на виду и в фокусе — свернуть (пауза); иначе — показать
        if (!gameWindow)
            return
        if (!gameWindow.minimized && gameWindow.active) {
            gameWindow.minimized = true
            return
        }
        raise(gameWindow)
        gameWindow.takeFocus()
    }
    function closeGame() {
        if (gamePid >= 0)
            naomiApps.stop(gamePid)
    }
    function gameArrived(toplevel, xdgSurface) {
        const w = gameWindowComponent.createObject(windows, {
            area: windows, toplevel: toplevel, xdgSurface: xdgSurface, box: desk.gameBox(gameNow ? gameNow.system : ""),
            title: gameNow ? gameNow.name : "", system: gameNow ? gameNow.system : "", label: gameNow ? gameNow.label : "",
            maximized: gameSettings.maximized, panelMode: Qt.binding(() => gameSettings.panel) })
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            w.destroy()
            if (desk.gameWindow === w)
                desk.gameWindow = null
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        w.panelToggled.connect(() => {
            gameSettings.panel = !gameSettings.panel
            raise(w)
            w.takeFocus()
        })
        w.closeClicked.connect(() => desk.closeGame())
        w.saveRequested.connect(() => {
            gameSettings.window = w.box
            gameSettings.maximized = w.maximized
        })
        w.minimizedChanged.connect(() => {
            ++windowsTick
            naomiApps.pause(desk.gamePid, w.minimized)
            if (w.minimized && w.active) {
                w.active = false
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            }
        })
        gameWindow = w
        ++windowsTick
        raise(w)
        w.takeFocus()
    }
    function gameExited(code) {
        const failed = code !== 0 && !gameWindow && !gameShown && !gameNext   // вышла сама, не показав окна: ядро не приняло игру
        const name = gameNow ? gameNow.name : ""
        gamePid = -1
        gameNow = null
        ++windowsTick
        if (failed)
            volumeOsd.showMessage("ИГРЫ", "НЕ ЗАПУСТИЛАСЬ", name)
        if (gameNext) {
            const next = gameNext
            gameNext = null
            startGame(next)
        }
    }

    // ---------- геймпад и станция (docs/GAMES.md, «Геймпад и станция») ----------
    // Станция геймпада включает его, когда его берут, и выключает, когда ставят (просьба пользователя 21 ноября 2026:
    // «беру геймпад — Денди сама стартует… выключается — ничего или закрыть текущую игру»). Появился (геймпадов стало
    // больше) — выбранная игра или последняя запущенная (game.conf, last); идёт уже какая-то — только показать её
    // (прохождение не терять). Пропал последний —
    // через 3 с закрыть идущую игру: короткий обрыв Bluetooth игру не закроет. Геймпад, что был при создании стола, —
    // не «появился». Выбор — «Настройки» → «Геймпад», ~/.config/naomi/desktop/gamepad.conf (НОВОЕ — ТОЛЬКО В КОНЕЦ).
    Settings {
        id: padSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/gamepad.conf"
        category: "gamepad"
        property string appearGame: ""   // появился — запустить эту игру (путь, как в naomiGames.games); "last" —
                                         // последнюю запущенную; "" — ничего
        property bool goneClose: false   // пропал — закрыть идущую игру
    }
    property int padCount: 0   // сколько геймпадов было (ставит Component.onCompleted стола)
    Connections {
        target: naomiGames
        function onGamepadsChanged() {
            const n = naomiGames.gamepads.length
            if (n > desk.padCount)
                desk.gamepadAppeared()
            else if (n === 0)
                padGoneTimer.restart()
            desk.padCount = n
        }
    }
    Timer {
        id: padGoneTimer
        interval: 3000
        onTriggered: {
            if (naomiGames.gamepads.length || !padSettings.goneClose || desk.gamePid < 0)
                return
            console.info("стол: геймпад пропал — закрываю игру", desk.gameNow ? desk.gameNow.name : "")
            desk.closeGame()
        }
    }
    function gamepadAppeared() {
        padGoneTimer.stop()
        if (!padSettings.appearGame || gameNext)
            return
        if (gamePid >= 0) {
            if (gameNow)
                activateGame(gameNow)   // идущая — наверх
            return
        }
        const path = padSettings.appearGame === "last" ? gameSettings.last : padSettings.appearGame
        const game = naomiGames.games.find(g => g.path === path)
        if (!game)
            return
        console.info("стол: геймпад появился — запускаю", game.name)
        activateGame(game)
    }

    // ---------- кнопки геймпада (docs/GAMES.md, «Кнопки геймпада») ----------
    // Шестерёнка у игры в «03 GAMES» — окно «Кнопки геймпада» этой игры (одно: открыто — показать его с другой игрой).
    // Раскладку хранит и переводит оболочка (naomiPadKeys): окно игры впереди — её поверхность и игра; «клавишами» —
    // кнопки геймпада жмут клавиши в этом окне, ушло назад — геймпад снова у игры
    property var padKeysWindow: null
    Component { id: padKeysWindowComponent; PadKeysWindow {} }
    function openPadKeys(game) {
        bar.menuOpen = false
        if (padKeysWindow) {
            padKeysWindow.game = game
            raise(padKeysWindow)
            return
        }
        const w = padKeysWindowComponent.createObject(windows, {
            game: game, area: windows, y: 24, runningPath: Qt.binding(() => desk.gameNow ? desk.gameNow.path : "") })
        w.x = Math.round((windows.width - w.width) / 2)
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            w.destroy()
            desk.padKeysWindow = null
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        padKeysWindow = w
        ++windowsTick
        raise(w)
    }
    readonly property var padTarget: {   // поверхность окна идущей игры, если оно впереди
        windowsTick
        if (!gameNow)
            return null
        const w = (gameIsWin ? winWindows : gameWindow ? [gameWindow] : []).find(o => o.active && !o.minimized)
        return w ? w.surfaceItem : null
    }
    onPadTargetChanged: naomiPadKeys.setTarget(padTarget, gameNow ? gameNow.path : "", gameNow ? gameNow.system : "")

    // ---------- программы Windows (docs/WINDOWS.md) ----------
    // Wine рисует окна сам, клиентом Wayland (его процессы стол узнаёт по метке окружения — naomiApps.deskApp); каждое
    // окно Windows — окно стола с той же полосой, что у игры (GameWindow, система «win»): имя — заголовок окна,
    // крестик просит окно закрыться. Картинка — как у программы, до места в окне её дотягивает стол.
    property var winWindows: []
    readonly property var winPanel: {
        windowsTick
        return winWindows.map((w, i) => ({ pkg: "win:" + i, name: w.title || "Windows", open: w.minimized ? 0.5 : 1 }))
    }
    function winArrived(toplevel, xdgSurface) {
        const aw = windows.width, ah = windows.height, n = winWindows.length
        const bw = Math.min(aw - 64, 1024), bh = Math.min(ah - 64, 768 + DeskStyle.webBarH)
        const w = gameWindowComponent.createObject(windows, {
            area: windows, toplevel: toplevel, xdgSurface: xdgSurface, system: "win", label: "WINDOWS",
            title: Qt.binding(() => toplevel.title),
            box: Qt.rect(Math.round((aw - bw) / 2) + n * 28, Math.round((ah - bh) / 2) + n * 28, bw, bh) })
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            desk.winWindows = desk.winWindows.filter(o => o !== w)
            w.destroy()
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        w.panelToggled.connect(() => {
            w.panelMode = !w.panelMode
            ++windowsTick
            raise(w)
            w.takeFocus()
        })
        w.closeClicked.connect(() => toplevel.sendClose())
        w.minimizedChanged.connect(() => {
            ++windowsTick
            if (w.minimized && w.active) {
                w.active = false
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            }
        })
        winWindows = winWindows.concat([w])
        if (gamePid >= 0 && gameIsWin)
            gameShown = true
        ++windowsTick
        raise(w)
        w.takeFocus()
    }
    function toggleWin(index) {   // имя окна на панели: на виду и в фокусе — свернуть; иначе — показать
        const w = winWindows[index]
        if (!w)
            return
        if (!w.minimized && w.active) {
            w.minimized = true
            return
        }
        raise(w)
        w.takeFocus()
    }

    // ---------- Claude (docs/CLAUDE.md) ----------
    // Claude Desktop для Linux (Electron; ~/naomi/opt/claude-desktop — tools/naomi claude) — программа стола, которую ядро
    // не морозит: стол не смотрят, а его задачи идут. Окна — как у программ Windows (GameWindow), система «app»: размер
    // окну задаёт стол, русские буквы — текстом (text-input-v3). CLAUDE в меню N: окна нет — запустить (запущен без
    // окна — второй запуск просит его показать окно и выходит); окно на виду и в фокусе — свернуть; иначе — показать.
    // Место окна и «развёрнуто» — в ~/.config/naomi/desktop/claude.conf.
    Settings {
        id: claudeSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/claude.conf"
        category: "claude"
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
    }
    readonly property string claudeProgram: String(StandardPaths.writableLocation(StandardPaths.HomeLocation))
                                            .replace("file://", "") + "/naomi/opt/claude-desktop/naomi-claude"
    property real claudePid: -1
    property var claudeWindows: []
    Settings {
        id: telegramSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/telegram.conf"
        category: "telegram"
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
    }
    property real telegramPid: -1
    property var telegramWindows: []
    Settings {
        id: deepseekSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/deepseek.conf"
        category: "deepseek"
        property bool maximized: false
        property rect window: Qt.rect(0, 0, 0, 0)
    }
    property real deepseekPid: -1
    property var deepseekWindows: []
    property var linuxWindows: []
    readonly property real claudeOpen: {   // черта под CLAUDE: 1 — окно на виду, 0,5 — свёрнуто или запускается
        windowsTick
        return claudeWindows.some(w => !w.minimized) ? 1 : claudeWindows.length || claudePid >= 0 ? 0.5 : 0
    }
    readonly property var claudePanel: {
        windowsTick
        return claudeWindows.map((w, i) => ({ pkg: "claude:" + i, name: w.title || "Claude", open: w.minimized ? 0.5 : 1 }))
    }
    readonly property real telegramOpen: {
        windowsTick
        return telegramWindows.some(w => !w.minimized) ? 1 : telegramWindows.length || telegramPid >= 0 ? 0.5 : 0
    }
    readonly property var telegramPanel: {
        windowsTick
        return telegramWindows.map((w, i) => ({ pkg: "telegram:" + i, name: w.title || "Telegram", open: w.minimized ? 0.5 : 1 }))
    }
    readonly property real deepseekOpen: {
        windowsTick
        return deepseekWindows.some(w => !w.minimized) ? 1 : deepseekWindows.length || deepseekPid >= 0 ? 0.5 : 0
    }
    readonly property var deepseekPanel: {
        windowsTick
        return deepseekWindows.map((w, i) => ({ pkg: "deepseek:" + i, name: "DEEPSEEK", open: w.minimized ? 0.5 : 1 }))
    }
    readonly property var linuxPanel: {
        windowsTick
        return linuxWindows.map((w, i) => ({ pkg: "linux:" + i, name: w.title || "Linux", open: w.minimized ? 0.5 : 1 }))
    }
    function activateLinuxApp(app) {
        if (!app || !app.program)
            return
        bar.menuOpen = false
        if (app.terminal) {
            openTerminalAt("~")
            return
        }
        naomiApps.run("linux-" + (app.id || "app"), app.program, naomiDesk.scale, app.args || [], false)
    }
    function activateClaude() {
        const w = claudeWindows[0]
        if (!w) {
            const pid = naomiApps.run("claude", claudeProgram, naomiDesk.scale, [], false)
            if (claudePid < 0)
                claudePid = pid
            return
        }
        toggleLinux(w)
    }
    function activateTelegram() {
        nmenu.telegramError = ""
        nmenu.deepseekError = ""
        const w = telegramWindows[0]
        if (w) {
            bar.menuOpen = false
            toggleLinux(w)
        } else if (telegramPid < 0) {
            telegramPid = naomiApps.run("telegram", "/usr/local/lib/naomi/naomi-telegram", naomiDesk.scale, [], false)
            if (telegramPid < 0)
                nmenu.telegramError = "Не удалось запустить Telegram Desktop"
        }
    }
    function activateDeepseek() {
        nmenu.telegramError = ""
        nmenu.deepseekError = ""
        const w = deepseekWindows[0]
        if (w) {
            bar.menuOpen = false
            toggleLinux(w)
        } else if (deepseekPid < 0) {
            deepseekPid = naomiApps.run("deepseek", "/usr/local/lib/naomi/naomi-deepseek", naomiDesk.scale, [], false)
            if (deepseekPid < 0)
                nmenu.deepseekError = "Не удалось запустить DeepSeek"
        }
    }
    function toggleLinux(w) {
        if (!w)
            return
        if (!w.minimized && w.active) {
            w.minimized = true
            return
        }
        raise(w)
        w.takeFocus()
    }
    function linuxBox(n, saved) {   // по центру, не шире 1400 и не выше 940; следующие окна — со сдвигом
        const aw = windows.width, ah = windows.height, r = saved
        if (r && n === 0 && r.width >= 320 && r.height >= 280 && r.x + r.width > 40 && r.x < aw - 40 && r.y >= 0 && r.y < ah - 40)
            return Qt.rect(r.x, r.y, Math.min(r.width, aw), Math.min(r.height, ah))
        const w = Math.min(aw - 64, 1400), h = Math.min(ah - 64, 940)
        return Qt.rect(Math.round((aw - w) / 2) + n * 28, Math.round((ah - h) / 2) + n * 28, w, h)
    }
    function linuxArrived(toplevel, xdgSurface, app) {
        const list = app + "Windows"
        const n = desk[list].length
        const settings = app === "claude" ? claudeSettings
                       : app === "telegram" ? telegramSettings
                       : app === "deepseek" ? deepseekSettings : null
        const w = gameWindowComponent.createObject(windows, {
            area: windows, toplevel: toplevel, xdgSurface: xdgSurface, system: "app", label: "", glyph: settings ? app : "monitor",
            title: Qt.binding(() => toplevel.title), box: linuxBox(n, settings ? settings.window : null),
            maximized: !!settings && n === 0 && settings.maximized,
            alwaysMaximized: app === "claude",
            naomiInput: app === "deepseek" })
        w.activated.connect(() => raise(w))
        w.closed.connect(() => {
            desk[list] = desk[list].filter(o => o !== w)
            w.destroy()
            ++windowsTick
            const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
            if (rest.length)
                raise(rest.reduce((a, b) => a.z > b.z ? a : b))
        })
        w.panelToggled.connect(() => {
            w.panelMode = !w.panelMode
            ++windowsTick
            raise(w)
            w.takeFocus()
        })
        w.closeClicked.connect(() => toplevel.sendClose())
        w.saveRequested.connect(() => {
            if (!settings || desk[list][0] !== w)
                return
            settings.window = w.box
            settings.maximized = w.maximized
        })
        w.minimizedChanged.connect(() => {
            ++windowsTick
            if (app === "deepseek" && desk.deepseekPid > 0)
                naomiApps.pause(desk.deepseekPid, w.minimized)
            if (w.minimized && w.active) {
                w.active = false
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            }
        })
        desk[list] = desk[list].concat([w])
        ++windowsTick
        raise(w)
        w.takeFocus()
    }

    // ---------- «Закрыть принудительно» (docs/DESKTOP.md) ----------
    // «02 FORCE QUIT» меню N: программы — процессы с окнами на столе (окна одного процесса — одна строка; окно Wine — у
    // его запускалки, naomiForce.owner), сверху — те, чьё окно выше; приложения Android; «Терминал», пока он открыт; и
    // запущенные, у которых окна ещё (или уже) нет — зависнуть можно и до окна. «Файлы», «Настройки», «Просмотр» —
    // сама оболочка, их тут нет. Пока модуль открыт — naomiForce пингует окна и меряет память (watch).
    readonly property var forceList: {
        windowsTick
        return nmenu.open && nmenu.forceOpen ? forceItems() : []
    }
    onForceListChanged: naomiForce.watch(forceList)
    function forceName(pid, w, app) {
        const a = app !== undefined ? app : naomiApps.deskApp(pid)
        if (a === "game" || a === "win")
            return gameNow ? gameNow.name : w && w.title ? w.title : "Windows"
        if (a === "claude")
            return "Claude"
        if (a === "telegram")
            return "Telegram"
        if (a === "deepseek")
            return "DeepSeek"
        if (a === "web")
            return "Naomi Web"
        return w && w.title ? w.title : w && w.toplevel && w.toplevel.appId ? w.toplevel.appId : "pid " + pid
    }
    function forceItems() {
        const out = [], seen = {}
        const add = (item) => {
            if (!seen[item.key]) {
                seen[item.key] = true
                out.push(item)
            }
        }
        const wins = windows.children.filter(w => w.xdgSurface && !w.android && !w.closing).sort((a, b) => b.z - a.z)
        for (const w of wins) {
            const client = w.xdgSurface.surface ? w.xdgSurface.surface.client : null
            if (!client)
                continue
            const pid = naomiForce.owner(client.processId)
            add({ key: "pid:" + pid, pid: pid, xdg: w.xdgSurface, name: forceName(pid, w) })
        }
        for (const a of androidPanel)
            add({ key: "android:" + a.pkg, pkg: a.pkg, name: a.name })
        if (termWindow && !termWindow.closing && naomiTerm.running)
            add({ key: "term", term: true, name: "Терминал" })
        for (const [pid, app] of [[webPid, "web"], [claudePid, "claude"], [telegramPid, "telegram"],
                                  [deepseekPid, "deepseek"], [gamePid, "game"]])
            if (pid > 0)
                add({ key: "pid:" + pid, pid: pid, name: forceName(pid, null, gamePid === pid && gameIsWin ? "win" : app) })
        return out
    }
    function forceKill(keys) {
        const items = forceList.filter(o => keys.includes(o.key))
        naomiForce.kill(items)   // раньше quitTerm: pid службы «Терминала» — пока он на связи
        if (items.some(o => o.term))
            quitTerm()
    }

    // ---------- Android (docs/ANDROID.md) ----------
    // Окно на приложение: Waydroid присылает окно задачи Android размером со весь экран Android, стол кладёт его на
    // область под панелью (AndroidWindow). Экран Android — эта область пиксель в пиксель: размер ему задаёт стол
    // (настройка окна xdg — Waydroid меняет по ней экран Android); стол сменил размер — экран Android вслед.
    Component { id: androidWindowComponent; AndroidWindow {} }
    // Экран Android — область под панелью и сверху ещё 64 пикселя (androidHiddenPx) — скрытая полоса под панелью стола:
    // туда уходит полоса Android окна «на весь экран» (AndroidMove.togglePanel), приложение — на всю видимую область.
    // Поднять окно выше экрана нельзя (Android не принимает отрицательных мест), поэтому экран выше видимого
    readonly property int androidHiddenPx: 64            // полоса Android — 42,7 dp, при плотности 240 — 64 пикселя
    readonly property real androidHidden: androidHiddenPx / naomiDesk.scale   // она же в точках стола
    readonly property size androidSize: Qt.size(Math.round(windows.width * naomiDesk.scale),
                                                Math.round(windows.height * naomiDesk.scale) + androidHiddenPx)
    property size androidSent: Qt.size(0, 0)
    function sizeAndroid(toplevel) {
        if (!toplevel || androidSize.width <= 0 || (androidSent.width === androidSize.width
                                                    && androidSent.height === androidSize.height))
            return
        androidSent = androidSize
        toplevel.sendMaximized(androidSize)
    }
    onAndroidSizeChanged: {
        const any = windows.children.find(w => w.android && w.toplevel)
        if (any)
            sizeAndroid(any.toplevel)
    }
    // Меню N, «03 ANDROID»: какие приложения с окнами (1 — на виду, 0,5 — только свёрнутые) и какие запускаются —
    // от щелчка до окна, не дольше 90 с (Android включается до ~20 с, приложение может и не открыться)
    readonly property var androidOpen: {
        windowsTick
        const o = {}
        for (const w of windows.children)
            if (w.android && !w.closing && w.shown && w.pkg)
                o[w.pkg] = Math.max(o[w.pkg] || 0, w.minimized ? 0.5 : 1)
        return o
    }
    // Панель: открытые приложения Android за FILE и WEB — по порядку, как открывали в первый раз (закрыли и открыли
    // снова — на прежнем месте: окно, которое Waydroid пересоздал, не прыгает в конец); на панели — те, у кого есть окна,
    // и те, чьё окно ушло под другим окном Android (androidCovered). Имя — из «03 ANDROID» (системные приложения там
    // скрыты — от пакета: com.android.calculator2 → calculator2)
    property var androidOrder: []
    // Waydroid уничтожает окно, целиком закрытое другим окном Android (задача жива; проверено: Калькулятор под
    // развёрнутыми «Настройками»). Такое приложение — на панели, щелчок вызывает его наверх (activateAndroid → Android
    // поднимает задачу, Waydroid даёт окно заново). Пакет → свёрнуто ли было окно
    property var androidCovered: ({})
    readonly property var androidPanel: {
        windowsTick
        const out = []
        for (const pkg of androidOrder) {
            const open = androidOpen[pkg] !== undefined ? androidOpen[pkg]
                       : androidCovered[pkg] !== undefined ? (androidCovered[pkg] ? 0.5 : 1) : undefined
            if (open !== undefined) {
                const name = androidName(pkg)
                out.push({ pkg: pkg, name: name === pkg ? pkg.split(".").pop() : name, open: open })
            }
        }
        return out
    }
    function setCovered(pkg, minimized) {   // minimized: undefined — убрать
        const c = Object.assign({}, androidCovered)
        if (minimized === undefined)
            delete c[pkg]
        else
            c[pkg] = minimized
        androidCovered = c
    }
    // щелчок по приложению на панели: все его окна на виду и одно в фокусе — свернуть; иначе — показать (как FILE)
    function toggleAndroid(pkg) {
        const ws = windows.children.filter(w => w.android && !w.closing && w.shown && w.pkg === pkg)
        if (ws.length && ws.every(w => !w.minimized) && ws.some(w => w.active)) {
            ws.forEach(w => w.minimized = true)
            return
        }
        activateAndroid(pkg)
    }
    property var androidLaunching: ({})
    function setLaunching(pkg, on) {
        const l = Object.assign({}, androidLaunching)
        if (on)
            l[pkg] = Date.now()
        else
            delete l[pkg]
        androidLaunching = l
    }
    Timer {
        interval: 5000
        repeat: true
        running: Object.keys(desk.androidLaunching).length > 0
        onTriggered: {
            for (const pkg in desk.androidLaunching)
                if (Date.now() - desk.androidLaunching[pkg] > 90000)
                    desk.setLaunching(pkg, false)
        }
    }
    function androidName(pkg) {
        const a = naomiAndroid.apps.find(o => o.pkg === pkg)
        return a ? a.name : pkg === "com.android.vending" ? "Google Play"
                          : pkg === "com.android.settings" ? "Настройки Android" : pkg
    }
    // строка «03 ANDROID», «Настройки» Android, Google Play: окна есть — показать (свёрнутые тоже), последнее сверху;
    // нет — открыть (Android выключен — naomiAndroid включит его сам)
    function activateAndroid(pkg) {
        bar.menuOpen = false
        const ws = windows.children.filter(w => w.android && !w.closing && w.shown && w.pkg === pkg)
        if (ws.length) {
            ws.sort((a, b) => a.z - b.z).forEach(w => raise(w))
            ws[ws.length - 1].takeFocus()
            return
        }
        if (androidLaunching[pkg])
            return   // уже запускается
        if (androidGone[pkg]) {   // открывают сами — окно наверх, хоть его и убрали только что
            const g = Object.assign({}, androidGone)
            delete g[pkg]
            androidGone = g
        }
        setLaunching(pkg, true)
        naomiAndroid.launch(pkg)
    }
    // ---------- сон Android (docs/ANDROID.md, «Сон») ----------
    // Окна Android не видно — Android спит, как телефон с погашенным экраном: игра на паузе, музыка в фоне играет,
    // кадров и работы нет. Иначе свёрнутая игра работала на полную — ядро, видеочип, 3 Вт (docs/ANDROID-AUDIT.md,
    // находка 3): Android не знает, что окно со стола убрали. Не видно — когда все окна Android свёрнуты (сон через
    // 0,4 с) или каждое не свёрнутое целиком закрыто окном стола (через 5 с: окно могли лишь пронести поверх).
    // Спит весь Android: одно приложение на виду — работают и свёрнутые (по одному Android усыплять не даёт)
    function deskCovers(x, y, width, height, z) {   // окно стола выше z закрывает прямоугольник (точки области окон) целиком
        if (width <= 0)
            return false
        for (const o of windows.children) {
            if (o.android || o.closing || o.minimized || o.parked || o.z < z || !o.visible || o.opacity < 1)
                continue
            // у окна стола скруглены углы: не во всю область — закрывает с запасом в 10 точек от краёв
            const m = o.x <= 0 && o.y <= 0 && o.x + o.width >= windows.width && o.y + o.height >= windows.height ? 0 : 10
            if (o.x + m <= x && o.y + m <= y && o.x + o.width - m >= x + width && o.y + o.height - m >= y + height)
                return true
        }
        return false
    }
    // Окна, которые Waydroid убрал, пока Android спал (засыпая, он иногда успевает отдать кадр без слоёв — окно
    // уходит): для стола они есть, пока Android не разбудят, — иначе «окон нет» будило бы его тут же, а он засыпал бы
    // снова, и так по кругу (замечено 19 ноября 2026: игра под развёрнутыми «Файлами»). [{pkg, x, y, width, height, z,
    // minimized}] — место в области окон
    property var androidGhosts: []
    readonly property int androidSeen: {   // 1 — видно (или окон нет: тогда Waydroid спит сам), 0 — свёрнуты/экран выкл., -1 — закрыты
        windowsTick
        if (DeskStyle.tablet && !naomiPower.displayOn && !naomiStream.connected)
            return 0
        if (Object.keys(androidLaunching).length > 0)
            return 1
        let any = false, covered = false
        for (const w of windows.children) {
            if (!w.android || w.closing)
                continue
            any = true
            if (w.minimized)
                continue
            if (!w.shown || !deskCovers(w.frame.x + w.x, w.frame.y + w.y, w.frame.width, w.frame.height, w.z))
                return 1
            covered = true
        }
        for (const g of androidGhosts) {
            any = true
            if (g.minimized)
                continue
            if (!deskCovers(g.x, g.y, g.width, g.height, g.z))
                return 1
            covered = true
        }
        return !any ? 1 : covered ? -1 : 0
    }
    onAndroidSeenChanged: naomiAndroid.setSeen(androidSeen > 0, androidSeen < 0 ? 5000 : 400)
    // есть ли у Android окна (и призраки) — без них видеокодек ему не нужен (naomiAndroid.setWindows)
    readonly property bool androidAny: {
        windowsTick
        return androidGhosts.length > 0 || windows.children.some(w => w.android && !w.closing)
    }
    onAndroidAnyChanged: naomiAndroid.setWindows(androidAny)
    Timer {   // окно, которое Waydroid после сна оставил как было, — показать
        id: androidStaleTimer
        interval: 800
        onTriggered: windows.children.forEach(w => { if (w.android && w.stale) w.stale = false })
    }
    Connections {
        target: naomiAndroid
        function onAsleepChanged() {
            if (naomiAndroid.asleep) {
                androidStaleTimer.stop()
                windows.children.forEach(w => { if (w.android && !w.closing) w.stale = true })
            } else {
                // окна, ушедшие во сне, Waydroid сейчас создаст заново — на прежние места (androidGone: срок — от сейчас)
                const g = Object.assign({}, desk.androidGone)
                for (const ghost of desk.androidGhosts)
                    if (g[ghost.pkg])
                        g[ghost.pkg] = Object.assign({}, g[ghost.pkg], { at: Date.now() })
                desk.androidGone = g
                desk.androidGhosts = []
                androidStaleTimer.restart()
            }
        }
    }

    // Окно Android, которое Waydroid только что убрал и создаёт снова: Android не отдаёт слоёв окна, целиком закрытого
    // другим (развернули соседнее), и Waydroid такое окно уничтожает, а открылось — создаёт заново. Пересозданное
    // встаёт на своё место в стопке и фокус не берёт: раньше оно выскакивало наверх, закрывая то, с чем работали
    // (проверено 18 ноября 2026: развернули и вернули «Калькулятор» — сверху оказались «Настройки»). Так же — все окна
    // после сна Android: на прежние места, свёрнутое остаётся свёрнутым
    property var androidGone: ({})   // пакет → { z, active, at, minimized }
    Connections {
        target: naomiAndroid
        function onWindow(toplevel, xdgSurface) {
            const w = androidWindowComponent.createObject(windows, {
                area: windows, toplevel: toplevel, xdgSurface: xdgSurface, hiddenPx: desk.androidHiddenPx,
                hidden: Qt.binding(() => desk.androidHidden) })
            desk.sizeAndroid(toplevel)
            // в журнал — когда окно пришло и через сколько у него картинка (обычно — доли секунды; нет — Android встал)
            const born = Date.now()
            console.info("окно Android: создано" + (w.pkg ? " — " + w.pkg : ""))
            w.stuck.connect(() => naomiAndroid.restartStuck(w.pkg))
            w.shownChanged.connect(() => {
                if (!w.shown)
                    return
                console.info("окно Android " + w.pkg + ": картинка через " + (Date.now() - born) + " мс")
                if (w.pkg)
                    desk.setLaunching(w.pkg, false)
                if (w.pkg && !desk.androidOrder.includes(w.pkg))
                    desk.androidOrder = desk.androidOrder.concat([w.pkg])
                if (w.pkg && desk.androidCovered[w.pkg] !== undefined)
                    desk.setCovered(w.pkg, undefined)
                ++windowsTick
                const gone = desk.androidGone[w.pkg]
                if (gone && Date.now() - gone.at < 5000) {
                    const g = Object.assign({}, desk.androidGone)
                    delete g[w.pkg]
                    desk.androidGone = g
                    if (gone.minimized)
                        w.minimized = true
                    if (!gone.active || gone.minimized) {
                        w.z = gone.z
                        return
                    }
                }
                raise(w)
                w.takeFocus()
            })
            w.pressedAt.connect((p) => {
                if (windowAt(w.mapToItem(desk, p.x, p.y)) !== w)
                    return   // нажали в окно стола над слоем Android
                raise(w)
                w.takeFocus()
            })
            w.closed.connect(() => {
                console.info("окно Android " + (w.pkg || "без имени") + ": ушло" + (w.shown ? "" : " без картинки"))
                // последнее окно приложения, целиком под другим окном Android, — Waydroid убрал его как невидимое
                const f = w.frame
                if (w.shown && w.pkg && !w.closeAsked && f.width > 0
                        && !windows.children.some(o => o !== w && o.android && !o.closing && o.pkg === w.pkg)
                        && windows.children.some(o => o !== w && o.android && o.shown && !o.closing
                                                 && o.frame.x <= f.x + 8 && o.frame.y <= f.y + 8
                                                 && o.frame.x + o.frame.width >= f.x + f.width - 8
                                                 && o.frame.y + o.frame.height >= f.y + f.height - 8))
                    desk.setCovered(w.pkg, w.minimized)
                // Android проснулся и создаёт окна заново: окно с картинкой до сна (stale) ушло — на панели приложение
                // остаётся (не мигает), пока идёт новое
                else if (w.stale && w.shown && w.pkg && !w.closeAsked)
                    desk.setCovered(w.pkg, w.minimized)
                if (naomiAndroid.asleep && w.shown && w.pkg && !w.closeAsked && f.width > 0)
                    desk.androidGhosts = desk.androidGhosts.concat([{ pkg: w.pkg, x: f.x + w.x, y: f.y + w.y, width: f.width,
                                                                      height: f.height, z: w.z, minimized: w.minimized }])
                if (w.shown && w.pkg) {
                    const g = Object.assign({}, desk.androidGone)
                    g[w.pkg] = { z: w.z, active: w.active, at: Date.now(), minimized: w.minimized }
                    desk.androidGone = g
                }
                w.destroy()
                ++windowsTick
                const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                if (rest.length)
                    raise(rest.reduce((a, b) => a.z > b.z ? a : b))
            })
            w.minimizedChanged.connect(() => {
                ++windowsTick
                if (w.minimized && w.active) {
                    w.active = false
                    const rest = windows.children.filter(o => o !== w && !o.minimized && !o.closing && !o.parked)
                    if (rest.length)
                        raise(rest.reduce((a, b) => a.z > b.z ? a : b))
                }
            })
        }
        function onStateChanged() {   // Android выключили — экран ему зададут заново
            if (naomiAndroid.state === "off") {
                desk.androidSent = Qt.size(0, 0)
                desk.androidGhosts = []
            }
        }
    }

    // Масштаб стола сменился (зритель другой: окно Mac — 1,5, приставка на Pi — под свой монитор): браузер рисует
    // с QT_SCALE_FACTOR при запуске — перезапустить в новом
    property bool webRescale: false
    Connections {
        target: naomiDesk
        function onScaleChanged() {
            if (desk.webPid < 0)
                return
            desk.webRescale = true
            naomiApps.stop(desk.webPid)
        }
    }
    // правый щелчок по странице — меню страницы у курсора (только окно на виду и в фокусе)
    Connections {
        target: naomiWeb
        function onContextMenu(tab) {
            if (!desk.webShown || !web.tabs.some(t => t.id === tab))
                return
            webMenu.tabId = tab
            webMenu.show(desk.pointer.x, desk.pointer.y, [
                { text: "Назад", tone: "off", action: "back" },
                { text: "Вперёд", tone: "off", action: "forward" },
                { text: "Обновить", tone: "off", action: "reload" },
                { text: "Тёмная / светлая страница", tone: "off", action: "toggle-dark" }])
        }
        function onFieldRequested(text) {
            if (desk.webWindow && !desk.webShown)
                desk.raise(desk.webWindow)
            web.focusField(text === undefined ? null : text)
        }
        function onTabsChanged() {   // вкладки меню больше нет — меню закрыть
            if (webMenu.open && !web.tabs.some(t => t.id === webMenu.tabId))
                webMenu.close()
        }
        function onOpenRequested(url) { desk.openLink(url) }
        function onConnectedChanged() {
            if (naomiWeb.connected && desk.linkPending) {
                const url = desk.linkPending
                desk.linkPending = ""
                desk.openLink(url)
            }
        }
    }
    // Ссылка снаружи (org.naomi.Web.Open — xdg-open у Claude, docs/CLAUDE.md): браузер на связи — новой вкладкой, его
    // окно наверх; нет — запустить и открыть, как только он на связи
    property string linkPending: ""
    function openLink(url) {
        if (!naomiWeb.connected) {
            linkPending = url
            if (webPid < 0)
                webPid = naomiApps.launch("web", naomiDesk.scale)
            return
        }
        naomiWeb.send({ type: "open", url: url, newTab: true })
        if (webWindow) {
            raise(webWindow)
            webWindow.takeFocus()
        }
    }
    // где мышь — для меню у курсора: последнее нажатие (над страницей браузера наведения стол не видит)
    property point pointer
    MouseArea {
        anchors.fill: parent
        z: 2000
        acceptedButtons: Qt.AllButtons
        Component.onCompleted: cursorShape = undefined   // без формы: MouseArea ставит стрелку сама и закрыла бы формы под собой
        onPressed: (m) => {
            desk.pointer = Qt.point(m.x, m.y)
            m.accepted = false
        }
    }

    // ---------- перетаскивание файлов между окнами ----------
    property var dragPaths: []
    property var dragInfo: null
    property point dragPos
    function windowAt(p) {   // верхнее видимое окно под точкой стола (окно Android — только его слои)
        let best = null
        for (const w of windows.children) {
            if (w.closing || w.minimized)
                continue
            const q = w.mapFromItem(desk, p.x, p.y)
            const inside = w.android ? w.hits(q) : q.x >= 0 && q.y >= 0 && q.x < w.width && q.y < w.height
            if (inside && (!best || w.z > best.z))
                best = w
        }
        return best
    }
    function dragStart(from, paths, info) {
        dragPaths = paths
        dragInfo = info
    }
    function dragMove(p) {
        dragPos = p
        const target = windowAt(p)
        for (const w of windows.children)
            w.dragOver(w === target ? w.mapFromItem(desk, p.x, p.y) : null)
    }
    function dragEnd(p) {
        const target = windowAt(p), paths = dragPaths
        dragPaths = []
        dragInfo = null
        for (const w of windows.children)
            w.dragOver(null)
        if (target)
            target.dropAt(target.mapFromItem(desk, p.x, p.y), paths)
    }
    Item {   // у мыши — значок первого и число
        visible: desk.dragPaths.length > 0 && desk.dragInfo !== null
        x: desk.dragPos.x - 24
        y: desk.dragPos.y - 18
        z: 1000
        FileIcon {
            k: 0.5
            dir: desk.dragInfo ? desk.dragInfo.isDir : false
            ext: desk.dragInfo ? desk.dragInfo.ext : ""
            image: desk.dragInfo ? desk.dragInfo.image : false
            path: desk.dragInfo ? desk.dragInfo.path : ""
        }
        Rectangle {
            visible: desk.dragPaths.length > 1
            x: 38; y: -4
            width: Math.max(18, badge.implicitWidth + 10); height: 18
            radius: 9
            color: DeskStyle.amber
            Text {
                id: badge
                anchors.centerIn: parent
                text: desk.dragPaths.length
                font.family: DeskStyle.mono
                font.pixelSize: 11
                color: DeskStyle.bg
            }
        }
    }

    // ---------- меню браузера: вкладки, страницы, «Избранное» ----------
    MouseArea {   // щелчок мимо меню — закрыть его (сам щелчок съедается)
        anchors.fill: parent
        z: 1004
        enabled: webMenu.open || bookmarks.open || termMenu.open
        // без формы: «выключенная» MouseArea для Qt всё равно элемент со стрелкой — закрыла бы формы под собой
        Component.onCompleted: cursorShape = undefined
        acceptedButtons: Qt.AllButtons
        onPressed: {
            webMenu.close()
            bookmarks.close()
            termMenu.close()
        }
    }
    WebMenu {
        id: webMenu
        property int tabId: -1
        z: 1005
        bounds: desk
        onPicked: (action) => {
            if (action === "pin") {
                const t = web.tabs.find(o => o.id === tabId)
                naomiWeb.send({ type: "pin", id: tabId, pinned: !(t && t.pinned) })
            } else if (action === "close") {
                web.close(tabId)
            } else {
                naomiWeb.send({ type: action, id: tabId })
            }
        }
        onDismissed: if (desk.webWindow && desk.webShown && !web.focused) desk.webWindow.takeFocus()
    }
    WebMenu {   // меню вкладки «Терминала» и его текста — то же меню у курсора, что у браузера
        id: termMenu
        property int tabId: -1
        z: 1005
        bounds: desk
        onPicked: (action) => {
            if (action === "pin") {
                const t = naomiTerm.tabs.find(o => o.id === tabId)
                naomiTerm.pin(tabId, !(t && t.pinned))
            } else if (action === "rename") {
                term.startRename(tabId)
            } else if (action === "duplicate") {
                naomiTerm.duplicate(tabId)
            } else if (action === "close") {
                term.close(tabId)
            } else if (action === "copy" && desk.termWindow) {
                desk.termWindow.view.copy()
            } else if (action === "paste" && desk.termWindow) {
                desk.termWindow.view.paste()
            }
        }
        onDismissed: if (desk.termWindow && desk.termOpen >= 1 && term.renaming < 0) desk.termWindow.takeFocus()
    }
    WebBookmarks {
        id: bookmarks
        z: 1005
        st: web
        onOpened: (url) => {
            naomiWeb.send({ type: "open", url: url, newTab: !!(web.activeTab && web.activeTab.pinned) })
            if (desk.webWindow)
                desk.raise(desk.webWindow)
        }
        onDismissed: if (desk.webWindow && desk.webShown && !web.focused) desk.webWindow.takeFocus()
    }

    // ---------- звук (docs/DESKTOP.md, «Звук») ----------
    // Выходы стола: «Динамики» — монитор приставки (зритель играет звук стола: naomiStream.audio; у naomid — выход
    // desk, его звук оболочка шлёт приставке), «Телефон» — его динамик и «Наушники» — Bluetooth, пока на связи. У каждого
    // — своя громкость; ползунок, колесо и клавиши меняют громкость того, где звук сейчас. Выбор помнится (sound.conf):
    // подключили приставку снова — звук туда же, где был в прошлый раз; громкость монитора — тоже (naomid берёт её,
    // только когда создаёт выход заново: живой держит свою). Наушники подключились (достали из кейса, перезагрузка) —
    // звук в них, если их не покидали щелчком (away); пропали — туда, где был до них (back): это naomid делает сам, по
    // порядку выходов (SetRoute ["bluetooth", "desk"]). Щелчок по значку выхода не даёт тишины: молчащему выходу (0 %,
    // «без звука») naomid вернёт его громкость. Зритель ушёл (кабель и Wi-Fi пропали, Pi выключили, трансляцию
    // выключили) — браузер ставит ролики на паузу (docs/WEB.md, «Без зрителя»), звук — по правилу naomid (наушники, иначе
    // динамик) через 0,6 с, когда пауза дошла: переводили сразу — поток браузера уезжал на динамик через 0,13 с после
    // команды «пауза», а встал только через 2 с, и хвост ролика мог прозвучать из телефона (замер 17 ноября 2026);
    // оболочка упала — naomid вернёт звук сам.
    Settings {
        id: soundSettings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/sound.conf"
        category: "sound"
        property string output: "monitor"   // monitor — «Динамики», phone — «Телефон», headphones — «Наушники»
        property string back: "monitor"     // куда звук, пока наушников нет: выбор до них (monitor / phone)
        property bool away: false           // звук увели щелчком с наушников на связи: подключатся снова — не перехватят
        property int monitorVolume: -1      // громкость «Динамиков» (−1 — ещё не меняли)
    }
    property bool soundOpen: false
    property bool btOpen: false         // окно Bluetooth (BluetoothPopover)
    property bool inetOpen: false       // окно «Интернет» (InternetPopover)
    // окно батареи (BatteryPopover; docs/DESKTOP.md, «Батарея») — одно из окон под значками: открылось другое окно,
    // меню N или щёлкнули по программе на панели — закрыть
    property bool batteryOpen: false
    property bool clipOpen: false
    onSoundOpenChanged: if (soundOpen) { batteryOpen = false; clipOpen = false }
    onBtOpenChanged: if (btOpen) { batteryOpen = false; clipOpen = false }
    onInetOpenChanged: if (inetOpen) { batteryOpen = false; clipOpen = false }
    onBatteryOpenChanged: if (batteryOpen) clipOpen = false
    onClipOpenChanged: if (clipOpen) { batteryOpen = false; soundOpen = false; btOpen = false; inetOpen = false; bar.menuOpen = false }
    Connections {
        target: bar
        function onMenuOpenChanged() { if (bar.menuOpen) { desk.batteryOpen = false; desk.clipOpen = false } }
        function onFilesClicked() { desk.batteryOpen = false; desk.clipOpen = false }
        function onWebClicked() { desk.batteryOpen = false; desk.clipOpen = false }
        function onTermClicked() { desk.batteryOpen = false; desk.clipOpen = false }
        function onAndroidClicked(pkg) { desk.batteryOpen = false; desk.clipOpen = false }
    }
    DeskBattery {   // заряд и прогноз для значка и окна; ток спрашивает, только пока стол смотрят
        id: battery
        live: DeskStyle.tablet ? naomiPower.displayOn : naomiStream.connected && naomiStream.desktop && !naomiStream.deskAway
        open: desk.batteryOpen
    }
    // монитор системы (MonitorPopover): открыт, пока по индикатору подключения не щёлкнут снова; собран — с первого
    // открытия
    property bool monitorOpen: false
    property bool monitorBuilt: false
    // куда монитор оттащили (левый верх, точки стола); не таскали — под индикатором. Помнится, пока жива оболочка
    property bool monitorMoved: false
    property point monitorAt
    onMonitorOpenChanged: if (monitorOpen) monitorBuilt = true
    readonly property bool monitorAvailable: naomiStream.audio
    readonly property string soundOut: naomiPhone.audio.output
    readonly property int audioVolume: naomiPhone.audio.volume
    readonly property int deskVolume: naomiPhone.audio.deskVolume   // громкость «Монитора», где бы ни был звук
    onDeskVolumeChanged: if (deskVolume >= 0) soundSettings.monitorVolume = deskVolume
    property int volTarget: -1          // громкость, только что поставленная отсюда (ответ naomid ещё в пути)
    readonly property int shownVolume: volTarget >= 0 ? volTarget : audioVolume
    Timer { id: volSettle; interval: 400; onTriggered: desk.volTarget = -1 }
    onSoundOutChanged: volTarget = -1   // другой выход — своя громкость
    readonly property bool headphones: naomiPhone.audio.headphones !== ""   // наушники Bluetooth на связи
    // стол смотрят — выход выбирает стол; нет (или смотрят экран телефона) — правило naomid: наушники, иначе динамик
    readonly property bool soundViewed: naomiStream.connected && naomiStream.desktop
    // выход по выбору сейчас: monitor, phone, headphones (наушников нет — выбор до них; монитора нет — телефон)
    function soundCurrent() {
        if (soundSettings.output === "headphones" && headphones)
            return "headphones"
        const base = soundSettings.output === "headphones" ? soundSettings.back : soundSettings.output
        return base === "monitor" && monitorAvailable ? "monitor" : "phone"
    }
    // порядок выходов для naomid: выбраны наушники — они, а пропадут — выбор до них, без задержки на стол
    function soundOrder() {
        const base = soundSettings.output === "headphones" ? soundSettings.back : soundSettings.output
        const fallback = base === "monitor" && monitorAvailable ? "desk" : "phone"
        return soundSettings.output === "headphones" ? ["bluetooth", fallback] : [fallback]
    }
    // audible — переключил человек: выход, куда ушёл звук, не молчит
    function routeSound(audible) {
        if (!naomiPhone.available)
            return
        naomiPhone.audio.call("SetRoute", [soundViewed ? soundOrder() : [], soundSettings.monitorVolume, !!audible])
    }
    onMonitorAvailableChanged: monitorAvailable ? routeSound(false) : soundBack.restart()
    onSoundViewedChanged: soundViewed ? routeSound(false) : soundBack.restart()
    Timer { id: soundBack; interval: 600; onTriggered: desk.routeSound(false) }
    // Наушники подключились — звук в них (выбор «до них» — back), если звук с них не уводили щелчком; пропали — снова
    // можно перехватывать (away), звук naomid уже вернул по порядку
    property bool hadHeadphones: false
    function headphonesArrived() {
        if (soundSettings.output === "headphones" || soundSettings.away)
            return
        soundSettings.back = soundSettings.output
        soundSettings.output = "headphones"
        routeSound(true)
    }
    onHeadphonesChanged: {
        if (headphones === hadHeadphones)
            return
        hadHeadphones = headphones
        if (headphones)
            headphonesArrived()
        else
            soundSettings.away = false
    }
    Component.onCompleted: {   // стол создаётся, когда его впервые смотрят: зритель уже мог сказать «играю звук»
        if (deskVolume >= 0)
            soundSettings.monitorVolume = deskVolume
        // наушники подключились раньше стола (перезагрузка: они — за ~20 с, стол — со зрителем): так же, как при нём
        hadHeadphones = headphones
        if (headphones)
            headphonesArrived()
        routeSound(false)
        settingsLoaded = true   // настройки мыши и экрана прочитаны — зрителю
        rememberMonitor()
        if (!DeskStyle.tablet)
            spareTimer.start()      // запас «Файлов» — через 1,5 с покоя
        padCount = naomiGames.gamepads.length   // геймпад, что уже был, — не «появился»
        if (DeskStyle.tablet && naomiKeyboard && naomiKeyboard.visible)
            keyboardBuilt = true
        naomiDisplay.setNightWarmth(uiSettings.nightFilter)
    }
    Connections {
        target: naomiPhone
        function onAvailableChanged() { desk.routeSound(false) }
    }
    Binding { target: naomiStream; property: "sendAudio"; value: desk.soundOut === "desk" }
    // обои — экранам, что рисуют заставку без дока сами (окно Mac, планшет; docs/SCREENS.md): файл и номер версии
    Binding {
        target: naomiStream
        property: "wallpaper"
        value: String(naomiFiles.desktopWallpaper) || "none"
    }
    // щелчок по значку выхода — следующий по кругу: «Динамики» → «Телефон» → «Наушники» (монитора или наушников нет —
    // без них; выход один — щелчок ничего не меняет). Ушли с наушников на связи — они звук больше не перехватывают
    function toggleOutput() {
        const outs = (monitorAvailable ? ["monitor"] : []).concat(["phone"], headphones ? ["headphones"] : [])
        if (outs.length < 2)
            return
        const next = outs[(outs.indexOf(soundCurrent()) + 1) % outs.length]
        soundSettings.output = next
        if (next !== "headphones")
            soundSettings.back = next
        soundSettings.away = headphones && next !== "headphones"
        routeSound(true)
    }
    function setVolume(v) {
        volTarget = v
        volSettle.restart()
        if (naomiPhone.audio.muted)
            naomiPhone.audio.call("SetMuted", [false])
        naomiPhone.audio.call("SetVolume", [v])
    }
    // шаг клавиш и колеса — как audio_step НаомиОС: ±5 по сетке (43 % → 50 %), снимает «без звука»; 0 — «без звука»
    function soundStep(dir) {
        if (!naomiPhone.available)
            return
        if (dir === 0) {
            naomiPhone.audio.call("SetMuted", [!naomiPhone.audio.muted])
            return
        }
        const steps = shownVolume / 5
        setVolume(Math.max(0, Math.min(100, (dir > 0 ? Math.floor(steps + 0.01) + 1 : Math.ceil(steps - 0.01) - 1) * 5)))
    }
    // Колесо над звуком: прокрутка стола плавная — щелчок колеса приставки (100 точек) приходит десятком событий,
    // трекпад Mac — потоком; шаг ±5 — на каждые 100 точек сдвига, остаток копится, пока крутят (0,4 с)
    property real wheelAcc: 0
    Timer { id: wheelIdle; interval: 400; onTriggered: desk.wheelAcc = 0 }
    function soundWheel(delta) {
        wheelIdle.restart()
        if (Math.sign(delta) !== Math.sign(wheelAcc))
            wheelAcc = 0
        wheelAcc += delta
        let stepped = false
        while (Math.abs(wheelAcc) >= 100) {
            soundStep(wheelAcc > 0 ? 1 : -1)
            wheelAcc -= 100 * Math.sign(wheelAcc)
            stepped = true
        }
        if (stepped && !soundOpen)
            volumeOsd.show()
    }
    Connections {   // клавиши громкости клавиатуры приставки; клавиши яркости — приставка ответила, какая стала
        target: naomiStream
        function onBrightnessShown(percent) { volumeOsd.showLevel("ЯРКОСТЬ", percent) }
        function onVolumeKey(step) {
            desk.soundStep(step)
            if (!desk.soundOpen)
                volumeOsd.show()
        }
    }
    Connections {   // кнопки громкости телефона, пока смотрят стол: шаг делает оболочка телефона, плашка — и здесь
        target: naomiPower
        function onVolumeKey(direction) {
            if (DeskStyle.tablet)
                desk.soundStep(direction)
            if ((DeskStyle.tablet || naomiStream.desktop) && !desk.soundOpen)
                volumeOsd.show()
        }
    }

    // ---------- скриншоты (docs/DESKTOP.md, «Скриншоты») ----------
    // Как в НаомиОС: Shift+Alt+3 — весь стол, Shift+Alt+4 — область (прицел); клавиши ловит ввод зрителя
    // (naomiStream.screenshotKey) — и когда в фокусе браузер. Перед снимком плашка и миниатюра прошлого убираются
    // сразу — на новый они не попадут. Снят — миниатюра справа сверху (плашки «СОХРАНЁН» нет: снимок подтверждает она),
    // щелчок по ней — снимок в окне «Просмотра», как пробел в «Файлах»; не вышло — плашка «СКРИНШОТ — НЕ ПОЛУЧИЛСЯ» с причиной. Папку ~/Screenshots
    // создаёт первый снимок — тогда же она закрепляется в «Файлах» («02 СВОИ»), как закладка у НаомиОС.
    function screenshot(area) {
        if (naomiShot.busy)
            return
        volumeOsd.hideNow()
        shotThumb.hideNow()
        naomiShot.take(area)
    }
    Connections {
        target: naomiStream
        function onScreenshotKey(key) {
            if (key === 3 && !shotSelect.selecting)
                desk.screenshot(Qt.rect(0, 0, 0, 0))
            else if (key === 4 && !naomiShot.busy)
                shotSelect.begin()
            else if (key === 0)
                shotSelect.cancel()
        }
    }
    Binding { target: naomiStream; property: "shotSelecting"; value: shotSelect.selecting }
    Connections {
        target: naomiShot
        function onSaved(path, thumb, size, folderCreated) {
            if (folderCreated)
                naomiFiles.pin(path.substring(0, path.lastIndexOf("/")))
            shotThumb.show(path, thumb, size)
        }
        function onFailed(why) { volumeOsd.showMessage("СКРИНШОТ", "НЕ ПОЛУЧИЛСЯ", why) }
    }
    ShotThumb {
        id: shotThumb
        z: 1993   // поверх всего, как layer_osd
        restX: Math.round((desk.width - 14) * naomiDesk.pixelScale) / naomiDesk.pixelScale - width   // THUMB_MARGIN
        y: Math.round((DeskStyle.panelH + 14) * naomiDesk.pixelScale) / naomiDesk.pixelScale
        onOpened: (path) => desk.openPreview(null, path)   // окном, как пробел в «Файлах»
    }
    ShotSelect {
        id: shotSelect
        z: 1996
        anchors.fill: parent
        onTaken: (area) => desk.screenshot(area)
    }

    // ---------- «Просмотр» (docs/DESKTOP.md, «Просмотр») ----------
    // Окно одно, как у НаомиОС (naomi-preview): поверх окон, даже когда фокус у «Файлов»; на весь стол — и поверх
    // панели. Создаётся при первом открытии и дальше живёт спрятанным: открыть снова — сразу.
    Item {
        id: previewArea
        y: DeskStyle.panelH
        width: parent.width
        height: parent.height - y
        z: desk.preview && desk.preview.full ? 1985 : 500
        opacity: desk.chromeIn
    }
    property var preview: null
    Component { id: previewComponent; PreviewWindow {} }
    function openPreview(source, path) {
        if (!path)
            return
        if (!preview)
            preview = previewComponent.createObject(previewArea, { desk: desk, area: previewArea, parked: true })
        preview.open(source, path)
    }
    function togglePreview(source, path) {   // пробел в «Файлах»: открыть выделенное, открыт — закрыть
        if (preview && preview.isOpen)
            preview.hide()
        else
            openPreview(source, path)
    }
    function previewFollow(source, path) {   // выделение в «Файлах» сменилось — просмотр показывает его
        if (preview && preview.isOpen && preview.source === source)
            preview.show(path, false)
    }
    Connections {   // зритель ушёл, стол ушёл или экран планшета погас — видео на паузу
        target: naomiStream
        function onConnectedChanged() { if (!naomiStream.connected && (!DeskStyle.tablet || !naomiPower.displayOn)) naomiPreview.video.pause() }
        function onDeskAwayChanged() { if (naomiStream.deskAway) naomiPreview.video.pause() }
    }
    Connections {
        target: naomiPower
        function onDisplayOnChanged() {
            if (DeskStyle.tablet && !naomiPower.displayOn && !naomiStream.connected)
                naomiPreview.video.pause()
        }
    }

    // ---------- меню N, окна звука, Bluetooth и «Интернет», панель ----------
    MouseArea {   // щелчок мимо меню или окна под значком (ниже панели) — закрыть его; сам щелчок дальше не идёт
        y: DeskStyle.panelH
        width: parent.width
        height: parent.height - y
        z: 1001
        enabled: bar.menuOpen || desk.soundOpen || desk.btOpen || desk.inetOpen || desk.batteryOpen || desk.clipOpen
        Component.onCompleted: cursorShape = undefined   // без формы — как у той, что над меню браузера
        onPressed: {
            bar.menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.batteryOpen = false
            desk.clipOpen = false
        }
    }

    // Монитор системы — под индикатором подключения, поверх всех окон; окна звука, Bluetooth и «Интернет» (ниже по тексту — выше
    // по слоям) ложатся на него. Loader: ошибка в окне монитора не роняет стол, а до первого открытия его нет вовсе.
    // Считает (naomiMonitor.active), пока открыт и стол смотрят
    Loader {
        id: monitorLoader
        z: 1002
        active: desk.monitorBuilt
        // оттащили — где оставили, но целиком на столе и ниже панели (стол сменил размер — окно едет в него следом)
        readonly property real homeX: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.netRight + 6 - width))
        readonly property real homeY: DeskStyle.panelH + DeskStyle.popGap
        x: !desk.monitorMoved ? homeX
           : Math.max(DeskStyle.popGap, Math.min(desk.width - DeskStyle.popGap - width, desk.monitorAt.x))
        y: !desk.monitorMoved ? homeY
           : Math.max(homeY, Math.min(desk.height - DeskStyle.popGap - height, desk.monitorAt.y))
        source: "MonitorPopover.qml"
        Connections {
            target: monitorLoader.item
            function onMovedTo(x, y) {
                if (!desk.monitorMoved && Math.abs(x - monitorLoader.homeX) < 3 && Math.abs(y - monitorLoader.homeY) < 3)
                    return   // щелчок с дрожанием руки — не перенос
                desk.monitorAt = Qt.point(x, y)
                desk.monitorMoved = true
            }
            function onHome() { desk.monitorMoved = false }
        }
        onLoaded: {
            item.area = desk
            item.route = Qt.binding(() => naomiStream.route)
            item.mbps = Qt.binding(() => naomiStream.mbps)
            item.androidState = Qt.binding(() => naomiAndroid.state)
            item.androidAsleep = Qt.binding(() => naomiAndroid.asleep)
            item.androidFrozen = Qt.binding(() => naomiAndroid.frozen)
            item.androidWindows = Qt.binding(() => (desk.windowsTick, windows.children.filter(w => w.android && !w.closing).length))
            item.open = Qt.binding(() => desk.monitorOpen)
        }
    }
    Binding {
        target: naomiMonitor
        property: "active"
        value: desk.monitorOpen && (DeskStyle.tablet ? naomiPower.displayOn
                             : naomiStream.connected && naomiStream.desktop && !naomiStream.deskAway)
               && (!monitorLoader.item || monitorLoader.item.view === "monitor")   // вид «Экраны» графиков не считает
    }

    BluetoothPopover {
        z: 1002
        x: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.btRight + 6 - width))
        y: DeskStyle.panelH + DeskStyle.popGap
        open: desk.btOpen
    }

    InternetPopover {
        z: 1002
        x: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.inetRight + 6 - width))
        y: DeskStyle.panelH + DeskStyle.popGap
        open: desk.inetOpen
    }

    BatteryPopover {
        z: 1002
        x: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.batteryRight + 6 - width))
        y: DeskStyle.panelH + DeskStyle.popGap
        open: desk.batteryOpen
        clock24h: uiSettings.clock24h
        battery: battery
    }

    ClipboardPopover {
        z: 1002
        x: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.clipRight + 6 - width))
        y: DeskStyle.panelH + DeskStyle.popGap
        open: desk.clipOpen
        onDismissed: desk.clipOpen = false
    }

    SoundPopover {
        z: 1002
        x: Math.max(DeskStyle.popGap, Math.min(desk.width - 8 - width, bar.soundRight + 6 - width))
        y: DeskStyle.panelH + DeskStyle.popGap
        open: desk.soundOpen
        output: desk.soundOut
        available: naomiPhone.available
        volume: desk.shownVolume
        muted: naomiPhone.audio.muted
        softVolume: naomiPhone.audio.softVolume
        stream: naomiStream
        onVolumeSet: (v) => desk.setVolume(v)
        onMuteToggled: naomiPhone.audio.call("SetMuted", [!naomiPhone.audio.muted])
        onOutputToggled: desk.toggleOutput()
        onWheel: (delta) => desk.soundWheel(delta)
    }

    // приложение Android открывается — от щелчка до окна (Android включается 15–20 с)
    AndroidLaunch {
        z: 1989
        x: Math.round((desk.width - width) / 2)
        y: DeskStyle.panelH + 18
        starting: naomiAndroid.state !== "on"
        names: Object.keys(desk.androidLaunching).map(pkg => desk.androidName(pkg))
    }

    DeskVolumeOsd {
        id: volumeOsd
        z: 1990   // поверх окон на весь экран (layer_osd)
        x: Math.round((desk.width - width) / 2)
        y: DeskStyle.panelH + 18
        volume: desk.shownVolume
        muted: naomiPhone.audio.muted
    }

    NMenu {
        id: nmenu
        x: DeskStyle.popGap
        y: DeskStyle.panelH + DeskStyle.popGap
        z: 1002
        open: bar.menuOpen
        forceItems: desk.forceList
        forceInfo: naomiForce.info
        onForceKill: (keys) => desk.forceKill(keys)
        filesOpen: desk.filesOpen
        maxHeight: desk.height - y - DeskStyle.popGap
        androidApps: naomiAndroid.apps
        androidOpen: desk.androidOpen
        androidLaunching: desk.androidLaunching
        androidState: naomiAndroid.state
        onAndroidActivated: (pkg) => desk.activateAndroid(pkg)
        linuxApps: naomiSystemTools.linuxApps
        onLinuxAppActivated: (app) => desk.activateLinuxApp(app)
        onLinuxAppsRefreshed: naomiSystemTools.refreshLinuxApps()
        games: naomiGames.games
        gameRunning: desk.gameNow ? desk.gameNow.path : ""
        gameOpen: desk.gameOpen
        onGameActivated: (game) => desk.activateGame(game)
        onGamePad: (game) => desk.openPadKeys(game)
        webOpen: desk.webOpen
        onWebClicked: {
            bar.menuOpen = false
            desk.activateWeb()
        }
        onFilesClicked: {
            bar.menuOpen = false
            desk.activateFiles()
        }
        termOpen: desk.termOpen
        onTermClicked: {
            bar.menuOpen = false
            desk.activateTerm()
        }
        claudeOpen: desk.claudeOpen
        onClaudeClicked: {
            bar.menuOpen = false
            desk.activateClaude()
        }
        telegramOpen: desk.telegramOpen
        onTelegramClicked: desk.activateTelegram()
        deepseekOpen: desk.deepseekOpen
        onDeepseekClicked: desk.activateDeepseek()
        onBootAndroidRequested: {
            bar.menuOpen = false
            confirmAndroid.active = true
        }
        onSystemAction: (action) => {
            bar.menuOpen = false
            if (action === "settings")
                desk.openSettings()
            else if (action === "sleep")
                naomiPower.sleep()
            else if (action === "leave")   // закрыть сессию — как плитка «трансляция» в шторке телефона
                naomiStream.enabled = false
            else   // перезагрузить телефон — службой железа (docs/NAOMID.md, «Система»)
                naomiPhone.system.call("Reboot")
        }
    }

    Loader {
        id: confirmAndroid
        anchors.fill: parent
        active: false
        z: 3010
        focus: active
        onLoaded: item.forceActiveFocus()
        sourceComponent: BootAndroidDialog {
            onDismissed: confirmAndroid.active = false
        }
    }

    DeskBar {
        id: bar
        z: 1003
        width: parent.width
        opacity: desk.chromeIn
        scale: 0.96 + 0.04 * desk.chromeIn
        transformOrigin: Item.Top
        filesOpen: desk.filesOpen
        // игра и окна программ Windows/Linux — перед приложениями Android
        androidApps: desk.gamePanel.concat(desk.winPanel, desk.claudePanel, desk.telegramPanel, desk.deepseekPanel, desk.linuxPanel, desk.androidPanel)
        onAndroidClicked: (pkg) => {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            if (pkg === "game")
                desk.toggleGame()
            else if (pkg.startsWith("win:"))
                desk.toggleWin(parseInt(pkg.substring(4)))
            else if (pkg.startsWith("claude:"))
                desk.toggleLinux(desk.claudeWindows[parseInt(pkg.substring(7))])
            else if (pkg.startsWith("telegram:"))
                desk.toggleLinux(desk.telegramWindows[parseInt(pkg.substring(9))])
            else if (pkg.startsWith("deepseek:"))
                desk.toggleLinux(desk.deepseekWindows[parseInt(pkg.substring(9))])
            else if (pkg.startsWith("linux:"))
                desk.toggleLinux(desk.linuxWindows[parseInt(pkg.substring(6))])
            else
                desk.toggleAndroid(pkg)
        }
        web: web
        webOpen: desk.webOpen
        webPanel: desk.webPanel
        onWebWindowClicked: {
            menuOpen = false
            desk.setWebPanel(false)
        }
        // крестик на панели — как у окна: щелчок — свернуть, с Shift — закрыть (браузер — выйти, Android — задачу прочь)
        onWebCloseClicked: (mods) => {
            menuOpen = false
            if (mods & Qt.ShiftModifier)
                naomiWeb.send({ type: "quit" })
            else if (desk.webWindow)
                desk.webWindow.minimized = true
        }
        term: term
        termOpen: desk.termOpen
        termPanel: desk.termPanel
        onTermClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.activateTerm()
        }
        onTermWindowClicked: {
            menuOpen = false
            desk.setTermPanel(false)
        }
        onTermCloseClicked: (mods) => {
            menuOpen = false
            if (mods & Qt.ShiftModifier)
                desk.quitTerm()
            else if (desk.termWindow)
                desk.termWindow.minimized = true
        }
        // окно Android «на весь экран» или игра «в доке»: «в окно» и крестик — на панели
        androidPanelShown: desk.panelTop !== null && (desk.panelTop.android === true || desk.panelTop.game === true)
        onAndroidWindowClicked: {
            menuOpen = false
            if (desk.panelTop && (desk.panelTop.android || desk.panelTop.game))
                desk.panelTop.togglePanel()
        }
        onAndroidCloseClicked: (mods) => {
            menuOpen = false
            if (desk.panelTop && (desk.panelTop.android || desk.panelTop.game))
                desk.panelTop.closeButton(mods)
        }
        soundOpen: desk.soundOpen
        btOpen: desk.btOpen
        inetOpen: desk.inetOpen
        monitorOpen: desk.monitorOpen
        onNetClicked: desk.monitorOpen = !desk.monitorOpen
        battery: battery
        batteryOpen: desk.batteryOpen
        onBatteryClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.batteryOpen = !desk.batteryOpen
        }
        onMenuClicked: {
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            menuOpen = !menuOpen
        }
        onSoundClicked: {
            menuOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.soundOpen = !desk.soundOpen
            if (desk.soundOpen)
                volumeOsd.hideNow()
        }
        onBtClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.inetOpen = false
            desk.btOpen = !desk.btOpen
        }
        onInetClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = !desk.inetOpen
        }
        onSoundWheel: (delta) => desk.soundWheel(delta)
        onFilesClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.activateFiles()
        }
        onWebClicked: {
            menuOpen = false
            desk.soundOpen = false
            desk.btOpen = false
            desk.inetOpen = false
            desk.activateWeb()
        }
        onKeyboardClicked: desk.toggleKeyboard()
        onScreenshotClicked: desk.screenshot(Qt.rect(0, 0, 0, 0))
        clipOpen: desk.clipOpen
        onClipClicked: desk.clipOpen = !desk.clipOpen
        clock24h: uiSettings.clock24h
        clockSeconds: uiSettings.clockSeconds
        dateFormat: uiSettings.dateFormat
    }

    // Ночной тёплый фильтр экрана: сначала пробуем аппаратную 4×4 матрицу HWC2 (0% нагрузки GPU),
    // иначе мягкий янтарный QML-слой поверх стола.
    Connections {
        target: uiSettings
        function onNightFilterChanged() { naomiDisplay.setNightWarmth(uiSettings.nightFilter) }
    }
    Rectangle {
        z: 9999
        anchors.fill: parent
        visible: uiSettings.nightFilter > 0.01 && !naomiDisplay.hardwareNightFilter
        color: Qt.rgba(1.0, 0.50, 0.12, Math.min(0.32, uiSettings.nightFilter * 0.45))
    }
}
