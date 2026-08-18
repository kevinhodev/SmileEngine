import QtQuick

// Linha de card: label a esquerda, hint discreto e Toggle a direita.
Item {
    id: toggleRow
    property string label
    property string hint
    property bool checked: false
    property bool interactive: true
    signal toggled()
    width: parent.width
    height: 26

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: hintText.left
        anchors.rightMargin: 8
        text: toggleRow.label
        color: toggleRow.interactive ? Theme.textNormal : Theme.textMuted
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Text {
        id: hintText
        anchors.right: parent.right
        anchors.rightMargin: toggleControl.width + 10
        anchors.verticalCenter: parent.verticalCenter
        text: toggleRow.hint
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 9
    }

    Toggle {
        id: toggleControl
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        checked: toggleRow.checked
        interactive: toggleRow.interactive
        onToggled: toggleRow.toggled()
    }
}
