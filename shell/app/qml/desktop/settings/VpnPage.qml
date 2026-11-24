import QtQuick
import ".."

Column {
    id: page
    property var sw
    spacing: 12
    onVisibleChanged: if (visible) naomiSystemTools.refreshVpn()
    Component.onCompleted: if (visible) naomiSystemTools.refreshVpn()

    Text {
        text: "VPN"
        font.family: DeskStyle.sans
        font.pixelSize: 20
        font.weight: Font.DemiBold
        color: DeskStyle.text
    }
    Text {
        width: page.width
        text: naomiSystemTools.vpnStatus || (naomiSystemTools.busy ? "Проверяем состояние VPN…" : "Состояние VPN неизвестно")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: DeskStyle.sans
        font.pixelSize: 16
        color: DeskStyle.text
    }
    Text {
        width: page.width
        visible: text !== ""
        text: naomiSystemTools.vpnError
        wrapMode: Text.Wrap
        font.family: DeskStyle.sans
        font.pixelSize: 14
        color: DeskStyle.bad
    }
    Row {
        spacing: 8
        NButton {
            width: 148
            centered: true
            text: "Отключить VPN"
            disabled: naomiSystemTools.busy
            onClicked: naomiSystemTools.setVpn("")
        }
        NButton {
            width: 120
            centered: true
            text: "Обновить"
            disabled: naomiSystemTools.busy
            onClicked: naomiSystemTools.refreshVpn()
        }
    }
    Text {
        visible: !naomiSystemTools.busy && naomiSystemTools.vpnProfiles.length === 0 && !naomiSystemTools.vpnError
        text: "Профили VPN не найдены"
        font.family: DeskStyle.sans
        font.pixelSize: 16
        color: DeskStyle.muted
    }
    Repeater {
        model: naomiSystemTools.vpnProfiles
        delegate: Item {
            id: row
            required property string modelData
            width: page.width
            height: DeskStyle.btnH
            Text {
                width: row.width - connectButton.width - 12
                height: row.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: row.modelData
                textFormat: Text.PlainText
                font.family: DeskStyle.sans
                font.pixelSize: 16
                color: DeskStyle.text
            }
            NButton {
                id: connectButton
                anchors.right: parent.right
                width: 144
                centered: true
                text: "Подключить"
                disabled: naomiSystemTools.busy
                onClicked: naomiSystemTools.setVpn(row.modelData)
            }
        }
    }
}
