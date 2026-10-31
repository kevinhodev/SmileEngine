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
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textBody
    }

    Rectangle {
        anchors.right: parent.right
        y: 0
        width: Math.max(48, badgeText.implicitWidth + 16)
        height: 20
        radius: Theme.radiusChip
        color: Theme.surface

        Text {
            id: badgeText
            anchors.centerIn: parent
            text: root.valueText
            color: Theme.textPrimary
            font.family: Theme.fontNum
            font.pixelSize: Theme.textBody
            font.weight: Font.Medium
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
            color: Theme.surface

            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: 1
                color: slider.pressed ? Theme.accent : Theme.textMuted
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 10
            height: 10
            radius: Theme.radiusControl
            color: slider.pressed ? Theme.accent : Theme.knob
            border.color: Theme.hairlineStrong
            border.width: 1
        }
    }
}
