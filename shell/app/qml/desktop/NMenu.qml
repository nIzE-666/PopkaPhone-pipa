// Меню под буквой N — как system_popover_draw в bcdesk: «01 NAOMIOS» с четырьмя кнопками (закрыть принудительно,
// настройки, перезагрузить, закрыть сессию) и «02 APP» — плитки GAME, WEB, FILES, ANDROID, TERMINAL, CLAUDE, TELEGRAM (плиток
// TORRENT и POPKATV НаомиОС здесь нет — убраны по просьбе пользователя 21 ноября 2026). Черта снизу плитки — окна
// открыты, вполсилы — свёрнуты (как app_tile). WEB — браузер стола (docs/WEB.md), FILES — «Файлы», шестерёнка — «Настройки» стола, «перезагрузить» телефон — как в НаомиОС, со второго нажатия (первое красит
// кнопку красным и ждёт 4 с, SYS_CONFIRM_MS), и «закрыть сессию» — выйти из дока, как плиткой «трансляция» в шторке
// телефона (вместо «выключить» НаомиОС — просьба пользователя 17 ноября 2026; вернуться — клавишей у приставки):
// с одного нажатия. «Закрыть принудительно» (окно с крестиком) — как у НаомиОС: значок горит, а вместо «02 APP» и «03» —
// «02 FORCE QUIT» (ниже; docs/DESKTOP.md, «Закрыть принудительно»), ещё щелчок — обычное меню. Появляется как окна
// НаомиОС: растёт от верхнего края и проявляется.
//   GAME (docs/GAMES.md) и ANDROID (docs/ANDROID.md) открывают и закрывают модуль «03» под меню — как GAME у НаомиОС
// (плитка горит, пока он открыт; помнится до перезапуска оболочки; открыли один — другой закрылся). «03 GAMES» — игры
// из папок ~/Games (появляются сами, как только файл положили): строка — грань слева (янтарь — игра идёт, серая — нет),
// значок, имя, справа — система; щелчок — запустить или показать. «03 ANDROID» — приложения Android: так же, значок
// приложения 32; в шильдике — число, справа — «Настройки» Android и Google Play (на месте значка Steam).
//   TERMINAL (docs/TERMINAL.md) — «Терминал» стола; черта снизу — окно на виду, вполсилы — свёрнуто, как у FILES.
//   CLAUDE (docs/CLAUDE.md) — Claude Desktop для Linux; черта — так же.
import QtQuick

Item {
    id: menu
    property bool open: false
    property real filesOpen: 0
    signal filesClicked()
    property real termOpen: 0
    signal termClicked()
    property real webOpen: 0
    signal webClicked()
    property real claudeOpen: 0
    signal claudeClicked()
    property real telegramOpen: 0
    property string telegramError: ""
    signal telegramClicked()
    property real deepseekOpen: 0
    property string deepseekError: ""
    signal deepseekClicked()
    // игры: [{path, name, system, label}] (naomiGames.games); gameRunning — файл идущей игры, gameOpen — её окно: 1 на
    // виду, 0,5 свёрнуто или ещё запускается
    property var games: []
    property string gameRunning: ""
    property real gameOpen: 0
    property bool gamesShown: false     // модуль «03 GAMES» открыт плиткой
    signal gameActivated(var game)
    signal gamePad(var game)            // шестерёнка у игры: окно «Кнопки геймпада» (docs/GAMES.md)
    // Android: приложения [{pkg, name, icon}], окна — {пакет: 1 на виду, 0,5 свёрнуто}, запускаемые — {пакет: true},
    // state — naomiAndroid.state (off, starting, on, stopping)
    property var androidApps: []
    property var androidOpen: ({})
    property var androidLaunching: ({})
    property string androidState: "off"
    property bool androidShown: false   // модуль «03 ANDROID» открыт плиткой
    signal androidActivated(string pkg)
    property var linuxApps: []          // установленные Linux .desktop программы
    property bool linuxShown: false     // модуль «03 LINUX» открыт кнопкой в шапке «02 APP»
    property string linuxQuery: ""
    signal linuxAppActivated(var app)
    signal linuxAppsRefreshed()
    signal bootAndroidRequested()
    signal systemAction(string action)   // "reboot" — уже подтверждено вторым нажатием; "leave" — закрыть сессию;
                                         // "settings" — «Настройки»
    property string armed: ""            // кнопка, ждущая второго нажатия
    // «02 FORCE QUIT»: программы [{key, name}] (Desktop.qml, forceList), что о них известно — {key: {kb, hung}}
    // (naomiForce.info); открыт ли и кого выбрали (ключи); закрыть выбранные — forceKill
    property var forceItems: []
    property var forceInfo: ({})
    property bool forceOpen: false
    property var forceSelected: []
    signal forceKill(var keys)
    onOpenChanged: if (!open) {
        armed = ""
        forceOpen = false
        forceSelected = []
        linuxQuery = ""
    }
    Timer { id: armTimer; interval: 4000; onTriggered: menu.armed = "" }
    width: DeskStyle.sysW
    height: sys.height + DeskStyle.gamesGap + (forceOpen ? forceModule.height : apps.height
            + (androidShown ? DeskStyle.gamesGap + androidModule.height
             : gamesShown ? DeskStyle.gamesGap + gamesModule.height
             : linuxShown ? DeskStyle.gamesGap + linuxModule.height : 0))
    visible: opacity > 0
    opacity: open ? 1 : 0
    scale: open ? 1 : 0.96
    transformOrigin: Item.Top
    Behavior on opacity { NumberAnimation { duration: menu.open ? 180 : 140; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: menu.open ? 180 : 140; easing.type: Easing.OutCubic } }

    MouseArea { anchors.fill: parent }   // щелчок по самому меню — не мимо него

    readonly property real innerW: width - 2 * DeskStyle.padX
    readonly property int icon: DeskStyle.appH   // SYS_ICON

    // кнопка-значок (draw_icon_button): рамка, под мышью — заливка, значок приглушённо (под мышью — светлый); action —
    // перезагрузить: первое нажатие красит кнопку красным (заливка .10, под мышью .16, рамка .6), второе в течение 4 с
    // выполняет; once — сразу, с первого (закрыть сессию); lit — горит янтарём, как открытое окно («FORCE QUIT» открыт:
    // заливка .12, под мышью .16, рамка .45, значок янтарём; не проседает)
    component IconButton: Item {
        id: b
        property string glyph
        property string action
        signal activated()   // кнопка без действия меню (action "") — «03 ANDROID», «закрыть принудительно»
        property bool once: false
        property bool lit: false
        readonly property bool armed: action !== "" && menu.armed === action
        width: menu.icon; height: menu.icon
        readonly property real dy: area.pressed && !lit ? 1 : 0
        Rectangle {
            y: b.dy
            width: parent.width; height: parent.height
            color: b.armed ? DeskStyle.badA(area.containsMouse ? 0.16 : 0.10)
                 : b.lit ? DeskStyle.amberA(area.containsMouse ? 0.16 : 0.12)
                 : DeskStyle.amberA(area.pressed ? DeskStyle.itemPress : DeskStyle.itemHover)
            visible: b.armed || b.lit || area.containsMouse || area.pressed
        }
        Rectangle {
            y: b.dy
            width: parent.width; height: parent.height
            color: "transparent"
            border.width: 1
            border.color: b.armed ? DeskStyle.badA(0.6) : DeskStyle.amberA(b.lit ? 0.45 : DeskStyle.line)
        }
        Glyph {
            kind: b.glyph
            x: b.width / 2; y: b.height / 2 + b.dy
            color: b.armed ? DeskStyle.bad : b.lit ? DeskStyle.amber : area.containsMouse ? DeskStyle.text : DeskStyle.muted
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                if (b.action === "") {
                    b.activated()
                    return
                }
                if (b.once || b.armed) {
                    menu.armed = ""
                    menu.systemAction(b.action)
                } else {
                    menu.armed = b.action
                    armTimer.restart()
                }
            }
        }
    }

    // ---------- 01 NAOMIOS ----------
    DeskModule {
        id: sys
        width: menu.width
        height: 2 * DeskStyle.padY + menu.icon + (bootAndroid.visible ? 8 + bootAndroid.height : 0)
        readonly property real iconsW: 4 * menu.icon + 3 * DeskStyle.sysIconGap
        Plate {
            x: DeskStyle.padX
            y: DeskStyle.padY + (menu.icon - DeskStyle.plateH) / 2
            width: menu.innerW
            num: "01"; title: "NAOMIOS"
            reserve: sys.iconsW
        }
        Row {
            x: DeskStyle.padX + menu.innerW - sys.iconsW
            y: DeskStyle.padY
            spacing: DeskStyle.sysIconGap
            IconButton {
                glyph: "force"
                lit: menu.forceOpen
                onActivated: {
                    menu.forceOpen = !menu.forceOpen
                    menu.forceSelected = []
                }
            }
            IconButton { glyph: "gear"; action: "settings"; once: true }
            IconButton { glyph: "reboot"; action: "reboot" }
            IconButton { glyph: DeskStyle.tablet ? "lock" : "logout"; action: DeskStyle.tablet ? "sleep" : "leave"; once: true }
        }
        NButton {
            id: bootAndroid
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 8
            width: menu.innerW
            visible: DeskStyle.tablet && naomiSystemTools.androidAvailable
            centered: true
            text: "Перезагрузить в One UI"
            disabled: naomiSystemTools.busy
            onClicked: menu.bootAndroidRequested()
        }
    }

    // ---------- 02 APP ----------
    readonly property var tiles: [
        { label: "GAME", glyph: "gamepad", game: true }, { label: "WEB", glyph: "globe", web: true },
        { label: "FILES", glyph: "folder", files: true }, { label: "ANDROID", glyph: "android", android: true },
        { label: "TERMINAL", glyph: "term", term: true }, { label: "CLAUDE", glyph: "claude", claude: true },
        { label: "TELEGRAM", glyph: "telegram", telegram: true },
        { label: "DEEPSEEK", glyph: "deepseek", deepseek: true }]
    readonly property int perRow: 4   // APP_TILES_PER_ROW
    readonly property int tileS: Math.floor((innerW - (perRow - 1) * DeskStyle.tileGap) / perRow)
    // черта под ANDROID: окно Android на виду — 1, только свёрнутые или запускается — 0,5
    readonly property real androidLit: {
        const v = Object.values(androidOpen)
        return v.some(o => o >= 1) ? 1 : v.length || Object.keys(androidLaunching).length ? 0.5 : 0
    }
    DeskModule {
        id: apps
        visible: !menu.forceOpen
        y: sys.height + DeskStyle.gamesGap
        width: menu.width
        height: tilesGrid.y + tilesGrid.height + (appError.visible ? appError.height + 8 : 0) + DeskStyle.padY
        Plate {
            x: DeskStyle.padX; y: DeskStyle.padY
            width: menu.innerW
            num: "02"; title: "APP"; note: String(menu.tiles.length)
            reserve: DeskStyle.tablet ? 84 : 0
        }
        NButton {
            visible: DeskStyle.tablet
            x: DeskStyle.padX + menu.innerW - width
            y: DeskStyle.padY - 4
            width: 78; height: DeskStyle.plateH + 8
            text: "LINUX"
            tone: menu.linuxShown ? "on" : "off"
            fontSize: 11
            centered: true
            onClicked: {
                menu.linuxShown = !menu.linuxShown
                if (menu.linuxShown) {
                    menu.androidShown = false
                    menu.gamesShown = false
                    menu.linuxAppsRefreshed()
                }
            }
        }
        Grid {
            id: tilesGrid
            x: DeskStyle.padX
            y: DeskStyle.padY + DeskStyle.plateH + 12
            columns: menu.perRow
            spacing: DeskStyle.tileGap
            Repeater {
                model: menu.tiles
                delegate: Item {   // плитка: рамка, значок и подпись под ним (app_tile)
                    id: tile
                    required property var modelData
                    width: menu.tileS; height: menu.tileS + 6
                    readonly property real dy: tileTap.pressed ? 1 : 0
                    readonly property real open: modelData.files ? menu.filesOpen : modelData.android ? menu.androidLit
                                                 : modelData.term ? menu.termOpen : modelData.web ? menu.webOpen
                                                 : modelData.game ? menu.gameOpen : modelData.claude ? menu.claudeOpen
                                                 : modelData.telegram ? menu.telegramOpen : menu.deepseekOpen
                    // плитка горит, пока открыт её модуль «03»
                    readonly property bool lit: !!modelData.android && menu.androidShown || !!modelData.game && menu.gamesShown
                    Rectangle {
                        y: tile.dy
                        width: parent.width; height: parent.height
                        color: DeskStyle.amberA(tileTap.pressed ? DeskStyle.itemPress
                                                : tile.lit ? (tileHover.hovered ? 0.16 : 0.12) : DeskStyle.itemHover)
                        visible: tileHover.hovered || tileTap.pressed || tile.lit
                    }
                    Rectangle {
                        y: tile.dy
                        width: parent.width; height: parent.height
                        color: "transparent"
                        border.width: 1
                        border.color: DeskStyle.amberA(tile.lit ? 0.45 : DeskStyle.line)
                    }
                    Rectangle {   // запущена — янтарная черта снизу (свёрнута — вполсилы)
                        visible: tile.open > 0
                        x: tile.width * 0.3
                        y: tile.height - 4 + tile.dy
                        width: tile.width * 0.4
                        height: 2
                        color: DeskStyle.amberA(tile.open)
                    }
                    readonly property color ink: tile.lit ? DeskStyle.amber
                                                 : tileHover.hovered || tile.open >= 1 ? DeskStyle.text : DeskStyle.muted
                    Glyph {
                        kind: tile.modelData.glyph
                        unit: 1.15
                        line: 1.4
                        x: tile.width / 2; y: tile.width * 0.42 + tile.dy
                        color: tile.ink
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: tile.width * 0.84 - font.pixelSize * 0.8 + tile.dy
                        text: tile.modelData.label
                        font.family: DeskStyle.mono
                        font.pixelSize: DeskStyle.caps - 4
                        font.letterSpacing: 1.0
                        color: tile.ink
                    }
                    HoverHandler { id: tileHover }
                    TapHandler {
                        id: tileTap
                        onTapped: {
                            if (tile.modelData.files)
                                menu.filesClicked()
                            else if (tile.modelData.android) {
                                menu.androidShown = !menu.androidShown
                                menu.gamesShown = false
                                menu.linuxShown = false
                            } else if (tile.modelData.game) {
                                menu.gamesShown = !menu.gamesShown
                                menu.androidShown = false
                                menu.linuxShown = false
                            } else if (tile.modelData.term)
                                menu.termClicked()
                            else if (tile.modelData.web)
                                menu.webClicked()
                            else if (tile.modelData.claude)
                                menu.claudeClicked()
                            else if (tile.modelData.telegram)
                                menu.telegramClicked()
                            else if (tile.modelData.deepseek)
                                menu.deepseekClicked()
                        }
                    }
                }
            }
        }
        Text {
            id: appError
            x: DeskStyle.padX
            y: tilesGrid.y + tilesGrid.height + 8
            width: menu.innerW
            visible: text.length > 0
            text: menu.deepseekError || menu.telegramError
            wrapMode: Text.WordWrap
            font.family: DeskStyle.sans
            font.pixelSize: DeskStyle.barText
            color: DeskStyle.bad
        }
    }

    // Список модуля «03» (game_row): строка — грань, значок, имя, справа — подпись (label, если есть). rows — строки,
    // keyName — поле-ключ строки; open — {ключ: …} открытых (грань янтарём), launching — запускаемых (вполсилы).
    // Значок — картинка строки (icon); нет её — геймпад в рамке. gear — под мышью на месте подписи шестерёнка
    // (у игр: «Кнопки геймпада», вариант А из макетов 21 ноября 2026): щелчок по ней — gearClicked, не запуск
    component RowList: ListView {
        id: list
        property var rows: []
        property string keyName
        property var open: ({})
        property var launching: ({})
        property bool gear: false
        signal activated(var row)
        signal gearClicked(var row)
        clip: true
        interactive: false   // колесо — MouseArea ниже (плавно, как у стола), тянуть — незачем
        boundsBehavior: Flickable.StopAtBounds
        model: rows
        delegate: Item {
            id: row
            required property var modelData
            readonly property string key: modelData[list.keyName]
            width: list.width
            height: menu.rowH
            readonly property real lit: list.open[key] !== undefined ? 1 : list.launching[key] ? 0.55 : 0.35
            Rectangle {   // под мышью — заливка (нажата — плотнее)
                width: parent.width; height: menu.rowH - 4
                color: DeskStyle.amberA(rowTap.pressed ? DeskStyle.itemPress : DeskStyle.itemHover)
                visible: rowHover.hovered
            }
            Rectangle {   // грань слева: янтарная — окно есть, вполсилы — запускается, серая — нет
                width: 3; height: menu.rowH - 4
                color: row.lit > 0.35 ? DeskStyle.amberA(row.lit) : Qt.rgba(DeskStyle.dim.r, DeskStyle.dim.g, DeskStyle.dim.b, 0.35)
            }
            Image {
                visible: row.modelData.icon !== undefined
                x: 12; y: (menu.rowH - 4 - 32) / 2
                width: 32; height: 32
                sourceSize: Qt.size(32 * naomiDesk.scale, 32 * naomiDesk.scale)
                source: row.modelData.icon !== undefined ? row.modelData.icon : ""
                smooth: true
                asynchronous: true
            }
            Rectangle {   // значок игры: геймпад в рамке
                visible: row.modelData.icon === undefined
                x: 12; y: (menu.rowH - 4 - 32) / 2
                width: 32; height: 32
                radius: 7
                color: DeskStyle.amberA(row.lit >= 1 ? 0.12 : 0.05)
                border.width: 1
                border.color: DeskStyle.amberA(row.lit >= 1 ? 0.45 : DeskStyle.line)
                Glyph {
                    kind: "gamepad"
                    unit: 0.95
                    x: 16; y: 16
                    color: row.lit >= 1 ? DeskStyle.amber : rowHover.hovered ? DeskStyle.text : DeskStyle.muted
                }
            }
            readonly property bool gearShown: list.gear && rowHover.hovered
            Item {   // шестерёнка: под ней — заливка, как у кнопок меню
                id: gearBox
                visible: row.gearShown
                x: row.width - 4 - width
                y: (menu.rowH - 4 - height) / 2
                width: 28
                height: 28
                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    color: DeskStyle.amberA(gearHover.hovered ? DeskStyle.itemPress : 0)
                    border.width: 1
                    border.color: DeskStyle.amberA(gearHover.hovered ? 0.36 : 0)
                }
                Glyph {
                    kind: "gear"
                    x: 14; y: 14
                    color: gearHover.hovered ? DeskStyle.amber : DeskStyle.text
                }
                HoverHandler { id: gearHover }
            }
            Text {
                id: rowTag
                visible: text !== "" && !row.gearShown
                x: row.width - 8 - implicitWidth
                height: menu.rowH - 4
                verticalAlignment: Text.AlignVCenter
                text: row.modelData.label !== undefined ? row.modelData.label : ""
                font.family: DeskStyle.mono
                font.pixelSize: DeskStyle.caps - 2
                font.letterSpacing: 1.0
                color: DeskStyle.dim
            }
            Text {
                x: 56
                width: (rowTag.visible ? rowTag.x - 10 : row.gearShown ? gearBox.x - 10 : row.width - 8) - x
                height: menu.rowH - 4
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: row.modelData.name
                font.family: DeskStyle.sans
                font.pixelSize: 15
                color: DeskStyle.text
            }
            HoverHandler { id: rowHover }
            TapHandler {
                id: rowTap
                onTapped: (p) => p.position.x >= gearBox.x - 2 && row.gearShown ? list.gearClicked(row.modelData)
                                                                                 : list.activated(row.modelData)
            }
        }
        MouseArea {   // колесо — MouseArea: WheelHandler столу событий колеса не получает (FilesWindow.qml)
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            Component.onCompleted: cursorShape = undefined   // без формы: не закрыть строки стрелкой
            onWheel: (e) => list.contentY = Math.max(0, Math.min(list.contentHeight - list.height,
                list.contentY - (e.pixelDelta.y !== 0 ? e.pixelDelta.y : e.angleDelta.y / 120 * 100)))
        }
    }

    // ---------- 03 ANDROID ----------
    // Как «03 GAMES» НаомиОС: открыли — сразу виден и дорастает от верхнего края (180 мс), закрыли — растворяется (140 мс).
    // Шильдик — на высоте кнопок (SYS_ICON), под ним через 10 — строки по 44 (GAME_ROW_H; сама строка — 40). Строк
    // больше, чем влезает под меню, — список прокручивается колесом.
    readonly property int rowH: 44
    property real maxHeight: 10000   // сколько меню может занять вниз (стол — до своего низа)
    readonly property real maxListH: Math.max(3 * rowH - 4, maxHeight - androidModule.y - DeskStyle.padY - icon - 10
                                                            - DeskStyle.padY)
    DeskModule {
        id: androidModule
        y: apps.y + apps.height + DeskStyle.gamesGap
        width: menu.width
        readonly property real listH: menu.androidApps.length ? Math.min(menu.androidApps.length * menu.rowH - 4, menu.maxListH)
                                                              : 22
        height: DeskStyle.padY + menu.icon + 10 + listH + DeskStyle.padY
        visible: opacity > 0 && !menu.forceOpen
        opacity: menu.androidShown ? 1 : 0
        scale: menu.androidShown ? 1 : 0.96
        transformOrigin: Item.Top
        Behavior on opacity { NumberAnimation { duration: menu.androidShown ? 180 : 140; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: menu.androidShown ? 180 : 140; easing.type: Easing.OutCubic } }
        enabled: menu.androidShown && !menu.forceOpen   // растворяется — щелчки ему уже не идут

        readonly property real buttonsW: 2 * menu.icon + DeskStyle.sysIconGap
        Plate {
            x: DeskStyle.padX
            y: DeskStyle.padY + (menu.icon - DeskStyle.plateH) / 2
            width: menu.innerW
            num: "03"; title: "ANDROID"
            // Android включается (первое приложение после запуска оболочки, ~10–20 с) — видно, что ждём
            value: menu.androidState === "starting" ? "ВКЛЮЧАЕТСЯ" : ""
            valueColor: DeskStyle.muted
            note: menu.androidApps.length ? String(menu.androidApps.length) : ""
            reserve: androidModule.buttonsW
        }
        Row {
            x: DeskStyle.padX + menu.innerW - androidModule.buttonsW
            y: DeskStyle.padY
            spacing: DeskStyle.sysIconGap
            IconButton { glyph: "gear"; onActivated: menu.androidActivated("com.android.settings") }
            IconButton { glyph: "play"; onActivated: menu.androidActivated("com.android.vending") }
        }
        Text {   // пусто — подсказка, как «игр без steam пока нет» (bt_hint: моно, приглушённым)
            visible: menu.androidApps.length === 0
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 10 + 2
            width: menu.innerW
            wrapMode: Text.WordWrap
            text: naomiAndroid.installed ? "приложений пока нет — поставь их в google play" : "android не настроен"
            font.family: DeskStyle.mono
            font.pixelSize: 12   // N_META − 0,5 (размер у Qt целый)
            color: DeskStyle.muted
        }
        RowList {
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 10
            width: menu.innerW
            height: androidModule.listH
            rows: menu.androidApps
            keyName: "pkg"
            open: menu.androidOpen
            launching: menu.androidLaunching
            onActivated: (row) => menu.androidActivated(row.pkg)
        }
    }
    // ---------- 03 GAMES (docs/GAMES.md) ----------
    // На том же месте и так же, как «03 ANDROID»; кнопок в шильдике нет — у игр нет магазина; настройки — у каждой
    // игры своя шестерёнка под мышью (кнопки геймпада)
    DeskModule {
        id: gamesModule
        y: androidModule.y
        width: menu.width
        readonly property real listH: menu.games.length ? Math.min(menu.games.length * menu.rowH - 4, menu.maxListH) : 38
        height: DeskStyle.padY + menu.icon + 10 + listH + DeskStyle.padY
        visible: opacity > 0 && !menu.forceOpen
        opacity: menu.gamesShown ? 1 : 0
        scale: menu.gamesShown ? 1 : 0.96
        transformOrigin: Item.Top
        Behavior on opacity { NumberAnimation { duration: menu.gamesShown ? 180 : 140; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: menu.gamesShown ? 180 : 140; easing.type: Easing.OutCubic } }
        enabled: menu.gamesShown && !menu.forceOpen

        Plate {
            x: DeskStyle.padX
            y: DeskStyle.padY + (menu.icon - DeskStyle.plateH) / 2
            width: menu.innerW
            num: "03"; title: "GAMES"
            note: menu.games.length ? String(menu.games.length) : ""
        }
        Text {   // пусто — подсказка, куда класть игры
            visible: menu.games.length === 0
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 10 + 2
            width: menu.innerW
            wrapMode: Text.WordWrap
            text: "игр пока нет — положи их в папку games/nes, появятся сами"
            font.family: DeskStyle.mono
            font.pixelSize: 12
            color: DeskStyle.muted
        }
        RowList {
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 10
            width: menu.innerW
            height: gamesModule.listH
            rows: menu.games
            keyName: "path"
            open: menu.gameRunning !== "" && menu.gameOpen >= 1 ? ({ [menu.gameRunning]: 1 }) : ({})
            launching: menu.gameRunning !== "" && menu.gameOpen < 1 ? ({ [menu.gameRunning]: true }) : ({})
            gear: true
            onActivated: (row) => menu.gameActivated(row)
            onGearClicked: (row) => menu.gamePad(row)
        }
    }
    // ---------- 03 LINUX (все установленные Linux .desktop программы) ----------
    readonly property var filteredLinuxApps: {
        const q = linuxQuery.trim().toLowerCase()
        if (!q)
            return linuxApps
        return linuxApps.filter(a => String(a.name || "").toLowerCase().includes(q)
                                  || String(a.id || "").toLowerCase().includes(q))
    }
    DeskModule {
        id: linuxModule
        y: androidModule.y
        width: menu.width
        readonly property real searchH: 32
        readonly property real listH: menu.filteredLinuxApps.length
                                      ? Math.min(menu.filteredLinuxApps.length * menu.rowH - 4, Math.max(3 * menu.rowH - 4, menu.maxListH - searchH - 8))
                                      : 30
        height: DeskStyle.padY + menu.icon + 10 + searchH + 8 + listH + DeskStyle.padY
        visible: opacity > 0 && !menu.forceOpen
        opacity: menu.linuxShown ? 1 : 0
        scale: menu.linuxShown ? 1 : 0.96
        transformOrigin: Item.Top
        Behavior on opacity { NumberAnimation { duration: menu.linuxShown ? 180 : 140; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: menu.linuxShown ? 180 : 140; easing.type: Easing.OutCubic } }
        enabled: menu.linuxShown && !menu.forceOpen

        Plate {
            x: DeskStyle.padX
            y: DeskStyle.padY + (menu.icon - DeskStyle.plateH) / 2
            width: menu.innerW
            num: "03"; title: "LINUX"
            note: menu.filteredLinuxApps.length ? String(menu.filteredLinuxApps.length) : ""
            reserve: menu.icon
        }
        IconButton {
            x: DeskStyle.padX + menu.innerW - width
            y: DeskStyle.padY
            glyph: "reboot"
            onActivated: menu.linuxAppsRefreshed()
        }
        Rectangle {
            id: linuxSearchBox
            x: DeskStyle.padX
            y: DeskStyle.padY + menu.icon + 10
            width: menu.innerW
            height: linuxModule.searchH
            radius: 3
            color: DeskStyle.surface
            border.width: 1
            border.color: linuxSearch.activeFocus ? DeskStyle.amberA(0.55) : DeskStyle.amberA(DeskStyle.line)
            TextInput {
                id: linuxSearch
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                text: menu.linuxQuery
                onTextChanged: menu.linuxQuery = text
                font.family: DeskStyle.sans
                font.pixelSize: 13
                color: DeskStyle.text
                selectionColor: DeskStyle.amberA(0.35)
                selectedTextColor: DeskStyle.text
                clip: true
                cursorDelegate: Rectangle { width: 1; color: DeskStyle.amber }
                Text {
                    visible: !linuxSearch.text
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Поиск программ Linux..."
                    font: linuxSearch.font
                    color: DeskStyle.dim
                }
            }
        }
        Text {
            visible: menu.filteredLinuxApps.length === 0
            x: DeskStyle.padX
            y: linuxSearchBox.y + linuxSearchBox.height + 10
            width: menu.innerW
            wrapMode: Text.WordWrap
            text: menu.linuxApps.length === 0 ? "программ пока не найдено" : "ничего не найдено"
            font.family: DeskStyle.mono
            font.pixelSize: 12
            color: DeskStyle.muted
        }
        RowList {
            x: DeskStyle.padX
            y: linuxSearchBox.y + linuxSearchBox.height + 8
            width: menu.innerW
            height: linuxModule.listH
            rows: menu.filteredLinuxApps
            keyName: "id"
            open: ({})
            launching: ({})
            onActivated: (row) => menu.linuxAppActivated(row)
        }
    }
    // ---------- 02 FORCE QUIT (docs/DESKTOP.md, «Закрыть принудительно») ----------
    // Как force_module НаомиОС («кнопки в шильдике»): программы с окнами на столе, строка — щелчок выбирает (грань и
    // заливка янтарём, ещё щелчок — снять), справа — сколько памяти у программы со всеми её процессами, левее — «не
    // отвечает» красным (окно не ответило на пинг за 2,5 с). В шильдике — «ВСЕ» (выбрать все; все выбраны — снять) и
    // «ЗАКРЫТЬ» (выбранные — насовсем, даже зависшие: просьба закрыться, через 2 с — SIGKILL). Строки по 34 (сама — 30);
    // больше, чем влезает под меню, — прокрутка колесом.
    readonly property int forceRowH: 34
    readonly property var forceKeys: forceItems.map(o => o.key)
    onForceKeysChanged: {   // выбранной программы уже нет — отметку снять
        const kept = forceSelected.filter(k => forceKeys.includes(k))
        if (kept.length !== forceSelected.length)
            forceSelected = kept
    }
    function forceToggle(key) {
        forceSelected = forceSelected.includes(key) ? forceSelected.filter(k => k !== key) : forceSelected.concat([key])
    }
    // память: «340 МБ», «1,2 ГБ»; не мерили — пусто (force_mem_label)
    function memoryLabel(kb) {
        if (!(kb > 0))
            return ""
        const mb = kb / 1024
        return mb < 1000 ? Math.max(1, Math.round(mb)) + " МБ" : (mb / 1024).toFixed(1).replace(".", ",") + " ГБ"
    }
    DeskModule {
        id: forceModule
        visible: menu.forceOpen
        y: sys.height + DeskStyle.gamesGap
        width: menu.width
        readonly property real listY: DeskStyle.padY + DeskStyle.plateH + 8
        readonly property real maxListH: Math.max(3 * menu.forceRowH - 4, menu.maxHeight - y - listY - DeskStyle.padY)
        readonly property real listH: menu.forceItems.length
                                      ? Math.min(menu.forceItems.length * menu.forceRowH - 4, maxListH) : 30
        height: listY + listH + DeskStyle.padY
        readonly property bool all: menu.forceItems.length > 0 && menu.forceSelected.length === menu.forceItems.length

        component PlateCaps: Text {   // мерка подписи кнопки шильдика: моно 11,5 (у Qt — 12), разрядка .08
            visible: false
            font.family: DeskStyle.mono
            font.pixelSize: 12
            font.letterSpacing: 11.5 * 0.08
        }
        PlateCaps { id: allCaps; text: "ВСЕ" }
        PlateCaps { id: killCaps; text: "ЗАКРЫТЬ" }
        readonly property real allW: Math.ceil(allCaps.implicitWidth) + 2 * 12 + 3
        readonly property real killW: Math.ceil(killCaps.implicitWidth) + 2 * 12 + 3
        readonly property real btnH: DeskStyle.plateH + 6
        Plate {
            x: DeskStyle.padX; y: DeskStyle.padY
            width: menu.innerW
            num: "02"; title: "FORCE QUIT"
            reserve: forceModule.allW + 6 + forceModule.killW
        }
        NButton {
            x: DeskStyle.padX + menu.innerW - forceModule.killW - 6 - width
            y: DeskStyle.padY + (DeskStyle.plateH - height) / 2
            width: forceModule.allW; height: forceModule.btnH
            text: allCaps.text
            tone: forceModule.all ? "on" : "off"
            fontSize: allCaps.font.pixelSize
            letterSpacing: allCaps.font.letterSpacing
            centered: true
            disabled: menu.forceItems.length === 0
            onClicked: menu.forceSelected = forceModule.all ? [] : menu.forceKeys.slice()
        }
        NButton {
            x: DeskStyle.padX + menu.innerW - width
            y: DeskStyle.padY + (DeskStyle.plateH - height) / 2
            width: forceModule.killW; height: forceModule.btnH
            text: killCaps.text
            tone: "bad"
            fontSize: killCaps.font.pixelSize
            letterSpacing: killCaps.font.letterSpacing
            centered: true
            disabled: menu.forceSelected.length === 0
            onClicked: {
                menu.forceKill(menu.forceSelected.slice())
                menu.forceSelected = []
            }
        }
        Text {   // пусто — подсказка (bt_hint)
            visible: menu.forceItems.length === 0
            x: DeskStyle.padX
            y: forceModule.listY + 6
            width: menu.innerW
            text: "открытых программ нет"
            font.family: DeskStyle.mono
            font.pixelSize: 12
            color: DeskStyle.muted
        }
        ListView {
            id: forceList
            x: DeskStyle.padX
            y: forceModule.listY
            width: menu.innerW
            height: forceModule.listH
            clip: true
            interactive: false   // колесо — MouseArea ниже, как у RowList
            boundsBehavior: Flickable.StopAtBounds
            model: menu.forceItems
            delegate: Item {
                id: frow
                required property var modelData
                readonly property var info: menu.forceInfo[modelData.key] || ({})
                readonly property bool sel: menu.forceSelected.includes(modelData.key)
                readonly property bool hover: frowHover.hovered
                readonly property bool hung: !!info.hung
                readonly property real hh: menu.forceRowH - 4
                readonly property color ink: sel || hover ? DeskStyle.text : DeskStyle.muted
                width: forceList.width
                height: menu.forceRowH
                Rectangle {   // выбрана — янтарь .10, под мышью — .07
                    width: parent.width; height: frow.hh
                    color: DeskStyle.amberA(frow.sel ? 0.10 : DeskStyle.itemHover)
                    visible: frow.sel || frow.hover
                }
                Rectangle {   // грань: выбрана — янтарём, не отвечает — красным, иначе — тускло
                    y: 6
                    width: 3; height: frow.hh - 12
                    color: frow.sel ? DeskStyle.amber : frow.hung ? DeskStyle.bad
                                    : Qt.rgba(DeskStyle.dim.r, DeskStyle.dim.g, DeskStyle.dim.b, 0.35)
                }
                Text {
                    id: memText
                    x: frow.width - 8 - implicitWidth
                    height: frow.hh
                    verticalAlignment: Text.AlignVCenter
                    text: menu.memoryLabel(frow.info.kb)
                    font.family: DeskStyle.mono
                    font.pixelSize: 12
                    font.letterSpacing: 12 * 0.04
                    color: frow.sel || frow.hover ? DeskStyle.text : DeskStyle.dim
                }
                readonly property real memLeft: memText.text !== "" ? memText.x - 14 : frow.width - 8
                Text {
                    id: hungText
                    visible: frow.hung
                    x: frow.memLeft - implicitWidth
                    height: frow.hh
                    verticalAlignment: Text.AlignVCenter
                    text: "не отвечает"
                    font.family: DeskStyle.mono
                    font.pixelSize: 12
                    font.letterSpacing: 12 * 0.04
                    color: DeskStyle.bad
                }
                Text {
                    x: 14
                    width: (frow.hung ? hungText.x - 12 : frow.memLeft) - x
                    height: frow.hh
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: frow.modelData.name
                    font.family: DeskStyle.sans
                    font.pixelSize: 14
                    color: frow.ink
                }
                Item {   // щелчок и наведение — по самой строке, без зазора под ней
                    width: parent.width; height: frow.hh
                    HoverHandler { id: frowHover }
                    TapHandler { onTapped: menu.forceToggle(frow.modelData.key) }
                }
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                Component.onCompleted: cursorShape = undefined   // без формы: не закрыть строки стрелкой
                onWheel: (e) => forceList.contentY = Math.max(0, Math.min(forceList.contentHeight - forceList.height,
                    forceList.contentY - (e.pixelDelta.y !== 0 ? e.pixelDelta.y : e.angleDelta.y / 120 * 100)))
            }
        }
    }
}
