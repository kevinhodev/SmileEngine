import QtQuick

Item {
    id: root

    property string label
    property color value: "#ffffff"
    property bool open: false
    property bool interactive: true
    signal pushed(color value)

    width: parent.width
    height: open ? 176 : 24
    clip: true
    opacity: interactive ? 1.0 : 0.48

    Behavior on height {
        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
    }

    Text {
        y: 4
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }

    Text {
        anchors.right: swatch.left
        anchors.rightMargin: 10
        y: 5
        text: String(root.value).toUpperCase()
        color: Theme.textMuted
        font.family: Theme.fontMono
        font.pixelSize: 10
    }

    Rectangle {
        id: swatch

        anchors.right: parent.right
        y: 2
        width: 46
        height: 18
        radius: Theme.radiusControl
        color: root.value
        border.color: swatchHover.hovered ? Theme.accent : Theme.hairline
        border.width: 1

        HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            enabled: root.interactive
            onTapped: root.open = !root.open
        }
    }

    HsvColorPicker {
        anchors.left: parent.left
        anchors.right: parent.right
        y: 30
        visible: root.open
        value: root.value
        onPushed: value => root.pushed(value)
    }

    Text {
        y: 148
        visible: root.open
        text: "arraste no quadrado (S/V) e na barra (matiz)"
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }
}
