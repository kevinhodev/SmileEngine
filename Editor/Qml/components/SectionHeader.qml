import QtQuick

// Cabecalho de secao do Inspector (Cartao 18%): faixa de 28 px um degrau acima do painel,
// fechada por fios escuros, titulo em sans semibold e caixa normal.
//
// A faixa VAZA para fora da coluna (`bleed`): o conteudo do Inspector tem margem lateral, o
// cabecalho nao — e a faixa de borda a borda que marca o inicio da secao, nao uma linha solta.
Item {
    id: root

    property string label
    // Metadado da secao (contagem, unidade, escopo) — alinhado a direita, em texto meta.
    property string trailing: ""
    // A primeira secao da coluna nao leva respiro em cima: encosta no que vem antes.
    property bool firstInColumn: false
    property int bleed: 14

    width: parent ? parent.width : 0
    height: (firstInColumn ? 0 : 8) + Theme.sectionHeight + 6

    Rectangle {
        x: -root.bleed
        y: root.firstInColumn ? 0 : 8
        width: root.width + 2 * root.bleed
        height: Theme.sectionHeight
        color: Theme.section

        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: Theme.hairlineStrong }
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 1 } height: 1; color: Theme.sectionTop }
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.hairlineStrong }

        Text {
            id: titleText
            anchors.left: parent.left
            anchors.leftMargin: root.bleed
            anchors.right: trailingText.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSection
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Text {
            id: trailingText
            anchors.right: parent.right
            anchors.rightMargin: root.bleed
            anchors.verticalCenter: parent.verticalCenter
            // Titulo tem prioridade; o metadado cede a largura e elide.
            width: root.trailing === "" ? 0
                 : Math.max(0, Math.min(implicitWidth, parent.width - 2 * root.bleed
                                        - titleText.implicitWidth - 12))
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideLeft
            text: root.trailing
            color: Theme.textMuted
            font.family: Theme.fontNum
            font.pixelSize: Theme.textMeta
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }
}
