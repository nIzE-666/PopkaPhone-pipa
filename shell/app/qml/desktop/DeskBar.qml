// Панель сверху — как у рабочего стола НаомиОС (bcdesk panel_redraw_bar): уголь почти сплошной, волосяная янтарная
// линия снизу. Слева — буква N (меню системы), черта и FILE — «Файлы» (bar_draw_apps: закреплена, видна и без окон;
// под подписью черта — окна на виду, вполсилы — все свёрнуты), WEB, TERM — «Терминал», пока он открыт
// (docs/TERMINAL.md), и за ними — открытые приложения Android
// (docs/ANDROID.md, «Полоса окна и панель»), справа налево: время и дата, черта, раскладка, батарея (BatteryIcon;
// щелчок — окно батареи, BatteryPopover), интернет телефона, Bluetooth, звук и — своё, НаомиФона — по чему идёт картинка на Mac: кабель или Wi-Fi
// (naomiStream.route: телефон узнаёт по своему ядру, на какой интерфейс пришло подключение; меняется сам, когда окно
// переходит). Значки — состояние телефона (naomid): интернет — три узла (по кабелю) или веер Wi-Fi, на связи —
// бирюзой, сила Wi-Fi — дугами; Bluetooth на связи —
// бирюзой, выключен — тускло; звук — куда он идёт: колонки (монитор приставки), телефон или наушники, без звука — с
// чертой. Звук нажимается — окно звука (SoundPopover), колесо над ним — громкость ±5; интернет — окно «Интернет»
// (InternetPopover); Bluetooth — окно Bluetooth (BluetoothPopover); раскладка — EN / RU зрителя (naomiStream.layout:
// клавиши в буквы переводит приставка), щелчок — сменить, как у НаомиОС (keyboard_next_layout).
import QtQuick
import Naomi.Ui
import "term"
import "web"

Item {
    id: bar
    property bool menuOpen: false
    property real filesOpen: 0     // черта под FILE: 1, 0,5 или нет
    signal menuClicked()
    signal filesClicked()
    signal webClicked()
    signal webWindowClicked()
    // открытые приложения Android (docs/ANDROID.md, «Полоса окна и панель») и перед ними идущая игра (docs/GAMES.md,
    // pkg «game»): [{ pkg, name, open }], open — 1 окна на виду, 0,5 — все свёрнуты; щелчок — androidClicked
    property var androidApps: []
    signal androidClicked(string pkg)
    property var web: null         // WebState браузера (docs/WEB.md)
    property real webOpen: 0       // черта под WEB: 1 — окно на виду, 0,5 — свёрнуто, 0 — браузера нет
    property bool webPanel: false  // режим панели и окно на виду: вкладки браузера — здесь, фон сплошной
    // «Терминал» (docs/TERMINAL.md): TERM за WEB, пока он открыт (черта — как у WEB); в режиме дока — его вкладки здесь
    property var term: null        // TermState
    property real termOpen: 0
    property bool termPanel: false
    signal termClicked()
    signal termWindowClicked()
    signal termCloseClicked(int modifiers)
    property bool soundOpen: false // окно звука открыто: значок подсвечен, как под мышью
    signal soundClicked()
    signal soundWheel(real delta)   // колесо над значком звука: сдвиг в точках стола (щелчок колеса — 100)
    readonly property real soundRight: sound.x + sound.width   // от него — окно звука (sound_button_right)
    property bool btOpen: false    // окно Bluetooth открыто: значок подсвечен, как под мышью
    signal btClicked()
    property bool inetOpen: false  // окно «Интернет» открыто — так же
    signal inetClicked()
    readonly property real inetRight: inetItem.x + inetItem.width   // от него — окно «Интернет»
    // индикатор подключения (кабель или Wi-Fi) — он же кнопка монитора системы (MonitorPopover): щелчок — открыть,
    // ещё щелчок — убрать
    property bool monitorOpen: false
    signal netClicked()
    readonly property real netRight: net.x + net.width
    readonly property real btRight: bt.x + bt.width            // от него — окно Bluetooth (bt_button_right)
    height: DeskStyle.panelH

    readonly property real by: (DeskStyle.panelH - DeskStyle.appH) / 2
    readonly property real mid: DeskStyle.panelH / 2
    readonly property bool compact: DeskStyle.tablet && width <= 900
    readonly property int groupGap: compact ? 12 : DeskStyle.sepGap

    Rectangle {   // фон — сплошной (у НаомиОС — .92, N_BAR_ALPHA; на столе прозрачности нет — docs/DESKTOP.md)
        anchors.fill: parent
        color: DeskStyle.bg
    }
    Rectangle {   // линия снизу
        y: DeskStyle.panelH - 1
        width: parent.width; height: 1
        color: DeskStyle.amberA(DeskStyle.line)
    }

    component Sep: Rectangle {   // черта между группами
        y: bar.by + 5
        width: 1; height: DeskStyle.appH - 10
        color: DeskStyle.amberA(DeskStyle.line)
    }
    component BarText: Text {
        font.family: DeskStyle.mono
        font.pixelSize: DeskStyle.barText
        font.letterSpacing: DeskStyle.barSpacing
        height: DeskStyle.panelH
        verticalAlignment: Text.AlignVCenter
    }

    // ---------- N ----------
    BarItem {
        id: nItem
        x: DeskStyle.panelSide; y: bar.by
        width: DeskStyle.appH
        open: bar.menuOpen
        onClicked: bar.menuClicked()
        Glyph {
            kind: "n"
            color: DeskStyle.amber
            x: parent.width / 2
            y: DeskStyle.appH / 2 + nItem.dy
        }
    }

    // ---------- черта и FILE ----------
    readonly property real nInk: (DeskStyle.icon - DeskStyle.iconLine) * 5 / 6 + DeskStyle.iconLine
    Sep {
        id: sepApps
        x: bar.compact ? nItem.x + nItem.width + 4
                       : DeskStyle.panelSide + (DeskStyle.appH + bar.nInk) / 2 + bar.groupGap
    }
    TextMetrics { id: fileWidth; font: time.font; text: "FILE" }
    BarItem {
        id: fileItem
        x: sepApps.x + bar.groupGap - DeskStyle.itemPad
        y: bar.by
        width: Math.ceil(fileWidth.tightBoundingRect.width) + 2 * DeskStyle.itemPad
        onClicked: bar.filesClicked()
        BarText {
            x: DeskStyle.itemPad - fileWidth.tightBoundingRect.x
            y: -bar.by + fileItem.dy
            text: "FILE"
            color: fileItem.hovered || bar.filesOpen >= 1 ? DeskStyle.text : DeskStyle.muted
        }
        Rectangle {   // черта: окна «Файлов» на виду — ярко, все свёрнуты — вполсилы
            visible: bar.filesOpen > 0
            x: DeskStyle.itemPad
            y: DeskStyle.appH - 5 + fileItem.dy
            width: parent.width - 2 * DeskStyle.itemPad
            height: 2
            color: DeskStyle.amberA(bar.filesOpen)
        }
    }

    TextMetrics { id: webWidth; font: time.font; text: "WEB" }
    BarItem {
        id: webItem
        x: fileItem.x + fileItem.width + DeskStyle.itemGap
        y: bar.by
        width: Math.ceil(webWidth.tightBoundingRect.width) + 2 * DeskStyle.itemPad
        onClicked: bar.webClicked()
        BarText {
            x: DeskStyle.itemPad - webWidth.tightBoundingRect.x
            y: -bar.by + webItem.dy
            text: "WEB"
            color: webItem.hovered || bar.webOpen >= 1 ? DeskStyle.text : DeskStyle.muted
        }
        Rectangle {   // черта: окно на виду — ярко, свёрнуто — вполсилы; в режиме панели — у открытой вкладки рядом
            visible: bar.webOpen > 0 && !bar.webPanel
            x: DeskStyle.itemPad
            y: DeskStyle.appH - 5 + webItem.dy
            width: parent.width - 2 * DeskStyle.itemPad
            height: 2
            color: DeskStyle.amberA(bar.webOpen)
        }
    }

    TextMetrics { id: termWidth; font: time.font; text: "TERM" }
    BarItem {
        id: termItem
        visible: bar.termOpen > 0
        x: webItem.x + webItem.width + DeskStyle.itemGap
        y: bar.by
        width: Math.ceil(termWidth.tightBoundingRect.width) + 2 * DeskStyle.itemPad
        onClicked: bar.termClicked()
        BarText {
            x: DeskStyle.itemPad - termWidth.tightBoundingRect.x
            y: -bar.by + termItem.dy
            text: "TERM"
            color: termItem.hovered || bar.termOpen >= 1 ? DeskStyle.text : DeskStyle.muted
        }
        Rectangle {   // черта: окно на виду — ярко, свёрнуто — вполсилы; в режиме дока — у открытой вкладки рядом
            visible: !bar.termPanel
            x: DeskStyle.itemPad
            y: DeskStyle.appH - 5 + termItem.dy
            width: parent.width - 2 * DeskStyle.itemPad
            height: 2
            color: DeskStyle.amberA(bar.termOpen)
        }
    }
    readonly property real pinnedRight: termItem.visible ? termItem.x + termItem.width : webItem.x + webItem.width

    // ---------- открытые приложения Android — за закреплёнными, как открытые программы у НаомиОС (bar_draw_apps):
    // подпись — имя заглавными (длинное — с многоточием, не шире 180), черта — как у FILE и WEB ----------
    Flickable {
        id: appStrip
        x: bar.pinnedRight + DeskStyle.itemGap
        y: bar.by
        width: DeskStyle.tablet ? Math.min(contentWidth, Math.max(0, bar.appsLimit - x)) : contentWidth
        height: DeskStyle.appH
        contentWidth: androidRow.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: DeskStyle.tablet
        interactive: DeskStyle.tablet && contentWidth > width
        Row {
            id: androidRow
            spacing: DeskStyle.itemGap
            Repeater {
                model: bar.androidApps
                delegate: BarItem {
                    id: appItem
                    required property var modelData
                    readonly property string label: modelData.name.toUpperCase()
                    TextMetrics { id: appWidth; font: time.font; text: appItem.label }
                    // как у FILE и WEB: по видимым буквам (межбуквенный интервал шрифт панели уже содержит); подпись —
                    // своей шириной, многоточие — только длинной (у FILE за краем последней буквы — ещё интервал)
                    readonly property bool tooLong: appWidth.tightBoundingRect.width > 180
                    width: Math.ceil(tooLong ? 180 : appWidth.tightBoundingRect.width) + 2 * DeskStyle.itemPad
                    onClicked: bar.androidClicked(modelData.pkg)
                    BarText {
                        x: DeskStyle.itemPad - appWidth.tightBoundingRect.x
                        y: -bar.by + appItem.dy
                        width: appItem.tooLong ? 180 : implicitWidth
                        elide: Text.ElideRight
                        text: appItem.label
                        color: appItem.hovered || appItem.modelData.open >= 1 ? DeskStyle.text : DeskStyle.muted
                    }
                    Rectangle {   // черта: окна на виду — ярко, все свёрнуты — вполсилы
                        x: DeskStyle.itemPad
                        y: DeskStyle.appH - 5 + appItem.dy
                        width: parent.width - 2 * DeskStyle.itemPad
                        height: 2
                        color: DeskStyle.amberA(appItem.modelData.open)
                    }
                }
            }
        }
    }
    readonly property real appsRight: appStrip.width > 0 ? appStrip.x + appStrip.width : bar.pinnedRight
    readonly property real appsLimit: {
        if (bar.webPanel || bar.termPanel) {
            const tabsLimit = bar.windowX - 2 - DeskStyle.webFieldMinW - 8 - bar.groupGap + DeskStyle.itemPad
            if (tabsLimit >= bar.pinnedRight)
                return tabsLimit
        }
        if (bar.webPanel || bar.termPanel || bar.androidPanelShown)
            return bar.windowX - 12 - bar.groupGap + DeskStyle.itemPad
        return net.x - DeskStyle.itemGap
    }

    // ---------- на весь экран (bar_draw_web): у браузера в режиме панели — черта, вкладки и «+»; у окна Android «на весь
    // экран» (docs/ANDROID.md, «На весь экран») — без вкладок. Справа у обоих — «в окно» (обычный значок панели) и
    // крестик (своё, НаомиФона: у НаомиОС его нет — окно браузера там закрывали из окна), черта ----------
    readonly property real webSep: bar.appsRight - DeskStyle.itemPad + bar.groupGap
    readonly property real webSepRight: (DeskStyle.tablet ? net.x : net.visible ? net.cx - bar.routeHalf
                                                       : sound.cx - bar.soundHalf) - bar.groupGap
    readonly property real closeX: webSepRight - (DeskStyle.tablet ? DeskStyle.appH + DeskStyle.itemGap : 42)
    readonly property real windowX: closeX - DeskStyle.appH - DeskStyle.itemGap
    readonly property real webTabsLeft: webSep + 8
    readonly property real webTabsRight: windowX - 2
    readonly property bool webTabsShown: bar.webPanel && bar.web !== null && webTabsRight - webTabsLeft >= DeskStyle.webFieldMinW
    readonly property bool termTabsShown: bar.termPanel && bar.term !== null
                                          && webTabsRight - webTabsLeft >= DeskStyle.webFieldMinW
    property bool androidPanelShown: false   // верхнее на виду — окно Android «на весь экран» или игра «в доке»
    readonly property bool webButtons: bar.webTabsShown || DeskStyle.tablet && bar.webPanel
    readonly property bool termButtons: bar.termTabsShown || DeskStyle.tablet && bar.termPanel
    readonly property bool androidButtons: bar.androidPanelShown && !bar.webButtons && !bar.termButtons
                                           && windowX - 12 - bar.groupGap >= bar.appsRight - DeskStyle.itemPad
    readonly property bool panelButtons: bar.webButtons || bar.termButtons || bar.androidButtons
    signal webCloseClicked(int modifiers)
    signal androidWindowClicked()
    signal androidCloseClicked(int modifiers)
    Sep { visible: bar.webTabsShown || bar.termTabsShown; x: Math.round(bar.webSep) }
    Loader {
        active: bar.termTabsShown
        x: bar.webTabsLeft; y: bar.by
        width: bar.webTabsRight - bar.webTabsLeft
        sourceComponent: TermStrip {
            width: bar.webTabsRight - bar.webTabsLeft
            tabs: naomiTerm.tabs
            active: naomiTerm.active
            renaming: bar.term.renaming
            onTabPressed: (id, button) => bar.term.pressed(id, button)
            onTabDoubleClicked: (id) => bar.term.startRename(id)
            onTabClosed: (id) => bar.term.close(id)
            onTabRenamed: (id, name) => bar.term.rename(id, name)
            onRenameCancelled: bar.term.cancelRename()
            onPlusClicked: bar.term.plus()
        }
    }
    Loader {
        active: bar.webTabsShown
        x: bar.webTabsLeft; y: bar.by
        width: bar.webTabsRight - bar.webTabsLeft
        sourceComponent: WebStrip {
            width: bar.webTabsRight - bar.webTabsLeft
            st: bar.web
            withStar: false
        }
    }
    Sep { visible: bar.androidButtons; x: Math.round(bar.windowX - 12) }   // у Android вкладок нет — черта у кнопок
    BarItem {   // «в окно»
        id: windowItem
        visible: bar.panelButtons
        x: bar.windowX; y: bar.by
        width: DeskStyle.appH
        onClicked: bar.termButtons ? bar.termWindowClicked() : bar.webButtons ? bar.webWindowClicked()
                                                                            : bar.androidWindowClicked()
        Glyph {
            x: parent.width / 2; y: DeskStyle.appH / 2 + windowItem.dy
            kind: "panel"
            unit: 1.136
            line: 1.5
            color: windowItem.hovered ? DeskStyle.text : DeskStyle.muted
        }
    }
    BarItem {   // крестик: щелчок — свернуть, с Shift — закрыть, как у крестика окна
        id: closeItem
        visible: bar.panelButtons
        danger: true
        x: bar.closeX; y: bar.by
        width: DeskStyle.appH
        onClicked: (mods) => bar.termButtons ? bar.termCloseClicked(mods) : bar.webButtons ? bar.webCloseClicked(mods)
                                                                                         : bar.androidCloseClicked(mods)
        Glyph {
            x: parent.width / 2; y: DeskStyle.appH / 2 + closeItem.dy
            kind: "close"
            unit: 11 * 1.136 / 9   // клетка — как у «в окно» (у окна так же: 0,9545 × 11 = 5,25 / 4,5 × 9)
            line: 1.5
            color: closeItem.hovered ? DeskStyle.bad : DeskStyle.muted
        }
    }
    Sep { visible: bar.panelButtons; x: Math.round(bar.webSepRight) }

    // ---------- справа: время, дата ----------
    property bool clock24h: true
    property bool clockSeconds: false
    property int dateFormat: 1          // 0 — скрыть, 1 — ВТ 24 НОЯ, 2 — 24.11, 3 — TUE 24
    property date now: new Date()
    Timer {   // минута (или секунда, если включены секунды) сменилась — новое время; при выключенном экране не тикаем!
        running: naomiPower.displayOn
        repeat: true
        interval: bar.clockSeconds ? 1000 : (60 - bar.now.getSeconds()) * 1000 + 50
        onTriggered: bar.now = new Date()
    }
    Connections {
        target: naomiPower
        function onDisplayOnChanged() {
            if (naomiPower.displayOn)
                bar.now = new Date()
        }
    }
    readonly property string timeText: {
        const h = now.getHours(), m = now.getMinutes(), s = now.getSeconds()
        const mm = (m < 10 ? "0" : "") + m
        const ss = bar.clockSeconds ? ":" + (s < 10 ? "0" : "") + s : ""
        if (bar.clock24h)
            return (h < 10 ? "0" : "") + h + ":" + mm + ss
        return (h % 12 === 0 ? 12 : h % 12) + ":" + mm + ss + (h < 12 ? " AM" : " PM")
    }
    readonly property string dateText: {
        if (bar.dateFormat === 0)
            return ""
        const d = now.getDate(), mo = now.getMonth()
        if (bar.dateFormat === 2)
            return (d < 10 ? "0" : "") + d + "." + (mo + 1 < 10 ? "0" : "") + (mo + 1)
        if (bar.dateFormat === 3)
            return ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"][now.getDay()] + " " + d
        const days = ["ВС", "ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ"]
        const mons = ["ЯНВ", "ФЕВ", "МАР", "АПР", "МАЙ", "ИЮН", "ИЮЛ", "АВГ", "СЕН", "ОКТ", "НОЯ", "ДЕК"]
        return days[now.getDay()] + " " + d + " " + mons[mo]
    }

    BarText {
        id: time
        x: bar.width - DeskStyle.panelSide - implicitWidth
        text: bar.timeText
        color: DeskStyle.text
    }
    BarText {
        id: date
        visible: !bar.compact && bar.dateFormat !== 0
        x: time.x - 12 - implicitWidth
        text: bar.dateText
        color: DeskStyle.muted
    }
    Sep { id: sepClock; x: (date.visible ? date.x : time.x) - bar.groupGap }

    // ---------- раскладка: зрителя (сочетание клавиш у приставки меняет и её), клавиатуры устройства (HardKeyboard)
    // или на планшете — экранной клавиатуры (naomiKeyboard.layout); щелчок — сменить. Шрифт моноширинный: EN и RU
    // одной ширины, смена значки не двигает ----------
    readonly property string layout: naomiStream.layout !== "" ? naomiStream.layout
                                   : DeskStyle.tablet && naomiKeyboard !== null ? naomiKeyboard.layout : ""
    signal keyboardClicked()
    signal screenshotClicked()
    property bool clipOpen: false
    signal clipClicked()
    readonly property real clipRight: clipBtn.x + clipBtn.width
    TextMetrics { id: langWidth; font: time.font; text: "EN" }
    BarItem {
        id: lang
        width: Math.max(DeskStyle.tablet ? DeskStyle.appH : 0,
                        Math.ceil(langWidth.advanceWidth) + 2 * DeskStyle.barTextPad)
        x: sepClock.x - (bar.groupGap - DeskStyle.barTextPad) - width
        y: bar.by
        onClicked: {
            if (bar.layout === "")
                return
            const next = bar.layout === "RU" ? "EN" : "RU"
            if (naomiStream.layout !== "")
                naomiStream.setLayout(next)
            if (DeskStyle.tablet && naomiKeyboard !== null)
                naomiKeyboard.setLayout(next)
        }
        BarText {
            anchors.horizontalCenter: parent.horizontalCenter
            y: -bar.by + lang.dy
            text: bar.layout !== "" ? bar.layout : "EN"
            color: lang.hovered ? DeskStyle.text : DeskStyle.muted
        }
    }
    BarItem {
        id: keyboard
        visible: DeskStyle.tablet
        width: DeskStyle.appH
        x: lang.x - width
        y: bar.by
        open: DeskStyle.tablet && naomiKeyboard !== null && naomiKeyboard.visible
        Accessible.role: Accessible.Button
        Accessible.name: "Экранная клавиатура"
        function toggle() { bar.keyboardClicked() }
        Accessible.onPressAction: toggle()
        onClicked: toggle()
        Text {
            anchors.centerIn: parent
            font.family: "Material Symbols Rounded"
            font.pixelSize: 22
            color: keyboard.open ? DeskStyle.amber
                 : (typeof naomiSystemTools !== "undefined" && naomiSystemTools && naomiSystemTools.hardwareKeyboard) ? DeskStyle.teal
                 : DeskStyle.muted
            text: "keyboard"
        }
    }
    BarItem {
        id: rotate
        visible: DeskStyle.tablet
        width: DeskStyle.appH
        x: keyboard.x - width
        y: bar.by
        Accessible.role: Accessible.Button
        Accessible.name: "Повернуть экран (долгое нажатие — автоповорот)"
        Accessible.onPressAction: naomiDisplay.toggleOrientation()
        onClicked: naomiDisplay.toggleOrientation()
        onLongPressed: {
            if (typeof naomiHaptics !== "undefined" && naomiHaptics)
                naomiHaptics.tap(14)
            naomiDisplay.autoRotate = !naomiDisplay.autoRotate
        }
        Text {
            anchors.centerIn: parent
            font.family: "Material Symbols Rounded"
            font.pixelSize: 22
            color: rotate.hovered || naomiDisplay.autoRotate ? DeskStyle.amber : DeskStyle.muted
            text: naomiDisplay.autoRotate ? "screen_rotation" : "screen_lock_rotation"
        }
    }
    BarItem {
        id: shotBtn
        visible: DeskStyle.tablet
        width: DeskStyle.appH
        x: rotate.x - width
        y: bar.by
        Accessible.role: Accessible.Button
        Accessible.name: "Скриншот экрана"
        Accessible.onPressAction: bar.screenshotClicked()
        onClicked: bar.screenshotClicked()
        Glyph {
            kind: "camera"
            x: parent.width / 2
            y: DeskStyle.appH / 2 + shotBtn.dy
            color: shotBtn.hovered ? DeskStyle.amber : DeskStyle.muted
        }
    }
    BarItem {
        id: clipBtn
        visible: DeskStyle.tablet
        width: DeskStyle.appH
        x: shotBtn.x - width
        y: bar.by
        open: bar.clipOpen
        Accessible.role: Accessible.Button
        Accessible.name: "Буфер обмена"
        Accessible.onPressAction: bar.clipClicked()
        onClicked: bar.clipClicked()
        Glyph {
            kind: "clipboard"
            x: parent.width / 2
            y: DeskStyle.appH / 2 + clipBtn.dy
            color: clipBtn.hovered || bar.clipOpen ? DeskStyle.amber : DeskStyle.muted
        }
    }

    // ---------- батарея (docs/DESKTOP.md, «Батарея»): корпус с заливкой, слева — сколько ещё проработает от батареи, на
    // зарядке — процент (BatteryIcon); щелчок — окно батареи. Без службы железа (заряд ещё не пришёл) — не видна ----------
    property var battery: null     // DeskBattery
    property bool batteryOpen: false
    signal batteryClicked()
    readonly property bool batteryShown: battery !== null && naomiPhone.available && battery.percent > 0
    readonly property real batteryRight: batteryItem.x + batteryItem.width   // от него — окно батареи
    BarItem {
        id: batteryItem
        visible: bar.batteryShown
        width: batteryIcon.width + 2 * DeskStyle.barTextPad
        x: DeskStyle.tablet ? clipBtn.x - width : lang.x + 2 * DeskStyle.barTextPad - DeskStyle.sepGap - width
        y: bar.by
        open: bar.batteryOpen
        onClicked: bar.batteryClicked()
        BatteryIcon {
            id: batteryIcon
            x: DeskStyle.barTextPad
            y: DeskStyle.appH / 2 - height / 2 + batteryItem.dy
            battery: bar.battery
            font: time.font
            lit: batteryItem.hovered || bar.batteryOpen
        }
    }

    // ---------- интернет телефона: по кабелю через приставку — три узла бирюзой (docs/DESKTOP.md, «Интернет»); иначе
    // Wi-Fi: на связи — горящие по силе сигнала дуги бирюзой, остальные тускло; подключается — янтарём; включён без
    // сети — приглушённо, выключен — тускло. Место — одно на оба значка: смена пути панель не двигает ----------
    readonly property var wifi: naomiPhone.wifi
    readonly property bool cableNet: naomiPhone.cable.active
    readonly property bool wifiLinked: wifi.enabled && wifi.state === "connected"
    readonly property real inetHalf: Math.max(9.8 * Math.SQRT1_2, 7) + DeskStyle.iconLine / 2
    BarItem {
        id: inetItem
        readonly property real cx: (bar.batteryShown ? batteryItem.x : lang.x) + DeskStyle.barTextPad
                                   - DeskStyle.sepGap - bar.inetHalf
        x: DeskStyle.tablet ? (bar.batteryShown ? batteryItem.x : clipBtn.x) - width : cx - width / 2
        y: bar.by
        width: DeskStyle.appH
        open: bar.inetOpen
        onClicked: bar.inetClicked()
        Glyph {   // по кабелю
            visible: bar.cableNet
            kind: "lan"
            x: parent.width / 2
            y: DeskStyle.appH / 2 + inetItem.dy
            color: inetItem.hovered || bar.inetOpen ? DeskStyle.text : DeskStyle.teal
        }
        Glyph {   // погасшие дуги — на связи со слабым сигналом
            visible: !bar.cableNet && bar.wifiLinked
            kind: "wifi"
            arcFrom: DeskStyle.wifiArcs(bar.wifi.strength)
            x: parent.width / 2
            y: DeskStyle.appH / 2 + inetItem.dy
            color: DeskStyle.unlit
        }
        Glyph {
            visible: !bar.cableNet
            kind: "wifi"
            arcTo: bar.wifiLinked ? DeskStyle.wifiArcs(bar.wifi.strength) : 3
            x: parent.width / 2
            y: DeskStyle.appH / 2 + inetItem.dy
            color: inetItem.hovered || bar.inetOpen ? DeskStyle.text : !bar.wifi.enabled ? DeskStyle.dim
                 : bar.wifiLinked ? DeskStyle.teal : bar.wifi.state === "connecting" ? DeskStyle.amber : DeskStyle.muted
        }
    }

    // ---------- Bluetooth ----------
    readonly property var btConnected: naomiPhone.bluetooth.devices.find(d => d.connected)
    readonly property real btHalf: 6.25 * 0.55 + DeskStyle.iconLine / 2
    BarItem {
        id: bt
        readonly property real cx: inetItem.cx - bar.inetHalf - DeskStyle.sepGap - bar.btHalf
        x: DeskStyle.tablet ? inetItem.x - width : cx - width / 2
        y: bar.by
        width: DeskStyle.appH
        open: bar.btOpen
        onClicked: bar.btClicked()
        Glyph {
            kind: "bt"
            x: parent.width / 2
            y: DeskStyle.appH / 2 + bt.dy
            color: bt.hovered || bar.btOpen ? DeskStyle.text : !naomiPhone.bluetooth.powered ? DeskStyle.dim
                 : bar.btConnected ? DeskStyle.teal : DeskStyle.muted
        }
    }

    // ---------- звук: куда идёт — колонки («Динамики», монитор приставки), телефон или наушники ----------
    readonly property string soundOut: naomiPhone.audio.output
    readonly property string soundGlyph: soundOut === "desk" ? "speaker"
                                       : soundOut === "speaker" || soundOut === "earpiece" ? "phone" : "headphones"
    readonly property real soundUnit: (DeskStyle.icon - DeskStyle.iconLine) / 13.4   // значок звука — высотой N_ICON
    // место под значком — одно на все (по самому широкому, наушникам): колонки ↔ телефон панель не двигают (у bcdesk
    // место шло по ширине значка, и соседи сдвигались; пользователь: «двигает док»)
    readonly property real soundHalf: soundUnit * 7.7 + DeskStyle.iconLine / 2
    BarItem {
        id: sound
        readonly property real cx: bt.cx - bar.btHalf - DeskStyle.sepGap - bar.soundHalf
        x: DeskStyle.tablet ? bt.x - width : cx - width / 2
        y: bar.by
        width: DeskStyle.appH
        open: bar.soundOpen
        onClicked: bar.soundClicked()
        MouseArea {   // только колесо: кнопки — TapHandler элемента
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            Component.onCompleted: cursorShape = undefined   // форма — руки элемента (BarItem)
            onWheel: (e) => bar.soundWheel(e.pixelDelta.y !== 0 ? e.pixelDelta.y : e.angleDelta.y / 120 * 100)
        }
        Glyph {
            kind: bar.soundGlyph
            unit: bar.soundUnit
            slash: naomiPhone.audio.muted
            cut: DeskStyle.bg
            x: parent.width / 2
            y: DeskStyle.appH / 2 + sound.dy
            color: sound.hovered || bar.soundOpen ? DeskStyle.text : naomiPhone.audio.muted ? DeskStyle.dim : DeskStyle.muted
        }
    }

    // ---------- путь трансляции: монитор, у левого нижнего угла — вилка (кабель) или волны (Wi-Fi); монитор один,
    // панель не двигается ----------
    readonly property string route: naomiStream.route
    readonly property real routeHalf: 8.625 + DeskStyle.iconLine / 2   // правая сторона экрана — 8,625 от середины
    BarItem {
        id: net
        visible: DeskStyle.tablet || bar.route !== ""
        readonly property real cx: sound.cx - bar.soundHalf - DeskStyle.sepGap - bar.routeHalf
        x: DeskStyle.tablet ? sound.x - width : cx - width / 2
        y: bar.by
        width: DeskStyle.appH
        open: bar.monitorOpen
        onClicked: bar.netClicked()
        Glyph {
            kind: DeskStyle.tablet ? "monitor" : bar.route === "wifi" ? "route-wifi" : "route-cable"
            x: parent.width / 2
            y: DeskStyle.appH / 2 + net.dy
            color: net.hovered || bar.monitorOpen ? DeskStyle.text : DeskStyle.muted
        }
    }
}
