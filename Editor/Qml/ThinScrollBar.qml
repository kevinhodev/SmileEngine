import QtQuick
import QtQuick.Controls
import "components" as C

// Scrollbar fina e arredondada do editor, estados conforme a folha de referencia.
// Reaproveitavel em qualquer painel QML: `ScrollBar.vertical: ThinScrollBar { revealed: ... }`.
// "Aparece no hover": fica transparente em repouso e revela ao interagir (active/hover/press)
// ou quando `revealed` (ligado a um HoverHandler da area de conteudo) e verdadeiro.
ScrollBar {
    id: control
    policy: ScrollBar.AsNeeded
    minimumSize: 0.08 // piso do thumb (aprox. tamanho minimo)

    // Revelar por hover da area de conteudo (o pai liga isto num HoverHandler).
    property bool revealed: false

    // Espessura fina e fixa (sem crescer no hover; so muda a cor).
    property int thickness: 8

    readonly property bool shown: active || hovered || pressed || revealed

    contentItem: Rectangle {
        implicitWidth: control.thickness
        implicitHeight: control.thickness
        radius: width / 2 // pill (~4px na espessura padrao)
        color: !control.enabled ? C.Theme.textDisabled
             :  control.pressed  ? C.Theme.textNormal
             :  control.hovered  ? C.Theme.textSecondary
             :                     C.Theme.textMuted
        opacity: control.shown ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
        Behavior on color   { ColorAnimation  { duration: 120 } }
    }

    background: Rectangle {
        color: C.Theme.surface
        radius: width / 2
        opacity: control.shown ? 0.9 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
    }
}
