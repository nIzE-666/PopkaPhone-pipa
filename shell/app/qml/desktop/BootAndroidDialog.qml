import QtQuick

FocusScope {
    id: dialog
    signal dismissed()
    Keys.onEscapePressed: if (!naomiSystemTools.busy) dismissed()

    Rectangle { anchors.fill: parent; color: "#a0000000" }
    MouseArea {
        anchors.fill: parent
        onWheel: (event) => { event.accepted = true }
    }
    DeskModule {
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 32)
        height: dialogBody.height + 32
        Column {
            id: dialogBody
            x: 16; y: 16
            width: parent.width - 32
            spacing: 12
            Text {
                width: parent.width
                text: "Перезагрузить в One UI"
                wrapMode: Text.Wrap
                color: DeskStyle.text
                font.family: DeskStyle.sans
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: naomiSystemTools.androidError || (naomiSystemTools.busy ? "Переключаемся в One UI…"
                      : "Сохраните работу. Планшет закроет сеанс Ubuntu Touch и загрузит установленную One UI.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: naomiSystemTools.androidError ? DeskStyle.bad : DeskStyle.text
                font.family: DeskStyle.sans
                font.pixelSize: 16
            }
            Row {
                spacing: 8
                NButton {
                    width: 112
                    centered: true
                    text: "Отмена"
                    focus: true
                    activeFocusOnTab: true
                    disabled: naomiSystemTools.busy
                    Keys.onReturnPressed: if (!disabled) clicked()
                    onClicked: dialog.dismissed()
                }
                NButton {
                    width: 160
                    centered: true
                    text: "Перезагрузить"
                    tone: "bad"
                    activeFocusOnTab: true
                    disabled: naomiSystemTools.busy
                    Keys.onReturnPressed: if (!disabled) clicked()
                    onClicked: naomiSystemTools.bootAndroid()
                }
            }
        }
    }
}
