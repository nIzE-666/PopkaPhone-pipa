import QtQuick
import ".."

Column {
    id: page
    property var sw
    spacing: 16

    component Label: Text {
        font.family: DeskStyle.sans
        font.pixelSize: 16
        color: DeskStyle.text
    }

    Label { text: "Ориентация" }
    Flow {
        width: page.width
        spacing: 6
        Repeater {
            model: [90, 0, 270, 180]
            NButton {
                required property int modelData
                width: 246
                centered: true
                text: (modelData % 180 ? "Горизонтально" : "Вертикально") + " (" + modelData + "°)"
                tone: naomiDisplay.sceneRotation === modelData ? "on" : "off"
                onClicked: naomiDisplay.chooseOrientation(modelData)
            }
        }
    }
    NButton {
        text: "Автоповорот"
        tone: naomiDisplay.autoRotate ? "on" : "off"
        onClicked: { naomiDisplay.autoRotate = !naomiDisplay.autoRotate }
    }
    Flow {
        width: page.width
        spacing: 8
        NButton {
            text: "Автояркость"
            tone: naomiPower.autoBrightness ? "on" : "off"
            onClicked: { naomiPower.autoBrightness = !naomiPower.autoBrightness }
        }
        NButton {
            visible: typeof naomiHaptics !== "undefined" && naomiHaptics !== null && naomiHaptics.available
            text: "Виброотклик клавиатуры"
            tone: typeof naomiHaptics !== "undefined" && naomiHaptics && naomiHaptics.enabled ? "on" : "off"
            onClicked: { if (typeof naomiHaptics !== "undefined" && naomiHaptics) naomiHaptics.enabled = !naomiHaptics.enabled }
        }
    }
    Label {
        text: "Режим ввода · "
            + ((typeof naomiSystemTools !== "undefined" && naomiSystemTools && naomiSystemTools.hardwareKeyboard)
               ? "Ноутбук (физ. клавиатура)" : "Планшет (сенсорный)")
            + " · USB-C DP: "
            + ((typeof naomiSystemTools !== "undefined" && naomiSystemTools && naomiSystemTools.externalDisplay)
               ? "подключён" : "нет")
        color: DeskStyle.muted
    }

    Label { text: "Яркость · " + Math.round(naomiPower.brightness * 100) + " %" }
    Item {
        width: Math.max(0, Math.min(page.width - 16, 440))
        height: 48
        DeskSlider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            touchMargin: 16
            value: naomiPower.brightness * 100
            onMoved: (value) => { naomiPower.brightness = value / 100 }
        }
    }

    Label { text: "Прокрутка пальцем в приложениях · " + sw.screen.touchScrollSpeed + " %" }
    Item {
        width: Math.max(0, Math.min(page.width - 16, 440))
        height: 48
        DeskSlider {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            touchMargin: 16
            from: 10
            stepSize: 5
            value: sw.screen.touchScrollSpeed
            onMoved: (value) => { sw.screen.touchScrollSpeed = value }
        }
    }

    Label {
        text: naomiDisplay.refreshHz > 1 ? "Частота панели · " + Math.round(naomiDisplay.refreshHz) + " Гц" : "Частота панели"
    }
    Flow {
        width: page.width
        spacing: 6
        Repeater {
            model: naomiDisplay.refreshRates
            NButton {
                required property int modelData
                width: 82
                centered: true
                text: modelData + " Гц"
                tone: Math.abs(naomiDisplay.refreshHz - modelData) < 0.5 ? "on" : "off"
                onClicked: naomiDisplay.setRefresh(modelData)
            }
        }
    }

    Label { text: "Гасить экран через" }
    Flow {
        width: page.width
        spacing: 6
        Repeater {
            model: [60, 180, 300, 600, 0]
            NButton {
                required property int modelData
                width: 100
                centered: true
                text: modelData > 0 ? modelData / 60 + " мин" : "никогда"
                tone: naomiPower.screenTimeout === modelData ? "on" : "off"
                onClicked: { naomiPower.screenTimeout = modelData }
            }
        }
    }
}
