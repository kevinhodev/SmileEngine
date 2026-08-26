import QtQuick

Item {
    id: root

    property string title
    property string detail
    property Component actions

    height: 40

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        Text {
            text: root.title
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: 10
            font.letterSpacing: 1.5
            font.weight: Font.DemiBold
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Text {
            visible: root.detail !== ""
            text: root.detail
            color: Theme.textMuted
            font.family: Theme.fontMono
            font.pixelSize: 10
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }

    Loader {
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        sourceComponent: root.actions
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.hairline
    }
}
