pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: root

    required property var windowBridge
    property string title
    property string subtitle
    property string iconName
    property url iconSource
    property color iconColor: Theme.textSecondary
    property color subtitleColor: Theme.textMuted
    property bool showMaximize: true
    property Component actions

    implicitHeight: 36
    color: Theme.chrome

    Image {
        id: sourceIcon

        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 20
        height: 20
        visible: root.iconSource.toString() !== ""
        source: root.iconSource
        sourceSize: Qt.size(32, 32)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    ToolbarIcon {
        id: vectorIcon

        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        height: 16
        visible: root.iconSource.toString() === "" && root.iconName !== ""
        name: root.iconName
        color: root.iconColor
    }

    Text {
        id: titleLabel

        anchors.left: parent.left
        anchors.leftMargin: root.iconSource.toString() !== "" || root.iconName !== "" ? 42 : 16
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSection
        font.weight: Font.DemiBold
    }

    Text {
        anchors.left: titleLabel.right
        anchors.leftMargin: root.subtitle !== "" ? 12 : 0
        anchors.right: actionsLoader.visible ? actionsLoader.left : windowButtons.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.subtitle
        color: root.subtitleColor
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta
        elide: Text.ElideRight
    }

    MouseArea {
        anchors.left: parent.left
        anchors.right: windowButtons.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        property bool moving: false

        onPressed: moving = false
        onPositionChanged: {
            if (pressed && !moving) {
                moving = true
                root.windowBridge.startSystemMove()
            }
        }
        onReleased: moving = false
        onDoubleClicked: if (root.showMaximize) root.windowBridge.toggleMaximize()
    }

    Loader {
        id: actionsLoader

        anchors.right: windowButtons.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        sourceComponent: root.actions
        visible: status === Loader.Ready
        z: 1
    }

    Row {
        id: windowButtons

        anchors.right: parent.right
        anchors.top: parent.top
        z: 1

        WindowButton { glyph: "−"; onTapped: root.windowBridge.minimize() }
        WindowButton {
            visible: root.showMaximize
            glyph: root.windowBridge.maximized ? "❐" : "□"
            onTapped: root.windowBridge.toggleMaximize()
        }
        WindowButton { glyph: "×"; danger: true; onTapped: root.windowBridge.close() }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.hairlineStrong
    }
}
