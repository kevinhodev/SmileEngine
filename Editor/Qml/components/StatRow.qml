import QtQuick

// Linha rotulo -> valor das secoes de estatistica. Rotulo no tier de rotulo (textSecondary),
// valor em texto principal com os algarismos tabulares da fonte da UI.
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
        font.pixelSize: Theme.textBody
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
        font.family: Theme.fontNum
        font.pixelSize: Theme.textBody
        elide: Text.ElideMiddle
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
}
