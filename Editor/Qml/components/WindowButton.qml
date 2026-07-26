import QtQuick

// Botao de janela frameless (minimizar/maximizar/fechar) — chrome das janelas QML.
Rectangle {
    id: windowButton
    property string glyph
    property bool danger: false
    signal tapped()
    width: 46
    height: 40
    color: winHover.hovered ? (danger ? "#c42b1c" : Theme.controlBg) : "transparent"
    Text {
        anchors.centerIn: parent
        text: windowButton.glyph
        color: winHover.hovered && windowButton.danger ? "#ffffff" : Theme.textSecondary
        font.family: "Segoe UI Symbol"
        font.pixelSize: windowButton.glyph === "□" ? 14 : 16
    }
    HoverHandler { id: winHover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: windowButton.tapped() }
}
