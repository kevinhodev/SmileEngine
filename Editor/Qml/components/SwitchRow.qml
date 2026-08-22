import QtQuick

// Linha rotulo -> switch das secoes do Inspector. Difere do ToggleRow (leva anterior, com
// card) por nao ter moldura: a separacao vem do SectionHeader.
Item {
    id: root

    property string label
    // Nota curta ao lado do rotulo, em mono: serve para o custo/efeito do estado, nao para
    // repetir o que o rotulo ja diz.
    property string hint: ""
    property bool checked: false
    property bool interactive: true

    signal toggled()

    width: parent ? parent.width : 0
    height: 28

    Text {
        id: rowLabel
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 13
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Text {
        anchors.left: rowLabel.right
        anchors.leftMargin: 8
        anchors.right: rowToggle.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.hint !== ""
        text: root.hint
        color: Theme.textMuted
        font.family: Theme.fontMono
        font.pixelSize: 9
        elide: Text.ElideRight
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Toggle {
        id: rowToggle
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        checked: root.checked
        interactive: root.interactive
        onToggled: root.toggled()
    }
}
