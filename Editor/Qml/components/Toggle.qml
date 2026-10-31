import QtQuick
import ".." // LucideIcon.qml mora na raiz de Qml/

// Liga/desliga do editor. No Cartao 18% e um CHECKBOX de 14 px, nao um switch: ocupa um
// quadrado na coluna de valores em vez de uma pilula, e alinha com o inicio dos campos.
// Ligado = preenchido em cinza claro com marca escura; o accent fica para selecao/foco.
Item {
    id: toggle
    property bool checked: false
    property bool interactive: true
    signal toggled()
    implicitWidth: 14
    implicitHeight: 14
    opacity: interactive ? 1.0 : 0.45

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: toggle.checked ? Theme.knob : Theme.surface
        border.width: toggle.checked ? 0 : 1
        border.color: hover.hovered && toggle.interactive ? Theme.textMuted : Theme.textDisabled
    }
    LucideIcon {
        visible: toggle.checked
        anchors.centerIn: parent
        name: "check"
        size: 12
        color: Theme.accentInk
    }
    HoverHandler { id: hover; cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: toggle.interactive; onTapped: toggle.toggled() }
}
