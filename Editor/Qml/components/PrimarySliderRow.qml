pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string label
    property string valueText
    property real from: 0
    property real to: 1
    property real stepSize: 0
    property real boundValue: 0
    property bool interactive: true

    signal moved(real value)

    width: parent.width
    height: 43
    opacity: interactive ? 1 : 0.45

    Text {
        anchors.left: parent.left
        y: 3
        text: root.label
        color: Theme.textNormal
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Rectangle {
        anchors.right: parent.right
        y: 0
        width: Math.max(48, badgeText.implicitWidth + 16)
        height: 20
        radius: Theme.radiusChip
        color: Theme.accentBg
        border.color: Theme.accentBorder
        border.width: 1

        Text {
            id: badgeText
            anchors.centerIn: parent
            text: root.valueText
            color: Theme.accent
            font.family: Theme.fontMono
            font.pixelSize: 8
            font.weight: Font.DemiBold
        }
    }

    Slider {
        id: slider
        anchors.left: parent.left
        anchors.right: parent.right
        y: 20
        height: 19
        enabled: root.interactive
        from: root.from
        to: root.to
        stepSize: root.stepSize
        onMoved: root.moved(value)

        Binding {
            target: slider
            property: "value"
            value: root.boundValue
            when: !slider.pressed
            restoreMode: Binding.RestoreBinding
        }

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - 1
            width: slider.availableWidth
            height: 2
            radius: 1
            color: Theme.controlBg

            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: 1
                color: Theme.accent
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 10
            height: 10
            radius: Theme.radiusControl
            color: Theme.knob
            border.color: Theme.accent
            border.width: 1.3
        }
    }
}
