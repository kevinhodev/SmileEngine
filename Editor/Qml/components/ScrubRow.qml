pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property string label
    property real from: 0
    property real to: 1
    property real stepSize: 0.01
    property real boundValue: 0
    property real defaultValue: Number.NaN
    property int decimals: 2
    property real displayScale: 1
    property string prefix: ""
    property string suffix: ""
    property bool interactive: true
    property real fieldRatio: 0.58

    signal moved(real value)

    width: parent.width
    height: 28
    opacity: interactive ? 1 : 0.45

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: field.left
        anchors.rightMargin: 10
        text: root.label
        color: Theme.textNormal
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Rectangle {
        anchors.right: field.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        visible: field.modified
        width: 5
        height: 5
        radius: 2.5
        color: Theme.amber
    }

    ScrubField {
        id: field
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width * root.fieldRatio
        from: root.from
        to: root.to
        stepSize: root.stepSize
        boundValue: root.boundValue
        defaultValue: root.defaultValue
        decimals: root.decimals
        displayScale: root.displayScale
        prefix: root.prefix
        suffix: root.suffix
        interactive: root.interactive
        onMoved: value => root.moved(value)
    }
}
