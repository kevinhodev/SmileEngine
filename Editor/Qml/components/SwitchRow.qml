import QtQuick

// Linha rotulo -> checkbox das secoes do Inspector. O checkbox fica no INICIO da coluna de
// valores (Theme.labelRatio), alinhado com os campos das linhas vizinhas; a dica vem logo depois.
Item {
    id: root

    property string label
    // Nota curta ao lado do checkbox: serve para o custo/efeito do estado, nao para repetir o
    // que o rotulo ja diz.
    property string hint: ""
    property bool checked: false
    property bool interactive: true

    signal toggled()

    width: parent ? parent.width : 0
    height: 28

    Text {
        anchors.left: parent.left
        anchors.right: rowToggle.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: root.interactive ? Theme.textSecondary : Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textBody
        elide: Text.ElideRight
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Toggle {
        id: rowToggle
        x: Math.round(parent.width * Theme.labelRatio)
        anchors.verticalCenter: parent.verticalCenter
        checked: root.checked
        interactive: root.interactive
        onToggled: root.toggled()
    }
    Text {
        anchors.left: rowToggle.right
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: root.hint !== ""
        text: root.hint
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta
        elide: Text.ElideRight
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
}
