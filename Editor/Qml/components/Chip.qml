import QtQuick

// Chip de acao/filtro (largura pelo texto por default; height/width overridaveis). Ligado =
// botao afundado em cinza claro, como o segmento ativo; o accent fica para selecao/foco.
Rectangle {
    id: chip
    property string label
    property bool active: false
    signal tapped()
    height: 24
    width: chipText.implicitWidth + 18
    radius: Theme.radiusControl
    color: active ? Theme.raisedOn : (chipHover.hovered ? Theme.raisedHover : Theme.surfaceRaised)
    Text {
        id: chipText
        anchors.centerIn: parent
        text: chip.label
        color: chip.active ? Theme.textPrimary : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textBody
        font.weight: Font.Medium
    }
    HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: chip.tapped() }
}
