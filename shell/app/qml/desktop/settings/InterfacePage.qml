// Раздел «Интерфейс» в Настройках — часы (24ч/12ч, секунды, формат даты), ночной фильтр экрана, терминал, буфер, браузер.
import QtQuick
import ".."

Column {
    id: page
    property var sw
    spacing: 16

    component Title: Text {
        font.family: DeskStyle.sans
        font.pixelSize: 20
        font.weight: Font.DemiBold
        color: DeskStyle.text
    }
    component Label: Text {
        font.family: DeskStyle.sans
        font.pixelSize: 16
        color: DeskStyle.text
    }
    component Hint: Text {
        width: page.width
        wrapMode: Text.WordWrap
        font.family: DeskStyle.sans
        font.pixelSize: 13
        color: DeskStyle.muted
    }

    Title { text: "Интерфейс и панель" }

    // ---------- Часы и дата ----------
    Label { text: "Формат времени" }
    Flow {
        width: page.width
        spacing: 8
        NButton {
            width: 170
            centered: true
            text: "24-часовой (19:30)"
            tone: sw.ui.clock24h ? "on" : "off"
            onClicked: sw.ui.clock24h = true
        }
        NButton {
            width: 170
            centered: true
            text: "12-часовой (7:30 PM)"
            tone: !sw.ui.clock24h ? "on" : "off"
            onClicked: sw.ui.clock24h = false
        }
        NButton {
            width: 160
            centered: true
            text: sw.ui.clockSeconds ? "Секунды: ВКЛ" : "Секунды: ВЫКЛ"
            tone: sw.ui.clockSeconds ? "on" : "off"
            onClicked: sw.ui.clockSeconds = !sw.ui.clockSeconds
        }
    }

    Label { text: "Дата в верхней панели" }
    Flow {
        width: page.width
        spacing: 8
        Repeater {
            model: [
                { v: 1, t: "ВТ 24 НОЯ" },
                { v: 2, t: "24.11" },
                { v: 3, t: "TUE 24" },
                { v: 0, t: "Скрыть" }
            ]
            NButton {
                required property var modelData
                width: 120
                centered: true
                text: modelData.t
                tone: sw.ui.dateFormat === modelData.v ? "on" : "off"
                onClicked: sw.ui.dateFormat = modelData.v
            }
        }
    }

    // ---------- Ночной фильтр экрана ----------
    Label { text: "Ночной тёплый фильтр · " + Math.round(sw.ui.nightFilter * 100) + " %" }
    Item {
        width: Math.max(0, Math.min(page.width - 16, 440))
        height: 44
        DeskSlider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            touchMargin: 16
            from: 0
            to: 60
            stepSize: 5
            value: Math.round(sw.ui.nightFilter * 100)
            onMoved: (v) => { sw.ui.nightFilter = v / 100 }
        }
    }
    Flow {
        width: page.width
        spacing: 8
        Repeater {
            model: [
                { v: 0.0, t: "Выкл" },
                { v: 0.2, t: "Мягкий (20%)" },
                { v: 0.4, t: "Ночь (40%)" }
            ]
            NButton {
                required property var modelData
                width: 132
                centered: true
                text: modelData.t
                tone: Math.abs(sw.ui.nightFilter - modelData.v) < 0.03 ? "on" : "off"
                onClicked: sw.ui.nightFilter = modelData.v
            }
        }
    }

    // ---------- Терминал ----------
    Label { text: "Терминал · шрифт " + sw.term.fontSize + " px" }
    Item {
        width: Math.max(0, Math.min(page.width - 16, 440))
        height: 44
        DeskSlider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            touchMargin: 16
            from: 10
            to: 26
            stepSize: 1
            value: sw.term.fontSize
            onMoved: (v) => { sw.term.fontSize = Math.round(v) }
        }
    }
    Label { text: "Сенсорная панель спецклавиш в терминале" }
    Flow {
        width: page.width
        spacing: 8
        Repeater {
            model: [
                { v: 0, t: "Авто (по клав.)" },
                { v: 1, t: "Всегда показать" },
                { v: 2, t: "Скрыть" }
            ]
            NButton {
                required property var modelData
                width: 156
                centered: true
                text: modelData.t
                tone: sw.ui.extraKeysMode === modelData.v ? "on" : "off"
                onClicked: sw.ui.extraKeysMode = modelData.v
            }
        }
    }

    // ---------- Буфер обмена и ПК ----------
    Label { text: "Буфер обмена · записей в истории: " + naomiSystemTools.clipboardHistory.length }
    Flow {
        width: page.width
        spacing: 8
        NButton {
            width: 160
            centered: true
            disabled: naomiSystemTools.pcClipboardBusy
            text: "С ПК → Планшет"
            tone: "on"
            onClicked: naomiSystemTools.pullPcClipboard()
        }
        NButton {
            width: 160
            centered: true
            disabled: naomiSystemTools.pcClipboardBusy
            text: "Планшет → ПК"
            tone: "off"
            onClicked: naomiSystemTools.pushPcClipboard()
        }
        NButton {
            width: 130
            centered: true
            disabled: naomiSystemTools.clipboardHistory.length === 0
            text: "Очистить"
            tone: "off"
            onClicked: naomiSystemTools.clearClipboardHistory()
        }
    }
    Hint {
        visible: naomiSystemTools.pcClipboardStatus !== ""
        text: naomiSystemTools.pcClipboardStatus
    }
}
