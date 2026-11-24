// Окно батареи — под значком батареи на панели (docs/DESKTOP.md, «Батарея»); вид выбран пользователем из десяти
// макетов — «Заряд и мощность». Шильдик «05 БАТАРЕЯ 77%» и что с зарядкой; «24 Ч · 7 ДН» — за сколько график
// (BatteryChart: заряд и мощность на одной шкале времени); прогноз — «осталось ≈ 6 ч 40 мин» от батареи, «док не
// успевает» (на зарядке, а заряд уходит), «до полной ≈ …», пересчёт раз в секунду по сглаженному за минуту току;
// ниже — ток, мощность, напряжение, температура батареи, запас, энергия, циклы, здоровье; внизу — когда последний раз
// зарядилась до 100 % и вход зарядки (или с каких пор от батареи). Модуль 348 точек, как монитор системы; появляется,
// как окно звука; щелчок мимо или по значку ещё раз — закрыть. Данные — DeskBattery (naomid Sample и History).
import QtQuick

Item {
    id: pop
    property bool open: false
    property bool clock24h: true
    property var battery               // DeskBattery

    width: 348
    height: col.y + col.height + 12

    // ---------- появление — как у окна звука (anim_curve): t — ход открытия или закрытия, 0…1 ----------
    property real openT: 1
    property real closeT: 1
    onOpenChanged: {
        if (open) {
            closeAnim.stop()
            closeT = 1
            openT = 0
            openAnim.restart()
        } else {
            openAnim.stop()
            closeT = 0
            closeAnim.restart()
        }
    }
    NumberAnimation { id: openAnim; target: pop; property: "openT"; from: 0; to: 1; duration: 110 }
    NumberAnimation { id: closeAnim; target: pop; property: "closeT"; from: 0; to: 1; duration: 150 }
    visible: open || closeAnim.running
    transformOrigin: Item.Top
    scale: open ? 0.98 + 0.02 * (1 - Math.pow(1 - openT, 3)) : 1 - 0.01 * (1 - Math.pow(1 - closeT, 3))
    opacity: open ? 1 : (Math.exp(-closeT * 150 / 38) - Math.exp(-150 / 38)) / (1 - Math.exp(-150 / 38))

    MouseArea {   // щелчок по пустому месту окна — ничего, и не мимо него; колесо сквозь окно не проходит
        anchors.fill: parent
        onWheel: (e) => {}
    }

    DeskModule { anchors.fill: parent }

    // ---------- числа ----------
    readonly property var s: battery.sample
    readonly property bool ready: s.chargeMah !== undefined
    readonly property string dimC: "" + DeskStyle.dim
    function u(t) { return ' <font color="' + dimC + '">' + t + "</font>" }       // единица — через пробел
    function grp(v) { return String(Math.round(Math.abs(v))).replace(/\B(?=(\d{3})+(?!\d))/g, " ") }
    function sgn(v) { return v >= 0.5 ? "+" : v <= -0.5 ? "−" : "" }
    function fx(v, d) { return v.toFixed(d).replace(".", ",") }
    function clock(t) {   // как часы панели: 18:52 или 6:52 PM
        const d = new Date(t * 1000), h = d.getHours(), m = d.getMinutes()
        const mm = (m < 10 ? "0" : "") + m
        if (pop.clock24h)
            return (h < 10 ? "0" : "") + h + ":" + mm
        return (h % 12 === 0 ? 12 : h % 12) + ":" + mm + (h < 12 ? " AM" : " PM")
    }
    readonly property var f: battery.forecast
    readonly property real watts: battery.currentMa * (s.voltageMv || 0) / 1e6
    readonly property string stateText: battery.low ? "МАЛО ЗАРЯДА" : !battery.plugged ? "ОТ БАТАРЕИ"
                                  : f.kind === "short" ? "ДОК НЕ УСПЕВАЕТ" : battery.percent >= 100 ? "НА ЗАРЯДКЕ" : "ЗАРЯЖАЕТСЯ"

    component Foot: Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: 1
        font.family: DeskStyle.mono
        font.pixelSize: 11
        font.letterSpacing: 0.7
        color: DeskStyle.dim
    }
    // показания: подпись и значение (единица — тускло); модель — номера, значения — привязки: раз в секунду меняется
    // текст, строки не пересоздаются
    readonly property var labels: ["Ток", "Мощность", "Напряжение", "Тепло", "Запас", "Энергия", "Циклы", "Здоровье"]
    function value(i) {
        const v = s.voltageMv || 0, q = s.chargeMah || 0, I = battery.currentMa
        switch (i) {
        case 0: return sgn(I) + grp(I) + u("мА")
        case 1: return sgn(watts) + fx(Math.abs(watts), 2) + u("Вт")
        case 2: return fx(v / 1000, 3) + u("В")
        case 3: return fx(s.batteryC || 0, 1) + u("°C")
        case 4: return grp(q) + u("мА·ч")
        case 5: return grp(q * v / 1000) + u("мВт·ч")
        case 6: return String(s.cycles || 0)
        default: return (s.designMah ? Math.round(s.fullMah / s.designMah * 100) : 0) + u("%")
        }
    }

    Column {
        id: col
        x: 16; y: 14
        width: 316

        Plate {
            width: 316
            num: "05"
            title: "БАТАРЕЯ"
            value: pop.battery.percent + "%"
            valueColor: pop.battery.low ? DeskStyle.bad : pop.battery.plugged ? DeskStyle.teal : DeskStyle.text
            note: pop.stateText
            noteColor: pop.battery.low ? DeskStyle.bad : pop.f.kind === "short" ? DeskStyle.amber
                     : pop.battery.plugged ? DeskStyle.teal : DeskStyle.muted
        }
        Item { width: 1; height: 8 }

        // ---------- за сколько график: 24 Ч · 7 ДН ----------
        Row {
            anchors.right: parent.right
            spacing: 2
            Repeater {
                model: [["day", "24 Ч"], ["week", "7 ДН"]]
                delegate: Item {
                    id: rangeItem
                    required property var modelData
                    readonly property bool on: pop.battery.range === rangeItem.modelData[0]
                    width: rangeText.implicitWidth + 12
                    height: 18
                    Rectangle {
                        anchors.fill: parent
                        radius: 5
                        color: rangeItem.on ? DeskStyle.amberA(0.08) : hover.hovered ? DeskStyle.amberA(DeskStyle.itemHover) : "transparent"
                    }
                    Text {
                        id: rangeText
                        anchors.centerIn: parent
                        font.family: DeskStyle.mono
                        font.pixelSize: 11
                        font.letterSpacing: 0.9
                        text: rangeItem.modelData[1]
                        color: rangeItem.on ? DeskStyle.amber : hover.hovered ? DeskStyle.text : DeskStyle.dim
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: pop.battery.range = rangeItem.modelData[0] }
                }
            }
        }
        BatteryChart {
            history: pop.battery.history
        }

        // ---------- прогноз ----------
        Item { width: 1; height: 8 }
        Rectangle { width: 316; height: 1; color: DeskStyle.amberA(DeskStyle.line) }
        Item { width: 1; height: 9 }
        Item {
            width: 316
            height: 22
            Text {
                anchors.verticalCenter: parent.verticalCenter
                font.family: DeskStyle.mono
                font.pixelSize: 12
                font.letterSpacing: 1.2
                color: DeskStyle.dim
                text: pop.f.kind === "full" ? "ДО ПОЛНОЙ" : pop.f.kind === "hold" ? "НА ЗАРЯДКЕ" : "ОСТАЛОСЬ"
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                font.family: DeskStyle.mono
                font.pixelSize: 17
                font.weight: Font.Medium
                color: pop.f.kind === "full" || pop.f.kind === "hold" ? DeskStyle.teal : pop.f.kind === "short" ? DeskStyle.amber
                     : pop.f.hours >= 0 && pop.f.hours < 1 ? DeskStyle.bad : DeskStyle.text
                text: pop.f.kind === "" ? "…" : pop.f.kind === "hold" ? "держит " + pop.battery.percent + " %"
                    : pop.f.hours < 0 ? "–" : "≈ " + pop.battery.duration(pop.f.hours)
            }
        }
        Text {
            width: 316
            font.family: DeskStyle.mono
            font.pixelSize: 11
            font.letterSpacing: 0.2
            color: DeskStyle.dim
            text: !pop.ready ? "" : "ток сейчас " + pop.sgn(pop.s.currentMa) + pop.grp(pop.s.currentMa) + " мА · за минуту "
                                    + pop.sgn(pop.battery.currentMa) + pop.grp(pop.battery.currentMa) + " мА"
        }

        // ---------- показания ----------
        Item { width: 1; height: 10 }
        Grid {
            columns: 2
            columnSpacing: 18
            rowSpacing: 6
            Repeater {
                model: pop.labels.length
                delegate: Item {
                    id: row
                    required property int index
                    width: 149
                    height: 15
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: DeskStyle.mono
                        font.pixelSize: 12
                        font.letterSpacing: 1.2
                        font.capitalization: Font.AllUppercase
                        color: DeskStyle.dim
                        text: pop.labels[row.index]
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: DeskStyle.mono
                        font.pixelSize: 13
                        textFormat: Text.StyledText
                        color: pop.ready ? (row.index === 3 && pop.s.batteryC > 40 ? DeskStyle.amber : DeskStyle.text) : DeskStyle.dim
                        text: pop.ready ? pop.value(row.index) : "–"
                    }
                }
            }
        }
        Item { width: 1; height: 6 }
        Text {
            font.family: DeskStyle.mono
            font.pixelSize: 11
            font.letterSpacing: 0.2
            color: DeskStyle.dim
            text: pop.ready ? "ёмкость " + pop.grp(pop.s.fullMah) + " из " + pop.grp(pop.s.designMah) + " мА·ч по паспорту" : ""
        }

        // ---------- подвал: последняя полная зарядка и вход ----------
        Item { width: 1; height: 10 }
        Rectangle { width: 316; height: 1; color: DeskStyle.amberA(DeskStyle.line) }
        Item {
            width: 316
            height: 28
            Foot {
                text: pop.battery.history && pop.battery.history.lastFull ? "ДО 100 % · " + pop.clock(pop.battery.history.lastFull) : ""
            }
            Foot {
                anchors.right: parent.right
                text: pop.battery.plugged
                      ? (pop.ready && pop.s.inputMa > 0 ? "USB · " + pop.fx(pop.s.inputMv / 1000, 1) + " В · " + pop.fx(pop.s.inputMa / 1000, 2) + " А" : "")
                      : pop.battery.history && pop.battery.history.since ? "ОТ БАТАРЕИ С " + pop.clock(pop.battery.history.since) : ""
            }
        }
    }
}
