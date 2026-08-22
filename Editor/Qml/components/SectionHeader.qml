import QtQuick

// Header de secao do Inspector (Signal Graphite): 10 px, caixa alta, tracking de 1.5 px, em
// mono, precedido pela hairline que separa da secao anterior.
//
// Substitui o card por hairline + rotulo: num dock de 320 px a moldura do card comia largura
// util em troca de uma separacao que uma linha de 1 px ja da.
Item {
    id: root

    property string label
    // Metadado da secao (contagem, unidade, escopo) — alinhado a direita, tambem em mono.
    property string trailing: ""
    // A primeira secao da coluna nao leva hairline em cima: nao ha nada para separar dela.
    property bool firstInColumn: false

    width: parent ? parent.width : 0
    height: firstInColumn ? 30 : 38

    Rectangle {
        visible: !root.firstInColumn
        anchors { left: parent.left; right: parent.right; top: parent.top }
        anchors.topMargin: 8
        height: 1
        color: Theme.hairline
    }
    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 4
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontMono
        font.pixelSize: 10
        font.letterSpacing: 1.5
        font.weight: Font.DemiBold
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 4
        text: root.trailing
        color: Theme.textMuted
        font.family: Theme.fontMono
        font.pixelSize: 10
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
}
