import QtQuick

// Linha rotulo -> valor das secoes de estatistica. O valor e DADO, entao sai em mono; o
// rotulo e prosa, entao sai em sans.
Item {
    id: root

    property string label
    property string value

    width: parent ? parent.width : 0
    height: 26

    // Mesmo tier do rotulo do VectorRow ("Posicao", "Rotacao"): rotulo de dado e LEGENDA do
    // numero, nao titulo. Em textSecondary ele empatava com o SectionHeader logo acima e a
    // secao perdia o inicio — o cabecalho so se le como cabecalho se a linha abaixo recuar.
    Text {
        id: rowLabel
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 11
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
