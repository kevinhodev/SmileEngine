import QtQuick

// Linha rotulo -> valor das secoes de estatistica. O valor e DADO, entao sai em mono; o
// rotulo e prosa, entao sai em sans.
Item {
    id: root

    property string label
    property string value

    width: parent ? parent.width : 0
    height: 26

    Text {
        id: rowLabel
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 12
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Text {
        anchors.left: rowLabel.right
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        horizontalAlignment: Text.AlignRight
        text: root.value
        color: Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: 11
        elide: Text.ElideMiddle
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
}
