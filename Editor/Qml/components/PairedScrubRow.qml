pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property string label
    property string firstCaption
    property string secondCaption
    property real firstValue: 0
    property real secondValue: 0
    property real firstDefaultValue: Number.NaN
    property real secondDefaultValue: Number.NaN
    property real firstFrom: 0
    property real firstTo: 1
    property real secondFrom: 0
    property real secondTo: 1
    property real firstStepSize: 0.01
    property real secondStepSize: 0.01
    property int firstDecimals: 2
    property int secondDecimals: 2
    property string firstSuffix: ""
    property string secondSuffix: ""
    property bool interactive: true

    signal firstMoved(real value)
    signal secondMoved(real value)

    width: parent.width
    height: 28
    opacity: interactive ? 1 : 0.45

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(66, parent.width * 0.25)
        text: root.label
        color: Theme.textNormal
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width * 0.68
        spacing: 6

        ScrubField {
            width: (parent.width - parent.spacing) / 2
            caption: root.firstCaption
            from: root.firstFrom
            to: root.firstTo
            stepSize: root.firstStepSize
            boundValue: root.firstValue
            defaultValue: root.firstDefaultValue
            decimals: root.firstDecimals
            suffix: root.firstSuffix
            interactive: root.interactive
            onMoved: value => root.firstMoved(value)
        }

        ScrubField {
            width: (parent.width - parent.spacing) / 2
            caption: root.secondCaption
            from: root.secondFrom
            to: root.secondTo
            stepSize: root.secondStepSize
            boundValue: root.secondValue
            defaultValue: root.secondDefaultValue
            decimals: root.secondDecimals
            suffix: root.secondSuffix
            interactive: root.interactive
            onMoved: value => root.secondMoved(value)
        }
    }
}
