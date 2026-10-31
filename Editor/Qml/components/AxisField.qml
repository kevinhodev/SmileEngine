import QtQuick

// Campo numerico do Inspector: campo rebaixado, valor alinhado a direita, barra de eixo de
// 3 px na borda esquerda.
//
// A barra e o unico lugar do editor em que vermelho/verde/azul aparecem sem sentido
// semantico — ali eles SAO os eixos X/Y/Z, a mesma convencao do gizmo no viewport. Trocar por
// outra coisa quebraria o pareamento entre o campo e a alca que o usuario arrasta. Sem cor de
// eixo (escala), o campo mostra a letra em texto meta no lugar da barra.
Rectangle {
    id: root

    property string axis: ""
    property color axisColor: "transparent"
    property real value: 0
    property int decimals: 3
    property string suffix: ""
    property bool editable: true

    signal committed(real v)

    height: Theme.controlHeight
    radius: Theme.radiusControl
    color: hover.hovered && !input.activeFocus ? Theme.surfaceHover : Theme.surface
    border.color: input.activeFocus ? Theme.accent : "transparent"
    border.width: 1
    opacity: editable ? 1.0 : 0.5
    clip: true

    HoverHandler { id: hover; cursorShape: Qt.IBeamCursor }

    Rectangle {
        visible: root.axisColor.a > 0
        width: 3; height: parent.height
        color: root.axisColor
    }
    Text {
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.axis !== "" && root.axisColor.a === 0
        text: root.axis
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta
        font.weight: Font.DemiBold
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: root.axis !== "" ? 20 : 8
        anchors.rightMargin: 8
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignRight
        enabled: root.editable
        color: Theme.textPrimary
        font.family: Theme.fontNum
        font.pixelSize: Theme.textBody
        selectByMouse: true
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentInk
        text: root.value.toFixed(root.decimals).replace(".", ",") + root.suffix
        validator: RegularExpressionValidator {
            regularExpression: /^[-+]?(?:\d+(?:[.,]\d*)?|[.,]\d+)?$/
        }
        onActiveFocusChanged: if (activeFocus) selectAll()
        onEditingFinished: {
            var v = parseFloat(text.replace(",", "."))
            if (!isNaN(v)) root.committed(v)
            focus = false
        }
        // Valor externo (gizmo arrastando) atualiza o campo quando ele nao esta em edicao.
        Binding {
            target: input; property: "text"
            value: root.value.toFixed(root.decimals).replace(".", ",") + root.suffix
            when: !input.activeFocus
            restoreMode: Binding.RestoreBinding
        }
    }
}
