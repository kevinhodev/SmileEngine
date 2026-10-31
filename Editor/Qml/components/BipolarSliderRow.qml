pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string label
    property string valueText
    property string minimumLabel
    property string maximumLabel
    property string centerLabel
    property real from: -1
    property real to: 1
    property real centerValue: 0
    property real stepSize: 0
    property real boundValue: 0
    property bool interactive: true

    signal moved(real value)

    readonly property real centerPosition: to === from ? 0.5
                                                        : (centerValue - from) / (to - from)

    width: parent.width
    height: 52
    opacity: interactive ? 1 : 0.45

    Text {
        anchors.left: parent.left
        y: 0
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textBody
    }

    Text {
        anchors.right: parent.right
        y: 0
        text: root.valueText
        color: Theme.textPrimary
        font.family: Theme.fontNum
        font.pixelSize: Theme.textBody
    }

    Text {
        anchors.left: parent.left
        y: 17
        text: root.minimumLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta - 1
    }

    Text {
        anchors.right: parent.right
        y: 17
        text: root.maximumLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta - 1
    }

    Slider {
        id: slider
        anchors.left: parent.left
        anchors.right: parent.right
        y: 25
        height: 18
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

        background: Item {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - 1
            width: slider.availableWidth
            height: 2

            Rectangle {
                anchors.fill: parent
                radius: 1
                color: Theme.surface
            }

            Rectangle {
                x: Math.min(root.centerPosition, slider.visualPosition) * parent.width
                width: Math.abs(slider.visualPosition - root.centerPosition) * parent.width
                height: parent.height
                radius: 1
                color: slider.pressed ? Theme.accent : Theme.textMuted
            }

            Rectangle {
                x: root.centerPosition * parent.width
                y: -4
                width: 1
                height: 10
                color: Theme.textMuted
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 10
            height: 10
            radius: Theme.radiusChip
            color: slider.pressed ? Theme.accent : Theme.knob
            border.color: Theme.hairlineStrong
            border.width: 1
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 43
        text: root.centerLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta - 1
    }
}
