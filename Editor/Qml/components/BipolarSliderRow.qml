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
        color: Theme.textNormal
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Text {
        anchors.right: parent.right
        y: 0
        text: root.valueText
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 9
    }

    Text {
        anchors.left: parent.left
        y: 17
        text: root.minimumLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 8
    }

    Text {
        anchors.right: parent.right
        y: 17
        text: root.maximumLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 8
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
                color: Theme.controlBg
            }

            Rectangle {
                x: Math.min(root.centerPosition, slider.visualPosition) * parent.width
                width: Math.abs(slider.visualPosition - root.centerPosition) * parent.width
                height: parent.height
                radius: 1
                color: Theme.green
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
            radius: 5
            color: Theme.knob
            border.color: Theme.green
            border.width: 1.3
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 43
        text: root.centerLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 7
    }
}
