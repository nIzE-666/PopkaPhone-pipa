// Раздел «Система» в Настройках — профиль CPU SM8250, службы планшета, часовой пояс, память ZRAM и очистка журнала.
import QtQuick
import ".."

Column {
    id: page
    property var sw
    spacing: 16

    onVisibleChanged: if (visible) naomiSystemTools.refreshSystemInfo()
    Component.onCompleted: if (visible) naomiSystemTools.refreshSystemInfo()

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

    Row {
        width: page.width
        spacing: 12
        Title { text: "Система (Xiaomi Pad 6)" }
        NButton {
            width: 112
            centered: true
            disabled: naomiSystemTools.systemBusy
            text: "Обновить"
            onClicked: naomiSystemTools.refreshSystemInfo()
        }
    }

    // ---------- Сводка железа ----------
    Rectangle {
        width: page.width
        height: infoCol.implicitHeight + 20
        radius: 8
        color: DeskStyle.surface
        border.width: 1
        border.color: DeskStyle.amberA(DeskStyle.line)

        Column {
            id: infoCol
            x: 14; y: 10
            width: parent.width - 28
            spacing: 6
            Text {
                width: parent.width
                text: "CPU: " + (naomiSystemTools.sysInfo.cpuTemp || "—") + " °C  ·  Батарея: "
                    + (naomiSystemTools.sysInfo.batTemp || "—") + " °C  ·  IP: "
                    + (naomiSystemTools.sysInfo.ip || "нет")
                font.family: DeskStyle.mono
                font.pixelSize: 13
                color: DeskStyle.text
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: "ZRAM: " + (naomiSystemTools.sysInfo.zramUsedMb || 0) + " / "
                    + (naomiSystemTools.sysInfo.zramTotalMb || 0) + " МБ  ·  Диск свободно: "
                    + (naomiSystemTools.sysInfo.diskFreeGb || "—") + " / "
                    + (naomiSystemTools.sysInfo.diskTotalGb || "—") + " ГБ"
                font.family: DeskStyle.mono
                font.pixelSize: 13
                color: DeskStyle.muted
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: "Пояс: " + (naomiSystemTools.sysInfo.timezone || "—")
                    + "  ·  Ядро: " + (naomiSystemTools.sysInfo.kernel || "—")
                font.family: DeskStyle.mono
                font.pixelSize: 12
                color: DeskStyle.dim
                elide: Text.ElideRight
            }
        }
    }

    Hint {
        visible: naomiSystemTools.systemStatus !== ""
        text: naomiSystemTools.systemStatus
        color: DeskStyle.amber
    }

    // ---------- Профиль производительности CPU ----------
    Label { text: "Профиль процессора SM8250" }
    Flow {
        width: page.width
        spacing: 8
        Repeater {
            model: [
                { id: "powersave", name: "Эконом" },
                { id: "schedutil", name: "Баланс" },
                { id: "performance", name: "Максимум" }
            ]
            NButton {
                required property var modelData
                width: 148
                centered: true
                disabled: naomiSystemTools.systemBusy
                text: modelData.name
                tone: naomiSystemTools.powerProfile === modelData.id ? "on" : "off"
                onClicked: naomiSystemTools.setPowerProfile(modelData.id)
            }
        }
    }
    Hint {
        text: "Баланс (schedutil) — оптимум: холодный в простое, мгновенно разгоняется при касаниях."
    }

    // ---------- Службы планшета ----------
    Label { text: "Службы" }
    Column {
        width: page.width
        spacing: 8
        Repeater {
            model: naomiSystemTools.services
            delegate: Rectangle {
                id: svcRow
                required property var modelData
                width: page.width
                height: 54
                radius: 6
                color: DeskStyle.surface
                border.width: 1
                border.color: modelData.active ? DeskStyle.amberA(0.32) : DeskStyle.amberA(DeskStyle.line)

                Column {
                    anchors.left: parent.left
                    anchors.right: svcBtn.left
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        width: parent.width
                        text: svcRow.modelData.name
                        font.family: DeskStyle.sans
                        font.pixelSize: 15
                        color: DeskStyle.text
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: svcRow.modelData.desc
                        font.family: DeskStyle.sans
                        font.pixelSize: 12
                        color: DeskStyle.dim
                        elide: Text.ElideRight
                    }
                }
                NButton {
                    id: svcBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 118
                    centered: true
                    disabled: naomiSystemTools.systemBusy || !svcRow.modelData.controllable
                    text: svcRow.modelData.active ? "Работает" : "Выкл"
                    tone: svcRow.modelData.active ? "on" : "off"
                    onClicked: naomiSystemTools.toggleService(svcRow.modelData.id, !svcRow.modelData.active)
                }
            }
        }
    }

    // ---------- Часовой пояс и время ----------
    Label { text: "Часовой пояс и синхронизация времени" }
    Flow {
        width: page.width
        spacing: 8
        Repeater {
            model: [
                { tz: "Europe/Moscow", name: "Москва (+3)" },
                { tz: "Asia/Yekaterinburg", name: "Екб (+5)" },
                { tz: "Asia/Novosibirsk", name: "Нск (+7)" },
                { tz: "Asia/Irkutsk", name: "Иркутск (+8)" },
                { tz: "Asia/Yakutsk", name: "Якутск (+9)" },
                { tz: "Asia/Vladivostok", name: "Владив. (+10)" }
            ]
            NButton {
                required property var modelData
                width: 142
                centered: true
                disabled: naomiSystemTools.systemBusy
                text: modelData.name
                tone: naomiSystemTools.sysInfo.timezone === modelData.tz ? "on" : "off"
                onClicked: naomiSystemTools.setTimezone(modelData.tz)
            }
        }
    }

    // ---------- Обслуживание ----------
    Flow {
        width: page.width
        spacing: 8
        NButton {
            width: 196
            centered: true
            disabled: naomiSystemTools.systemBusy
            text: "Синхронизировать NTP"
            tone: "off"
            onClicked: naomiSystemTools.syncTime()
        }
        NButton {
            width: 210
            centered: true
            disabled: naomiSystemTools.systemBusy
            text: "Очистить журнал (20 МБ)"
            tone: "off"
            onClicked: naomiSystemTools.cleanJournal()
        }
    }
}
