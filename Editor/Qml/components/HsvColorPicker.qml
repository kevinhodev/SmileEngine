pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property color value: "#ffffff"
    property var presets: []
    property int fieldHeight: 110
    property bool showPreview: presets.length > 0
    signal pushed(color value)

    property real hue: 0
    property real saturation: 0
    property real brightness: 1
    property bool syncing: false
    readonly property bool dragging: svDrag.pressed || hueDrag.pressed

    implicitHeight: fieldHeight + (showPreview || presets.length > 0 ? 30 : 0)

    function syncFromValue() {
        syncing = true
        hue = value.hsvHue >= 0 ? value.hsvHue : hue
        saturation = value.hsvSaturation
        brightness = value.hsvValue
        syncing = false
    }

    function pushValue() {
        if (!syncing)
            pushed(Qt.hsva(hue, saturation, brightness, 1.0))
    }

    onValueChanged: if (!dragging) syncFromValue()
    Component.onCompleted: syncFromValue()

    Rectangle {
        id: svBox

        anchors.left: parent.left
        anchors.right: hueBar.left
        anchors.rightMargin: 10
        height: root.fieldHeight
        radius: Theme.radiusControl
        clip: true
        border.color: Theme.hairline
        border.width: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: Qt.hsva(root.hue, 1, 1, 1) }
        }

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "#000000" }
            }
        }

        Rectangle {
            x: root.saturation * (svBox.width - 1) - width / 2
            y: (1 - root.brightness) * (svBox.height - 1) - height / 2
            width: 13
            height: 13
            radius: width / 2
            color: "transparent"
            border.color: root.brightness > 0.55 && root.saturation < 0.5
                ? "#00000090" : "#ffffffd0"
            border.width: 2
        }

        MouseArea {
            id: svDrag

            anchors.fill: parent
            cursorShape: Qt.CrossCursor

            function applyPosition(x, y) {
                root.saturation = Math.max(0, Math.min(1, x / (width - 1)))
                root.brightness = 1 - Math.max(0, Math.min(1, y / (height - 1)))
                root.pushValue()
            }

            onPressed: event => applyPosition(event.x, event.y)
            onPositionChanged: event => {
                if (pressed) applyPosition(event.x, event.y)
            }
        }
    }

    Rectangle {
        id: hueBar

        anchors.right: parent.right
        width: 22
        height: root.fieldHeight
        radius: Theme.radiusControl
        clip: true
        border.color: Theme.hairline
        border.width: 1
        gradient: Gradient {
            GradientStop { position: 0.000; color: "#ff0000" }
            GradientStop { position: 0.167; color: "#ffff00" }
            GradientStop { position: 0.333; color: "#00ff00" }
            GradientStop { position: 0.500; color: "#00ffff" }
            GradientStop { position: 0.667; color: "#0000ff" }
            GradientStop { position: 0.833; color: "#ff00ff" }
            GradientStop { position: 1.000; color: "#ff0000" }
        }

        Rectangle {
            x: 1
            y: root.hue * (hueBar.height - 5) + 1
            width: parent.width - 2
            height: 4
            radius: Theme.radiusChip
            color: "transparent"
            border.color: "#ffffffd0"
            border.width: 1.5
        }

        MouseArea {
            id: hueDrag

            anchors.fill: parent

            function applyPosition(y) {
                root.hue = Math.max(0, Math.min(1, y / (height - 1)))
                root.pushValue()
            }

            onPressed: event => applyPosition(event.y)
            onPositionChanged: event => {
                if (pressed) applyPosition(event.y)
            }
        }
    }

    Row {
        anchors.left: parent.left
        anchors.top: svBox.bottom
        anchors.topMargin: 10
        spacing: 6
        visible: root.showPreview || root.presets.length > 0

        Rectangle {
            visible: root.showPreview
            width: 40
            height: 20
            radius: Theme.radiusChip
            color: Qt.hsva(root.hue, root.saturation, root.brightness, 1)
            border.color: Theme.hairlineStrong
            border.width: 1
        }

        Repeater {
            model: root.presets

            delegate: Rectangle {
                id: presetSwatch
                required property string modelData

                width: 20
                height: 20
                radius: Theme.radiusChip
                color: modelData
                border.color: Qt.darker(color, 1.5)
                border.width: 1

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.pushed(presetSwatch.modelData) }
            }
        }
    }
}
