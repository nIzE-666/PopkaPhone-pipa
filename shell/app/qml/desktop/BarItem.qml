// Элемент панели — как ui_item / ui_bar_item в bcdesk: без рамки, под мышью — янтарная заливка .07, нажат — .12 и
// содержимое на точку ниже; open — окно под элементом открыто: та же заливка, что под мышью. Курсор — рука, как над
// кнопками панели у bcdesk. danger — крестик: заливка красная, как у крестика окна (WinButton). clicked — с клавишами
// (Shift на крестике — закрыть, а не свернуть).
import QtQuick

Item {
    id: item
    property bool open: false
    property bool danger: false
    readonly property bool hovered: hover.hovered
    readonly property bool pressed: tap.pressed
    readonly property real dy: pressed ? 1 : 0
    signal clicked(int modifiers)
    signal longPressed()
    height: DeskStyle.appH

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: item.dy
        anchors.bottomMargin: -item.dy
        color: item.danger ? Qt.rgba(217 / 255, 112 / 255, 95 / 255, item.pressed ? 0.26 : 0.18)
                           : DeskStyle.amberA(item.pressed ? DeskStyle.itemPress : DeskStyle.itemHover)
        visible: item.hovered || item.pressed || item.open
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler {
        id: tap
        longPressThreshold: 0.45
        onTapped: item.clicked(tap.point.modifiers)
        onLongPressed: item.longPressed()
    }
}
