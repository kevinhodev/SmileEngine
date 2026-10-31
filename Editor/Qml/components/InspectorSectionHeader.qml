pragma ComponentBehavior: Bound

import QtQuick
import ".." as UI

// Cabecalho de secao EXPANSIVEL: a mesma faixa do SectionHeader, com chevron. Fechada, a
// secao mostra o resumo a direita ("7.938 tri · 0,4 MB") — da para ler o estado sem abrir.
Item {
    id: root

    property string label
    property string summary
    property bool expanded: false
    property int bleed: 14

    signal toggled()

    width: parent.width
    height: 8 + Theme.sectionHeight + (expanded ? 6 : 0)

    Rectangle {
        x: -root.bleed
        y: 8
        width: root.width + 2 * root.bleed
        height: Theme.sectionHeight
        color: hover.hovered ? Theme.rowHover : Theme.section

        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: Theme.hairlineStrong }
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 1 } height: 1; color: Theme.sectionTop }
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.hairlineStrong }

        UI.LucideIcon {
            id: chevron
            x: root.bleed - 4
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-right"
            size: 12
            rotation: root.expanded ? 90 : 0
            color: Theme.textSecondary
            Behavior on rotation { NumberAnimation { duration: 100 } }
        }

        Text {
            id: titleText
            anchors.left: chevron.right
            anchors.leftMargin: 6
            anchors.right: summaryText.left
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
            id: summaryText
            anchors.right: parent.right
            anchors.rightMargin: root.bleed
            anchors.verticalCenter: parent.verticalCenter
            // O titulo tem prioridade: o resumo so ocupa o que sobra depois dele (e some quando
            // nao sobra nada). Com largura fixa, titulos como "Superficies molhadas" eram
            // cortados ao lado de um resumo vazio.
            width: root.summary === "" ? 0
                 : Math.max(0, Math.min(implicitWidth, parent.width - root.bleed - chevron.x
                                        - chevron.width - 6 - titleText.implicitWidth - 12))
            horizontalAlignment: Text.AlignRight
            text: root.summary
            color: Theme.textMuted
            font.family: Theme.fontNum
            font.pixelSize: Theme.textMeta
            elide: Text.ElideLeft
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.toggled() }
    }
}
