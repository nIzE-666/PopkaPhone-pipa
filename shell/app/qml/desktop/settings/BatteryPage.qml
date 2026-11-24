// «Настройки» → «Батарея» (выбор пользователя 21 ноября 2026 — вариант 08 «Заряд и тепло» из десяти макетов): слева два
// столбика — заряд с коридором режима (жёлтым: от «снова ниже» до «заряжать до») и температура батареи с порогами
// 35 · 40 · 45 °C; справа — что сейчас, режимы плиткой 2 × 2 («Каждый день» 75–80 %, «Только док» 45–55 %, «Своё»,
// «Без предела»), уставки «Своего» и «до 100 % один раз»; ниже — защита: жара, запасная защита ядра, калибровка,
// выключение на 3 %. Ведёт naomid (объект Charge, docs/NAOMID.md «Заряд»); ток, напряжение и температура — Sample()
// батареи раз в 2 с, пока раздел виден.
import QtQuick
import QtQuick.Shapes
import ".."

Item {
    id: page
    property var sw
    readonly property var ch: naomiPhone.charge
    readonly property int level: naomiPhone.battery.percent
    property var sample: ({})
    readonly property bool known: sample.batteryC !== undefined
    readonly property real tempC: known ? sample.batteryC : 0
    readonly property bool plugged: ch.state !== "battery"
    readonly property var modes: [
        { id: "daily", name: "Каждый день", up: 80, lo: 75, s: "с собой" },
        { id: "dock", name: "Только док", up: 55, lo: 45, s: "всегда на доке" },
        { id: "custom", name: "Своё", up: ch.upper, lo: ch.lower, s: "сами" },
        { id: "off", name: "Без предела", up: 100, lo: 97, s: "как было" }]
        // Pad 6 (naomid: Backend night) держит заряд только от 80 % — «Только док» (55 %) ему не по силам
        .filter(m => m.id !== "dock" || !(ch.minUpper > m.up))
    readonly property var mode: modes.find(m => m.id === ch.mode) || modes[0]
    implicitHeight: col.height

    Timer {
        interval: 2000
        running: page.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: naomiPhone.battery.call("Sample", [], s => page.sample = s)
    }

    function comma(v, d) { return v.toFixed(d).replace(".", ",") }
    // напряжение покоя при заряде (батарея на 4,45 В): 100 % — замер, остальное — оценка (как у naomid, charge.cpp)
    function volts(p) {
        const t = [[0, 3.35], [10, 3.70], [20, 3.73], [30, 3.78], [40, 3.82], [50, 3.87], [60, 3.95], [70, 4.03],
                   [80, 4.12], [90, 4.27], [100, 4.41]]
        for (let i = 1; i < t.length; ++i)
            if (p <= t[i][0])
                return comma(t[i - 1][1] + (t[i][1] - t[i - 1][1]) * (p - t[i - 1][0]) / 10, 2)
        return "4,41"
    }
    readonly property var months: ["января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                                   "сентября", "октября", "ноября", "декабря"]
    function day(t) { const d = new Date(t * 1000); return d.getDate() + " " + months[d.getMonth()] }
    function band(m) { return m.up >= 100 ? "до 100 %" : m.lo + "–" + m.up + " %" }

    // что сейчас: слово (светлым) и пояснение
    readonly property var now: {
        const m = mode.name.toLowerCase()
        switch (ch.state) {
        case "charging": return ["заряжается до " + ch.stop + " %", "дойдёт — заряд встанет, а телефон так и будет питаться от кабеля"]
        case "holding": return ["держит " + level + " %", level >= 100 ? "полная: дозаряд с 97 % — на кабеле батарея стоит на 4,41 В"
                                                                       : "батарея отдыхает: телефон питается от кабеля"]
        case "short": return ["кабель не тянет", "на кабеле, а заряд уходит: телефон тратит больше, чем даёт кабель"]
        case "draining": return ["спускается к " + ch.stop + " %", ch.warm
            ? "полчаса теплее 35 °C — верх снижен до 60 %; питание от кабеля отключено, пока батарея не спустится"
            : "питание от кабеля отключено, пока батарея не дойдёт до верха: так быстрее уходит с высокого напряжения"]
        case "resting": return ["пауза до " + ch.start + " %", "заряд стоит, планшет живёт от батареи: удержать заряд на месте ядро Xiaomi не умеет — ниже "
                                                              + ch.start + " % снова зарядится до " + ch.stop + " %"]
        case "hot": return ["пауза: жарко", "батарея " + comma(tempC, 1) + " °C — заряд продолжится, когда остынет до 37 °C"]
        case "full": return ["до 100 % один раз", "отключите кабель — и снова «" + m + "»; на 100 % — не дольше 3 ч"]
        case "calibrating": return ["калибровка", ch.stop <= 15 ? "спускается до 15 %, потом одним заходом до 100 % — чтобы проценты не врали"
                                                                : "одним заходом до 100 %, полчаса — и назад к режиму"]
        case "learning": return ["новая батарея", "первый заряд до 100 % — датчик учит её ёмкость"]
        default: return ["от батареи", "на кабеле — " + (level < mode.up ? "заряд до " + mode.up + " %" : "не раньше " + mode.lo + " %")]
        }
    }
    readonly property string modeNote: {
        switch (ch.mode) {
        case "dock": return "держит 45–55 % (≈ " + volts(45) + "–" + volts(55) + " В) — ниже ступеньки старения около 60 %: телефон питается от кабеля, батарея отдыхает"
        case "custom": return "заряжать до " + ch.upper + " % (≈ " + volts(ch.upper) + " В), снова — ниже " + ch.lower + " %"
        case "off": return "до 100 % (≈ 4,41 В) и дозаряд с 97 % — как было; на доке так батарея стареет быстрее всего"
        default: return "до 80 % (≈ " + volts(80) + " В) — на день хватает; на кабеле снова заряжается ниже 75 %. Перед дорогой — «до 100 % один раз»"
        }
    }

    component Seg: NButton { height: 30 }
    component Label: Text {
        font.family: DeskStyle.mono
        font.pixelSize: 12
        font.letterSpacing: 12 * 0.12
        color: DeskStyle.muted
    }
    component Meta: Text {
        wrapMode: Text.Wrap
        lineHeight: 1.3
        font.family: DeskStyle.mono
        font.pixelSize: 12
        color: DeskStyle.muted
    }
    component Line: Rectangle { height: 1; color: DeskStyle.amberA(DeskStyle.line) }
    component Big: Row {   // число под столбиком: тонко, единица мельче приглушённым
        id: big
        property string value
        property string unit
        property color tone: DeskStyle.text
        spacing: 4
        Text {
            id: num
            text: big.value
            font.family: DeskStyle.sans
            font.pixelSize: 30
            font.weight: Font.ExtraLight
            color: big.tone
        }
        Text {
            y: num.baselineOffset - baselineOffset
            text: big.unit
            font.family: DeskStyle.sans
            font.pixelSize: 14
            font.weight: Font.Light
            color: DeskStyle.muted
        }
    }
    component Dash: Row {   // пунктир порога поперёк столбика
        property color tone
        spacing: 3
        Repeater { model: 12; Rectangle { width: 4; height: 1; color: parent.tone } }
    }

    // столбик: корпус 64 × 220, внутри — заливка на долю fill (0…1) с зазором 4
    component Column64: Rectangle {
        property real fill: 0
        property color tone: DeskStyle.muted
        property real toneOpacity: 0.55
        width: 64; height: 220
        radius: 8
        color: Qt.rgba(1, 1, 1, 0.06)
        border.width: 1.5
        border.color: DeskStyle.muted
        Rectangle {
            x: 4; width: 56
            height: Math.max(0, 212 * Math.min(1, parent.fill))
            y: 4 + 212 - height
            radius: 4
            color: parent.tone
            opacity: parent.toneOpacity
            Behavior on height { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        }
    }

    Column {
        id: col
        width: page.width
        spacing: 22

        // ---------- 01 батарея ----------
        Column {
            width: col.width
            spacing: 16
            Head { width: col.width; num: "01"; title: "БАТАРЕЯ"; info: page.now[0] + " · " + page.mode.name.toLowerCase() }
            Row {
                spacing: 26
                Item {   // два столбика: заряд и тепло
                    width: 250
                    height: 286
                    Item {
                        width: 125; height: parent.height
                        Column64 {
                            id: charge
                            x: (parent.width - width) / 2
                            fill: page.level / 100
                            tone: page.ch.state === "hot" ? DeskStyle.bad : page.plugged ? DeskStyle.teal : DeskStyle.muted
                            toneOpacity: page.plugged ? 0.5 : 0.55
                            Item {   // коридор режима: от «снова ниже» до «заряжать до»
                                visible: page.mode.up < 100
                                x: -7; width: 78
                                y: 4 + 212 * (1 - page.mode.up / 100)
                                height: 212 * (page.mode.up - page.mode.lo) / 100
                                Rectangle { anchors.fill: parent; color: DeskStyle.amberA(0.12) }
                                Rectangle { width: parent.width; height: 1.5; color: DeskStyle.amber }
                                Rectangle { y: parent.height - 1.5; width: parent.width; height: 1.5; color: DeskStyle.amber }
                            }
                        }
                        Big { anchors.horizontalCenter: parent.horizontalCenter; y: 230; value: page.level; unit: "%" }
                        Label { anchors.horizontalCenter: parent.horizontalCenter; y: 270; text: "ЗАРЯД" }
                    }
                    Item {
                        x: 125
                        width: 125; height: parent.height
                        readonly property real t0: 20
                        readonly property real t1: 50
                        function yOf(t) { return 4 + 212 * (1 - (t - t0) / (t1 - t0)) }
                        Column64 {
                            id: temp
                            x: (parent.width - width) / 2
                            fill: page.known ? (page.tempC - parent.t0) / (parent.t1 - parent.t0) : 0
                            tone: page.tempC >= 40 ? DeskStyle.bad : DeskStyle.muted
                        }
                        Repeater {
                            model: [[35, DeskStyle.dim], [40, DeskStyle.amber], [45, DeskStyle.bad]]
                            delegate: Item {
                                required property var modelData
                                y: parent.yOf(modelData[0])
                                Dash { x: temp.x - 10; tone: modelData[1] }
                                Text {
                                    x: temp.x + temp.width + 8; y: -7
                                    text: modelData[0] + "°"
                                    font.family: DeskStyle.mono
                                    font.pixelSize: 11
                                    color: modelData[0] === 35 ? DeskStyle.dim : modelData[1]
                                }
                            }
                        }
                        Big {
                            anchors.horizontalCenter: temp.horizontalCenter
                            y: 230
                            value: page.known ? page.comma(page.tempC, 1) : "…"
                            unit: "°C"
                            tone: page.tempC >= 40 ? DeskStyle.bad : DeskStyle.text
                        }
                        Label { anchors.horizontalCenter: temp.horizontalCenter; y: 270; text: "БАТАРЕЯ" }
                    }
                }
                Column {
                    id: rcol
                    width: col.width - 276
                    spacing: 12
                    Meta {
                        width: rcol.width
                        textFormat: Text.StyledText
                        text: "<font color='" + DeskStyle.text + "'>" + page.now[0] + "</font> · " + page.now[1]
                    }
                    Meta {
                        width: rcol.width
                        color: DeskStyle.dim
                        text: "тепло: 35 °C полчаса — верх 60 % · 40 °C — пауза заряда · 42–55 °C — ядро само убавляет ток, выше 55 °C — стоп"
                    }
                    Grid {
                        columns: 2
                        spacing: 7
                        Repeater {
                            model: page.modes
                            delegate: Item {
                                id: tile
                                required property var modelData
                                readonly property bool on: page.ch.mode === modelData.id
                                readonly property real dy: tileArea.pressed ? 1 : 0
                                width: (rcol.width - 7) / 2
                                height: 48
                                Rectangle {
                                    y: tile.dy
                                    width: tile.width; height: tile.height
                                    color: tile.on ? DeskStyle.amberA(0.09) : tileArea.containsMouse ? DeskStyle.amberA(DeskStyle.itemHover) : "transparent"
                                    border.width: 1
                                    border.color: DeskStyle.amberA(DeskStyle.line)
                                }
                                Rectangle {
                                    y: tile.dy
                                    width: 3; height: tile.height
                                    color: tile.on ? DeskStyle.amber : Qt.rgba(125 / 255, 120 / 255, 110 / 255, 0.55)
                                }
                                Text {
                                    x: 14; y: 9 + tile.dy
                                    text: tile.modelData.name.toUpperCase()
                                    font.family: DeskStyle.mono
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    font.letterSpacing: 12 * 0.14
                                    color: tile.on ? DeskStyle.amber : DeskStyle.muted
                                }
                                Text {
                                    x: 14; y: 27 + tile.dy
                                    width: tile.width - 20
                                    elide: Text.ElideRight
                                    text: page.band(tile.modelData) + " · " + tile.modelData.s
                                    font.family: DeskStyle.mono
                                    font.pixelSize: 12
                                    color: DeskStyle.dim
                                }
                                MouseArea {
                                    id: tileArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: page.ch.call("SetMode", [tile.modelData.id])
                                }
                            }
                        }
                    }
                    Column {   // уставки «Своего»
                        visible: page.ch.mode === "custom"
                        width: rcol.width
                        Repeater {
                            model: [["Заряжать до", "upper"], ["Снова ниже", "lower"]]
                            delegate: Item {
                                required property var modelData
                                required property int index
                                width: rcol.width
                                height: 40
                                Line { visible: index > 0; width: parent.width }
                                Label { height: parent.height; verticalAlignment: Text.AlignVCenter; text: modelData[0].toUpperCase() }
                                Stepper {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    readonly property bool up: modelData[1] === "upper"
                                    value: up ? page.ch.upper : page.ch.lower
                                    from: up ? Math.max(50, page.ch.minUpper || 0) : 20
                                    to: up ? 100 : page.ch.upper - 5
                                    step: 5
                                    unit: "%"
                                    onMoved: (v) => page.ch.call("SetLimits", up ? [v, Math.min(page.ch.lower, v - 5)] : [page.ch.upper, v])
                                }
                            }
                        }
                    }
                    Seg {
                        width: rcol.width
                        tone: page.ch.fullOnce ? "goOn" : "go"
                        text: page.ch.fullOnce ? "До 100 % · отменить" : "До 100 % один раз"
                        onClicked: page.ch.call("ChargeFull", [!page.ch.fullOnce])
                    }
                    Meta { width: rcol.width; text: page.modeNote }
                }
            }
        }

        // ---------- 02 защита ----------
        Column {
            width: col.width
            spacing: 6
            Head { width: col.width; num: "02"; title: "ЗАЩИТА" }
            Repeater {
                model: [
                    ["heat", "Жара", page.ch.backend === "night"
                        ? "теплее 40 °C — заряд на паузе, пока не остынет до 37 °C; ядро Xiaomi ставит паузу только выше 80 %, ниже — заряд идёт, но ток ядро убавляет само"
                        : "на кабеле теплее 35 °C полчаса — верх 60 %; теплее 40 °C — заряд на паузе, пока не остынет до 37 °C"],
                    ["defender", "Защита ядра Google", "запасная, когда предела нет («Без предела», «до 100 % один раз»): 6 ч на кабеле в тепле — спустить до 80 %. С завода стоит «вхолостую», даже в Android"],
                    ["calibrate", "Калибровка процентов", "раз в месяц, в прохладе: от 15 % одним заходом до 100 %, полчаса — и назад к режиму"
                        + (page.ch.nextCalibration > 0 ? " · следующая — " + page.day(page.ch.nextCalibration) : "")
                        + (page.ch.lastFull > 0 ? " · последний 100 %: " + page.day(page.ch.lastFull) : "")],
                    ["lowoff", "Выключить на 3 %", "без кабеля на 3 % — аккуратно выключить телефон: лежать в нуле батарее вредно"]]
                    .filter(r => r[0] !== "defender" || page.ch.backend !== "night")   // TEMP-DEFEND — только у Google
                delegate: Item {
                    id: prow
                    required property var modelData
                    required property int index
                    readonly property string key: modelData[0]
                    readonly property bool on: key === "heat" ? page.ch.heat : key === "defender" ? page.ch.defender
                                             : key === "calibrate" ? page.ch.calibrate : page.ch.lowOff
                    width: col.width
                    height: desc.y + desc.height + 12
                    Line { visible: prow.index > 0; width: parent.width }
                    Label { y: 12; text: prow.modelData[1].toUpperCase() }
                    Meta {
                        id: desc
                        y: 31
                        width: prow.width - 160
                        color: DeskStyle.dim
                        text: prow.modelData[2]
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Seg { width: 64; text: "Выкл"; tone: prow.on ? "off" : "on"; onClicked: page.ch.call("SetOption", [prow.key, false]) }
                        Seg { width: 64; text: "Вкл"; tone: prow.on ? "on" : "off"; onClicked: page.ch.call("SetOption", [prow.key, true]) }
                    }
                }
            }
        }
    }
}
