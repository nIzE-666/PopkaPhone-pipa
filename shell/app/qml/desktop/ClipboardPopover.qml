// История буфера обмена и синхронизация с Windows ПК (mypc) — поповер под значком буфера на панели.
import QtQuick

Item {
    id: pop
    property bool open: false
    signal dismissed()

    width: 348
    height: Math.min(460, col.y + col.height + 14)

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

    MouseArea {
        anchors.fill: parent
        onWheel: (e) => {}
    }

    DeskModule { anchors.fill: parent }

    Column {
        id: col
        x: 16; y: 14
        width: 316
        spacing: 10

        Plate {
            width: 316
            num: "06"
            title: "БУФЕР"
            value: String(naomiSystemTools.clipboardHistory.length)
            valueColor: DeskStyle.amber
            note: naomiSystemTools.pcClipboardStatus
        }

        Row {
            width: 316
            spacing: 6
            NButton {
                width: 155
                centered: true
                disabled: naomiSystemTools.pcClipboardBusy
                text: "С ПК → Планшет"
                tone: "on"
                onClicked: naomiSystemTools.pullPcClipboard()
            }
            NButton {
                width: 155
                centered: true
                disabled: naomiSystemTools.pcClipboardBusy
                text: "Планшет → ПК"
                tone: "off"
                onClicked: naomiSystemTools.pushPcClipboard()
            }
        }

        Text {
            visible: naomiSystemTools.clipboardHistory.length === 0
            width: 316
            text: "История пуста — скопируй текст или забери его с ПК"
            font.family: DeskStyle.sans
            font.pixelSize: 13
            color: DeskStyle.dim
            wrapMode: Text.WordWrap
        }

        Flickable {
            visible: naomiSystemTools.clipboardHistory.length > 0
            width: 316
            height: Math.min(280, listCol.height)
            contentWidth: width
            contentHeight: listCol.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: listCol
                width: 316
                spacing: 6
                Repeater {
                    model: naomiSystemTools.clipboardHistory
                    delegate: Rectangle {
                        id: rowItem
                        required property string modelData
                        required property int index
                        width: 316
                        height: 44
                        radius: 6
                        color: itemArea.containsMouse ? DeskStyle.amberA(0.14) : DeskStyle.surface
                        border.width: 1
                        border.color: index === 0 ? DeskStyle.amberA(0.42) : DeskStyle.amberA(DeskStyle.line)

                        MouseArea {
                            id: itemArea
                            anchors.fill: parent
                            anchors.rightMargin: 34
                            hoverEnabled: true
                            onClicked: {
                                naomiSystemTools.copyClipboard(rowItem.modelData)
                                pop.dismissed()
                            }
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.right: rmBtn.left
                            anchors.leftMargin: 10
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            text: rowItem.modelData.replace(/\s+/g, " ").trim()
                            font.family: DeskStyle.sans
                            font.pixelSize: 13
                            color: rowItem.index === 0 ? DeskStyle.text : DeskStyle.muted
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            wrapMode: Text.WrapAnywhere
                        }

                        WinButton {
                            id: rmBtn
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "close"
                            danger: true
                            unit: 0.78
                            onClicked: naomiSystemTools.removeClipboardAt(rowItem.index)
                        }
                    }
                }
            }
        }

        Row {
            visible: naomiSystemTools.clipboardHistory.length > 0
            width: 316
            layoutDirection: Qt.RightToLeft
            NButton {
                width: 112
                centered: true
                text: "Очистить"
                tone: "off"
                onClicked: naomiSystemTools.clearClipboardHistory()
            }
        }
    }
}
