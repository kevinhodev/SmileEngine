import QtQuick
import ".." // LucideIcon.qml mora na raiz de Qml/

// Card com header (icone + titulo + slot a direita), divisor e coluna de conteudo.
// contentSpacing: 6 nos paines densos (Materiais), 10 nos mais arejados (TOD/Outliner).
Rectangle {
    id: card
    default property alias content: inner.data
    property string title
    property string iconName
    property alias headerItem: headerSlot.data
    property alias contentSpacing: inner.spacing
    property bool dimmed: false
    width: parent.width
    radius: 8
    color: Theme.cardBg
    border.color: Theme.borderColor
    border.width: 1
    implicitHeight: 40 + inner.implicitHeight + 14

    LucideIcon {
        visible: card.iconName !== ""
        x: 14; y: 13
        name: card.iconName
        size: 14
        color: Theme.textSecondary
    }
    Text {
        x: card.iconName !== "" ? 36 : 16; y: 12
        text: card.title
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Medium
    }
    Item {
        id: headerSlot
        anchors.right: parent.right
        anchors.rightMargin: 14
        y: 10
        width: childrenRect.width
        height: 20
    }
    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right
        y: 36; height: 1
        color: Theme.divider
    }
    Column {
        id: inner
        x: 14; y: 44
        width: parent.width - 28
        spacing: 6
        opacity: card.dimmed ? 0.45 : 1.0
    }
}
