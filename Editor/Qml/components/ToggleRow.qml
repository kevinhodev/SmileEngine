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
        y: 3
        text: toggleRow.label
        color: toggleRow.interactive ? Theme.textNormal : Theme.textMuted
        font.family: "Segoe UI"
        font.pixelSize: 11
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 46
        y: 4
        text: toggleRow.hint
        color: Theme.textMuted
        font.family: "Segoe UI"
        font.pixelSize: 10
    }
    Toggle {
        anchors.right: parent.right
        y: 1
        checked: toggleRow.checked
        interactive: toggleRow.interactive
        onToggled: toggleRow.toggled()
    }
}
