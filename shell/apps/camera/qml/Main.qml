import QtQuick
import QtQuick.Window
import Naomi.Camera

Window {
    id: win
    width: 1280
    height: 800
    visible: true
    color: "#0e0f0c"
    title: "Камера"

    readonly property color bgColor: "#0e0f0c"
    readonly property color panelColor: "#161614"
    readonly property color pillColor: "#1b1b18"
    readonly property color borderColor: "#2d2b26"
    readonly property color amberColor: "#d8b25a"
    readonly property color textColor: "#ece9e3"
    readonly property color mutedColor: "#9c968b"
    readonly property color recordRed: "#d9705f"

    property int timerSetting: 0        // 0, 3, 10 seconds
    property int countdownLeft: 0
    property string toastText: ""

    function flashLabel(mode) {
        if (mode === 1) return "⚡ Авто"
        if (mode === 2) return "⚡ Вкл"
        if (mode === 3) return "🔦 Фонарик"
        return "⚡ Выкл"
    }

    function formatRecTime(sec) {
        const m = Math.floor(sec / 60)
        const s = sec % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    function showToast(msg) {
        if (!msg) return
        toastText = msg
        toastHideTimer.restart()
    }

    function triggerShutter() {
        if (camera.mode === "video") {
            camera.toggleVideo()
            return
        }
        if (countdownLeft > 0) {
            countdownTimer.stop()
            countdownLeft = 0
            showToast("Таймер отменён")
            return
        }
        if (timerSetting > 0) {
            countdownLeft = timerSetting
            countdownTimer.restart()
        } else {
            camera.takePhoto()
        }
    }

    Connections {
        target: camera
        function onShutterTriggered() {
            shutterFlashAnim.restart()
        }
        function onToastMessage(message) {
            win.showToast(message)
        }
        function onFocusCompleted() {
            reticleFade.restart()
        }
    }

    Timer {
        id: countdownTimer
        interval: 1000
        repeat: true
        onTriggered: {
            if (win.countdownLeft > 1) {
                win.countdownLeft -= 1
            } else {
                countdownTimer.stop()
                win.countdownLeft = 0
                camera.takePhoto()
            }
        }
    }

    Timer {
        id: toastHideTimer
        interval: 2400
        onTriggered: win.toastText = ""
    }

    // Reusable top-bar pill button
    component PillButton: Rectangle {
        id: pill
        property string label: ""
        property bool active: false
        property bool danger: false
        signal clicked()

        height: 40
        implicitWidth: Math.max(72, pillText.implicitWidth + 26)
        radius: 20
        color: danger ? Qt.rgba(217 / 255, 112 / 255, 95 / 255, 0.22)
             : active ? Qt.rgba(216 / 255, 178 / 255, 90 / 255, 0.18)
             : win.pillColor
        border.width: 1
        border.color: danger ? win.recordRed : (active ? win.amberColor : win.borderColor)
        scale: pillTap.pressed ? 0.96 : 1.0
        Behavior on scale { NumberAnimation { duration: 90 } }

        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            color: pill.danger ? win.recordRed : (pill.active ? win.amberColor : win.textColor)
            font.pixelSize: 14
            font.weight: Font.Medium
        }

        TapHandler {
            id: pillTap
            onTapped: pill.clicked()
        }
    }

    // -----------------------------------------------------------------------
    // Center Viewfinder (fills area between top and bottom bars)
    // -----------------------------------------------------------------------
    Item {
        id: stage
        anchors.top: topBar.bottom
        anchors.bottom: bottomBar.top
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true

        CameraViewfinder {
            id: viewfinder
            anchors.fill: parent
            engine: camera
            rotationDegrees: camera.rotationDegrees
            mirrored: camera.mirrored
            fillCover: true

            TapHandler {
                onTapped: (point) => {
                    if (win.countdownLeft > 0) {
                        countdownTimer.stop()
                        win.countdownLeft = 0
                        win.showToast("Таймер отменён")
                        return
                    }
                    const px = point.position.x
                    const py = point.position.y
                    focusReticle.x = px - focusReticle.width / 2
                    focusReticle.y = py - focusReticle.height / 2
                    reticleShowAnim.restart()
                    reticleFade.restart()
                    const sensorPt = viewfinder.mapPointToSensor(px, py)
                    camera.focusAt(sensorPt.x, sensorPt.y)
                }
            }
        }

        // Animated amber tap-to-focus reticle
        Rectangle {
            id: focusReticle
            width: 78
            height: 78
            radius: 10
            color: "transparent"
            border.color: win.amberColor
            border.width: 2
            opacity: 0
            scale: 1.0

            Rectangle {
                anchors.centerIn: parent
                width: 6
                height: 6
                radius: 3
                color: win.amberColor
            }

            ParallelAnimation {
                id: reticleShowAnim
                NumberAnimation { target: focusReticle; property: "opacity"; from: 0.0; to: 1.0; duration: 110 }
                NumberAnimation { target: focusReticle; property: "scale"; from: 1.38; to: 1.0; duration: 190; easing.type: Easing.OutCubic }
            }

            SequentialAnimation {
                id: reticleFade
                PauseAnimation { duration: 1050 }
                NumberAnimation { target: focusReticle; property: "opacity"; to: 0.0; duration: 280 }
            }
        }

        // Recording badge: red pulsing dot + REC MM:SS
        Rectangle {
            id: recBadge
            visible: camera.recording
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 16
            height: 36
            width: recRow.implicitWidth + 28
            radius: 18
            color: Qt.rgba(22 / 255, 22 / 255, 20 / 255, 0.88)
            border.color: win.recordRed
            border.width: 1

            Row {
                id: recRow
                anchors.centerIn: parent
                spacing: 9

                Rectangle {
                    id: recDot
                    width: 10
                    height: 10
                    radius: 5
                    anchors.verticalCenter: parent.verticalCenter
                    color: win.recordRed

                    SequentialAnimation on opacity {
                        running: camera.recording
                        loops: Animation.Infinite
                        NumberAnimation { from: 1.0; to: 0.25; duration: 550 }
                        NumberAnimation { from: 0.25; to: 1.0; duration: 550 }
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "REC " + win.formatRecTime(camera.recordingSeconds)
                    color: win.textColor
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
            }
        }

        // Timer countdown overlay
        Rectangle {
            visible: win.countdownLeft > 0
            anchors.centerIn: parent
            width: 148
            height: 148
            radius: 74
            color: Qt.rgba(14 / 255, 15 / 255, 12 / 255, 0.72)
            border.color: win.amberColor
            border.width: 2

            Column {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: String(win.countdownLeft)
                    color: win.amberColor
                    font.pixelSize: 64
                    font.weight: Font.Bold
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "нажмите для отмены"
                    color: win.mutedColor
                    font.pixelSize: 11
                }
            }
        }

        // Loading / error overlay when camera is not streaming yet
        Rectangle {
            visible: !camera.ready
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            height: statusCol.implicitHeight + 36
            radius: 16
            color: Qt.rgba(22 / 255, 22 / 255, 20 / 255, 0.92)
            border.color: camera.errorMessage ? win.recordRed : win.borderColor
            border.width: 1

            Column {
                id: statusCol
                anchors.centerIn: parent
                width: parent.width - 36
                spacing: 8

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: camera.errorMessage ? "Камера недоступна" : "Запуск камеры…"
                    color: camera.errorMessage ? win.recordRed : win.amberColor
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }

                Text {
                    visible: camera.errorMessage.length > 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: camera.errorMessage
                    color: win.mutedColor
                    font.pixelSize: 13
                }
            }
        }

        // Toast pill at bottom of viewfinder
        Rectangle {
            visible: win.toastText.length > 0
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 16
            height: 36
            width: Math.min(parent.width - 40, toastLabel.implicitWidth + 30)
            radius: 18
            color: Qt.rgba(22 / 255, 22 / 255, 20 / 255, 0.9)
            border.color: win.amberColor
            border.width: 1

            Text {
                id: toastLabel
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 24)
                elide: Text.ElideMiddle
                text: win.toastText
                color: win.textColor
                font.pixelSize: 13
            }
        }

        // Brief white shutter flash animation
        Rectangle {
            id: shutterFlash
            anchors.fill: parent
            color: "#ffffff"
            opacity: 0

            SequentialAnimation {
                id: shutterFlashAnim
                NumberAnimation { target: shutterFlash; property: "opacity"; from: 0.0; to: 0.78; duration: 45 }
                NumberAnimation { target: shutterFlash; property: "opacity"; from: 0.78; to: 0.0; duration: 190; easing.type: Easing.OutQuad }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Top Controls Bar
    // -----------------------------------------------------------------------
    Rectangle {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 68
        color: win.panelColor

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: win.borderColor
        }

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 20
            spacing: 10

            PillButton {
                label: "📷 " + camera.cameraLabel
                active: true
                onClicked: camera.switchCamera()
            }

            PillButton {
                visible: camera.flashSupported
                label: win.flashLabel(camera.flashMode)
                active: camera.flashMode !== 0
                onClicked: camera.cycleFlashMode()
            }

            PillButton {
                label: "📐 " + camera.resolutionLabel
                active: camera.resolutionPreset === 0
                onClicked: camera.cycleResolution()
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 20
            spacing: 10

            PillButton {
                label: win.timerSetting === 0 ? "⏱ Выкл" : ("⏱ " + win.timerSetting + "с")
                active: win.timerSetting > 0
                onClicked: {
                    if (win.timerSetting === 0) win.timerSetting = 3
                    else if (win.timerSetting === 3) win.timerSetting = 10
                    else win.timerSetting = 0
                }
            }

            PillButton {
                label: "⇄ Зеркало"
                active: camera.mirrored
                onClicked: camera.toggleMirror()
            }

            PillButton {
                label: camera.rotationDegrees === 0 ? "↻" : ("↻ " + camera.rotationDegrees + "°")
                active: camera.rotationDegrees !== 0
                onClicked: camera.rotatePreview()
            }
        }
    }

    // -----------------------------------------------------------------------
    // Bottom Controls Bar (Thumbnail | Zoom Slider | Shutter | Photo/Video)
    // -----------------------------------------------------------------------
    Rectangle {
        id: bottomBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 128
        color: win.panelColor

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: win.borderColor
        }

        // Left: Last photo thumbnail + filename
        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 24
            spacing: 12

            Rectangle {
                id: thumbBox
                width: 68
                height: 68
                radius: 14
                color: win.pillColor
                border.color: camera.lastPhotoPath.length > 0 ? win.amberColor : win.borderColor
                border.width: 1.5
                clip: true

                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: camera.lastPhotoPath.length > 0 ? camera.lastPhotoUrl : ""
                    fillMode: Image.PreserveAspectCrop
                    cache: false
                    asynchronous: true
                    visible: camera.lastPhotoPath.length > 0
                }

                Text {
                    anchors.centerIn: parent
                    visible: camera.lastPhotoPath.length === 0
                    text: "🖼"
                    font.pixelSize: 24
                    color: win.mutedColor
                }

                TapHandler {
                    onTapped: camera.openLastPhoto()
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                width: 150
                visible: win.width >= 960

                Text {
                    text: camera.lastPhotoPath.length > 0 ? "Последний кадр" : "Снимков нет"
                    color: win.textColor
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }

                Text {
                    width: parent.width
                    elide: Text.ElideMiddle
                    text: camera.lastPhotoPath.length > 0
                          ? camera.lastPhotoPath.split("/").pop()
                          : "~/Pictures/Camera"
                    color: win.mutedColor
                    font.pixelSize: 11
                }
            }
        }

        // Center-left: Zoom Slider (1.0x .. 6.0x)
        Item {
            id: zoomBox
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: shutterContainer.left
            anchors.rightMargin: 36
            width: Math.min(260, Math.max(160, win.width * 0.21))
            height: 62

            Row {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 8

                Text {
                    text: "Зум"
                    color: win.mutedColor
                    font.pixelSize: 12
                }

                Text {
                    text: camera.zoomFactor.toFixed(1) + "×"
                    color: win.amberColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }

            Item {
                id: zoomSlider
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 36

                property real liveVal: -1
                readonly property real currentNorm: liveVal >= 0
                    ? liveVal
                    : (camera.maxZoom > 0 ? camera.zoom / camera.maxZoom : 0)

                function applyX(px) {
                    const usable = Math.max(1, width - knob.width)
                    const v = Math.max(0, Math.min(1, (px - knob.width / 2) / usable))
                    liveVal = v
                    camera.setZoomNormalized(v)
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: win.borderColor

                    Rectangle {
                        width: knob.x + knob.width / 2
                        height: parent.height
                        radius: 2
                        color: win.amberColor
                    }
                }

                Rectangle {
                    id: knob
                    width: 24
                    height: 24
                    radius: 12
                    anchors.verticalCenter: parent.verticalCenter
                    x: zoomSlider.currentNorm * Math.max(0, zoomSlider.width - width)
                    color: win.textColor
                    border.color: win.amberColor
                    border.width: 2
                }

                DragHandler {
                    target: null
                    xAxis.enabled: true
                    yAxis.enabled: false
                    onCentroidChanged: if (active) zoomSlider.applyX(centroid.position.x)
                    onActiveChanged: if (!active) zoomSlider.liveVal = -1
                }

                TapHandler {
                    onTapped: (pt) => {
                        zoomSlider.applyX(pt.position.x)
                        zoomSlider.liveVal = -1
                    }
                }
            }
        }

        // Center: Big Shutter / Record Button
        Item {
            id: shutterContainer
            anchors.centerIn: parent
            width: 84
            height: 84

            Rectangle {
                anchors.fill: parent
                radius: 42
                color: "transparent"
                border.width: 4
                border.color: camera.mode === "video" ? win.recordRed : win.amberColor
                opacity: camera.ready ? 1.0 : 0.4
            }

            Rectangle {
                id: shutterInner
                anchors.centerIn: parent
                width: camera.recording ? 34 : 64
                height: camera.recording ? 34 : 64
                radius: camera.recording ? 9 : 32
                color: camera.mode === "video" ? win.recordRed : win.textColor
                scale: shutterTap.pressed ? 0.91 : 1.0
                opacity: camera.busy ? 0.6 : 1.0

                Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                Behavior on height { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                Behavior on radius { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                Behavior on scale { NumberAnimation { duration: 80 } }
            }

            TapHandler {
                id: shutterTap
                enabled: camera.ready
                onTapped: win.triggerShutter()
            }
        }

        // Right: Photo / Video Mode Switcher
        Rectangle {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 24
            height: 46
            width: 196
            radius: 23
            color: win.bgColor
            border.color: win.borderColor
            border.width: 1

            Row {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                Rectangle {
                    width: (parent.width - 4) / 2
                    height: parent.height
                    radius: 19
                    color: camera.mode === "photo" ? win.amberColor : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "Фото"
                        color: camera.mode === "photo" ? win.bgColor : win.mutedColor
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    TapHandler {
                        onTapped: camera.setMode("photo")
                    }
                }

                Rectangle {
                    width: (parent.width - 4) / 2
                    height: parent.height
                    radius: 19
                    color: camera.mode === "video" ? win.recordRed : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "Видео"
                        color: camera.mode === "video" ? win.textColor : win.mutedColor
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    TapHandler {
                        onTapped: camera.setMode("video")
                    }
                }
            }
        }
    }
}
