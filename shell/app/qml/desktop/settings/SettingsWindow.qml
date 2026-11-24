// «Настройки» рабочего стола (docs/DESKTOP.md, «Настройки»): открываются шестерёнкой в меню N. Окно НаомиОС с одним
// крестиком (свои программы НаомиОС: свернуть — нечего), слева разделы — «Экран» и под ним
// «Обои» и «Без дока», «Мышь» и под ней «Подробно», «Трекпад» (22 ноября 2026 — карточки, вариант 04 из десяти; touch,
// trackpad.conf стола, по нему трекпад ведёт приставка — сообщение Pad), «Батарея» и под ней «Здоровье», «Геймпад»,
// справа открытый. Вид
// выбран пользователем 17 ноября 2026 из макетов (вариант 10, «Подробно» — из 9; «Экран» — свой макет того же дня;
// «Батарея» — 21 ноября, вариант 08 «Заряд и тепло»; её ведёт naomid, объект Charge; «Геймпад» — 21 ноября, что делать,
// когда его берут со станции и ставят: pad, gamepad.conf стола). Значения мыши — cfg (mouse.conf стола),
// экрана — screen (screen.conf, Desktop.qml); курсор, колесо, размер стола, частоту монитора и экран без дока по ним
// ведёт приставка (сообщения Mouse и Screen).
// Разделы слева — «Шкала» (NavScale; 21 ноября 2026, вариант 08 из десяти). Черту между ними и разделом тянут мышью —
// панель шире или уже, раздел справа той же ширины (окно растёт вправо). Высоту окна задаёт пользователь — тянет за
// нижний край (просьба того же дня); раздел выше окна крутится колесом или полосой «Нить» справа. Ширина панели и
// высота помнятся в windows.conf (категория settings).
import QtQuick
import QtCore
import ".."

DeskWindow {
    id: sw
    tool: true
    property var cfg                         // Settings мыши (Desktop.qml)
    property var screen                      // Settings экрана (Desktop.qml)
    property var pad                         // Settings геймпада (Desktop.qml)
    property var touch                       // Settings трекпада (Desktop.qml)
    property var ui                          // Settings интерфейса (Desktop.qml)
    property var term                        // Settings терминала (Desktop.qml)
    property Item desk
    property string page: "screen"           // screen, wall, interface, lock, mouse, more, touch, battery, health, pad, system, vpn
    // панель слева: ширина — от sideMin до sideMax, раздел справа всегда pageW (как было: окно 880 при панели 170)
    readonly property int sideMin: 140
    readonly property int sideMax: 340
    readonly property int pageW: 710
    Settings {
        id: place
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/naomi/desktop/windows.conf"
        category: "settings"
        property int side: 190
        property int height: 640
    }
    property real sideW: Math.max(sideMin, Math.min(sideMax, place.side))
    // высота — не ниже панели слева (она влезает целиком) и не выше стола: открывается на 24 ниже панели, снизу — 12.
    // Стол стал ниже (окно Mac) — окно ниже, своё число в файле остаётся
    readonly property real minH: barH + 16 + nav.usedHeight + 16
    readonly property real maxH: area ? area.height - 36 : 900
    width: DeskStyle.tablet && area ? Math.min(sideW + pageW, area.width) : sideW + pageW
    height: Math.max(minH, Math.min(maxH, place.height))

    barContent: Text {
        x: 22
        height: sw.barH
        verticalAlignment: Text.AlignVCenter
        text: "НАСТРОЙКИ"
        font.family: DeskStyle.mono
        font.pixelSize: DeskStyle.caps
        font.weight: Font.Medium
        font.letterSpacing: DeskStyle.caps * 0.2
        color: DeskStyle.amber
    }

    // ---------- наборы и подписи (общие для «Мыши» и «Подробно») ----------
    // «Обычно» — как было до настроек (приставка без сообщения Mouse ведёт себя так же)
    readonly property var presets: [
        { id: "tochno", name: "Точно", speed: 35, lines: 2, soft: 100, smooth: true, accel: 0, wheelAccel: false },
        { id: "obychno", name: "Обычно", speed: 50, lines: 3, soft: 130, smooth: true, accel: 1, wheelAccel: false },
        { id: "bystro", name: "Быстро", speed: 70, lines: 5, soft: 200, smooth: true, accel: 2, wheelAccel: true }]
    readonly property string preset: {
        const c = cfg
        for (const p of presets)
            if (c.speed === p.speed && c.lines === p.lines && c.soft === p.soft && c.smooth === p.smooth
                    && c.accel === p.accel && c.wheelAccel === p.wheelAccel)
                return p.id
        return "svoe"
    }
    readonly property string presetName: preset === "svoe" ? "Своё" : presets.find(p => p.id === preset).name
    function applyPreset(id) {
        const p = presets.find(o => o.id === id)
        if (!p)
            return
        cfg.speed = p.speed; cfg.lines = p.lines; cfg.soft = p.soft
        cfg.smooth = p.smooth; cfg.accel = p.accel; cfg.wheelAccel = p.wheelAccel
    }
    function presetLines(p) {
        return [p.speed + " % · " + p.lines + " стр", "мягкость " + p.soft + " мс",
                p.accel === 0 ? "без ускорения" : p.accel === 1 ? "ускорение мягкое" : "сильное + разгон"]
    }
    function linesWord(n) {
        const m = n % 10, h = n % 100
        return h > 10 && h < 20 ? "строк" : m === 1 ? "строка" : m > 1 && m < 5 ? "строки" : "строк"
    }
    function softWord(ms) { return ms < 110 ? "почти сразу" : ms <= 220 ? "мягко" : "очень плавно" }
    function accelName(a) { return a === 0 ? "без ускорения" : a === 1 ? "ускорение мягкое" : "ускорение сильное" }
    function accelWord(a) {
        return a === 0 ? "выкл: курсор идёт ровно за рукой" : a === 1 ? "мягкое: быстрое движение уносит дальше"
                       : "сильное: рывок рукой — через весь стол"
    }
    function hint() {
        return cfg.natural ? "колесо к себе — страница вверх, как пальцем по экрану" : "колесо к себе — страница вниз"
    }
    // как у приставки (pi/dock/input.cpp, main.cpp): ускорение от скорости руки (отсчётов мыши в мс) и множитель скорости
    function gain(v, accel) {
        if (accel === 0)
            return 1
        const t = Math.max(0, Math.min(1, (v - 0.3) / 2.7))
        return 1 + (accel === 2 ? 3 : 1.5) * t * t * (3 - 2 * t)
    }
    function speedFactor(speed) { return Math.pow(2, (speed - 50) / 33.3) }

    // ---------- экран: как считает приставка (pi/dock/main.cpp, deskFor) ----------
    // монитор — что она назвала (сейчас или в прошлый раз); не называла — 2560 × 1440, 60 Гц (как у пользователя)
    readonly property var monitor: {
        const live = naomiStream.monitor
        if (live && live.w)
            return live
        try {
            const m = JSON.parse(screen.monitor)
            if (m && m.w)
                return m
        } catch (e) {}
        return { name: "", port: "", w: 2560, h: 1440, hz: 60, rates: [60] }
    }
    readonly property bool monitorKnown: monitor.port !== ""
    readonly property bool docked: naomiStream.connected && naomiStream.viewers.indexOf("pi") >= 0
    readonly property var sizes: [1280, 1440, 1600, 1800, 2048]
    readonly property var sharpShare: [0.75, 0.84375, 0.9375, 1]
    readonly property var sharpNames: ["Лёгкая", "Средняя", "Чёткая", "Родная"]
    function deskH(w) { return Math.max(720, Math.min(1280, Math.round(w * monitor.h / monitor.w / 4) * 4)) }
    function frameOf(sharp) { return Math.min(2560, Math.round(monitor.w * sharpShare[sharp])) }
    function sharpOk(w, sharp) { const k = frameOf(sharp) / w; return k >= 1 && k <= 2 }
    function scaleOf(w, sharp) { return Math.min(Math.max(1, Math.min(2, frameOf(sharp) / w)), 1600 / deskH(w)) }
    function frameSize(w, sharp) {   // как у телефона (StreamService::Worker::resizeDesktop): чётные
        const k = Math.round(scaleOf(w, sharp) * 1000) / 1000
        return [Math.round(w * k / 2) * 2, Math.round(deskH(w) * k / 2) * 2]
    }
    function pct(w) { return Math.round(monitor.w / w * 100) }
    function comma(v, digits) { return (+v.toFixed(digits)).toString().replace(".", ",") }
    // замер 15 ноября 2026 (docs/PI.md, «Размер и масштаб»): стол 1600 точек, монитор 75 Гц, кабель USB 2 — по ширине кадра
    readonly property var measured: ({
        1920: { scroll: 84, enc: "7", lat: "38,5" }, 2160: { scroll: 83, enc: "8,3", lat: "40,0" },
        2400: { scroll: 81, enc: "10", lat: "41,8" }, 2560: { scroll: 73, enc: "10,8", lat: "41,1" } })
    // частоты монитора — целыми, без повторов (59,95 и 60,00 — одна); кино-режимы ниже 48 — прочь
    readonly property var rates: {
        const out = []
        for (const r of (monitor.rates || [monitor.hz])) {
            const v = Math.round(r)
            if (v >= 48 && out.indexOf(v) < 0)
                out.push(v)
        }
        return out.length ? out.sort((a, b) => a - b) : [60]
    }
    function hzWord(hz) {
        return hz <= 60 ? "браузер рисует 60 к/с — с 60 Гц листание и видео ровные"
                        : "курсор и окна глаже, но браузер рисует 60 к/с — часть обновлений повтор"
    }
    function minutesWord(s) { return s >= 3600 ? s / 3600 + " ч" : s / 60 + " мин" }
    function sleepWord(s, dim) {   // без дока, без дела: сон или приглушение
        return s === 0 ? (dim ? "не тускнеет" : "не спит") : (dim ? "тусклее через " : "сон через ") + minutesWord(s)
    }

    // ---------- разделы ----------
    // Разделы — по требованию: открытый создаётся сразу, остальные — фоном по частям, когда стол в покое (или сразу,
    // если в раздел зашли раньше). Было — все пять при открытии окна: 111 мс в потоке интерфейса, стол стоял (прогон
    // стола 19 ноября 2026, docs/DESKTOP.md, «Прогон стола»)
    readonly property var loaders: ({ screen: screenLoad, wall: wallLoad, interface: ifaceLoad, lock: lockLoad,
                                      mouse: mouseLoad, more: moreLoad, touch: touchLoad, battery: batteryLoad,
                                      health: healthLoad, pad: padLoad, system: sysLoad, vpn: vpnLoad })
    readonly property var pageFiles: ({ screen: DeskStyle.tablet ? "TabletScreenPage.qml" : "ScreenPage.qml",
                                        wall: "WallPage.qml", interface: "InterfacePage.qml", lock: "LockPage.qml",
                                        mouse: "MousePage.qml", more: "MouseMorePage.qml", touch: "TrackpadPage.qml",
                                        battery: "BatteryPage.qml",
                                        health: "HealthPage.qml", pad: "GamepadPage.qml",
                                        system: "SystemPage.qml", vpn: "VpnPage.qml" })
    readonly property Item current: loaders[page] ? loaders[page].item : null
    property var loaded: ({})
    property int loadedCount: 0
    function loadPage(name) {   // sw — начальным свойством: он есть до первых привязок раздела
        if (loaded[name] || (name === "vpn" && !naomiSystemTools.vpnAvailable))
            return
        loaded[name] = true
        ++loadedCount
        loaders[name].setSource(Qt.resolvedUrl(pageFiles[name]), { sw: sw })
    }
    onPageChanged: {
        scroller.contentY = 0
        loadPage(page)
    }
    Component.onCompleted: loadPage(page)
    Timer {   // остальные разделы — когда окно появилось и стол в покое: фоном, по частям (asynchronous)
        interval: 600
        running: sw.shown && sw.loadedCount < 9
        repeat: true
        onTriggered: if (naomiDesk.idleMs() > 300) for (const n in sw.loaders) sw.loadPage(n)
    }
    Item {
        anchors.fill: parent
        Item {
            id: side
            width: sw.sideW
            height: parent.height
            Rectangle {   // черта; пока её тянут — ярче
                x: parent.width - 1; width: 1; height: parent.height
                color: DeskStyle.amberA(divider.pressed ? 0.36 : DeskStyle.line)
            }
            NavScale {
                id: nav
                x: 12; y: 16
                width: parent.width - 24
                page: sw.page
                onPick: (id) => sw.page = id
            }
        }
        MouseArea {   // черту тянут: 4 точки по обе стороны; шире — пока раздел справа влезает в стол
            id: divider
            x: side.width - 5
            width: 9
            height: parent.height
            z: 1
            cursorShape: Qt.SplitHCursor
            property real from
            property real start
            onPressed: (m) => {
                from = mapToItem(sw.parent, m.x, 0).x
                start = sw.sideW
            }
            onPositionChanged: (m) => {
                if (!pressed)
                    return
                const room = sw.area ? sw.area.width - sw.x - sw.pageW : sw.sideMax
                const w = start + mapToItem(sw.parent, m.x, 0).x - from
                sw.sideW = Math.round(Math.max(sw.sideMin, Math.min(sw.sideMax, room, w)))
            }
            onReleased: place.side = sw.sideW
        }
        MouseArea {   // колесо над разделом, который выше окна (поле проверки колесо забирает себе)
            x: side.width
            width: parent.width - x
            height: parent.height
            acceptedButtons: Qt.NoButton
            onWheel: (e) => {
                const d = e.pixelDelta.y !== 0 ? e.pixelDelta.y : e.angleDelta.y / 120 * 100
                scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, scroller.contentY - d))
            }
        }
        Flickable {
            id: scroller
            x: side.width
            width: parent.width - x
            height: parent.height
            clip: contentHeight > height
            interactive: DeskStyle.tablet
            acceptedButtons: DeskStyle.tablet ? Qt.NoButton : Qt.LeftButton
            contentHeight: 22 + (sw.current ? sw.current.implicitHeight : 0) + 26
            // окно выше или раздел короче — не оставаться прокрученным за конец
            function fit() { contentY = Math.max(0, Math.min(contentY, contentHeight - height)) }
            onHeightChanged: fit()
            onContentHeightChanged: fit()
            Item {
                x: 26; y: 22
                width: scroller.width - 52
                // открытый раздел — сразу; зашли в тот, что собирается фоном, — досоздать сразу (asynchronous → false)
                Loader { id: screenLoad; width: parent.width; visible: sw.page === "screen"; asynchronous: !visible }
                Loader { id: wallLoad; width: parent.width; visible: sw.page === "wall"; asynchronous: !visible }
                Loader { id: ifaceLoad; width: parent.width; visible: sw.page === "interface"; asynchronous: !visible }
                Loader { id: lockLoad; width: parent.width; visible: sw.page === "lock"; asynchronous: !visible }
                Loader { id: mouseLoad; width: parent.width; visible: sw.page === "mouse"; asynchronous: !visible }
                Loader { id: moreLoad; width: parent.width; visible: sw.page === "more"; asynchronous: !visible }
                Loader { id: touchLoad; width: parent.width; visible: sw.page === "touch"; asynchronous: !visible }
                Loader { id: batteryLoad; width: parent.width; visible: sw.page === "battery"; asynchronous: !visible }
                Loader { id: healthLoad; width: parent.width; visible: sw.page === "health"; asynchronous: !visible }
                Loader { id: padLoad; width: parent.width; visible: sw.page === "pad"; asynchronous: !visible }
                Loader { id: sysLoad; width: parent.width; visible: sw.page === "system"; asynchronous: !visible }
                Loader { id: vpnLoad; width: parent.width; visible: sw.page === "vpn"; asynchronous: !visible }
            }
        }
        ThreadBar {   // «Нить» у правого края; снизу — мимо скругления угла окна
            x: parent.width - width - 2
            y: 6
            height: parent.height - 6 - 18
            flick: scroller
        }
    }
    EdgeGrip {   // нижний край — высота окна (курсор ↕); пишется сразу: запись в файл Settings копит сам
        sy: 1
        cursorShape: Qt.SizeVerCursor
        property real start
        property real from
        onPressed: (m) => {
            sw.activate()
            start = sw.height
            from = mapToItem(sw.parent, m.x, m.y).y
        }
        onPositionChanged: (m) => {
            if (pressed)
                place.height = Math.round(Math.max(sw.minH, Math.min(sw.maxH, start + mapToItem(sw.parent, m.x, m.y).y - from)))
        }
    }
}
