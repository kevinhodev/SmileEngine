import QtQuick

// Chip de acao/filtro (largura pelo texto por default; height/width overridaveis).
Rectangle {
    id: chip
    property string label
    property bool active: false
    signal tapped()
    height: 24
    width: chipText.implicitWidth + 18
    radius: 6
    color: active ? Theme.blueBg : (chipHover.hovered ? Theme.controlHover : Theme.controlBg)
    border.color: active ? Theme.blueBorder : Theme.controlBorder
    border.width: 1
    Text {
        id: chipText
        anchors.centerIn: parent
        text: chip.label
        color: chip.active ? Theme.blue : Theme.textNormal
        font.family: "Segoe UI"
        font.pixelSize: 10
    }
    HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: chip.tapped() }
}
