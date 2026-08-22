import QtQuick

// Campo numerico do Inspector: valor em mono, alinhado a direita, com faixa de eixo opcional.
//
// A faixa de 2 px e o unico lugar do editor em que vermelho/verde/azul aparecem sem sentido
// semantico — ali eles SAO os eixos X/Y/Z, a mesma convencao do gizmo no viewport. Trocar por
// outra coisa quebraria o pareamento entre o campo e a alca que o usuario arrasta.
Rectangle {
    id: root

    property string axis: ""
    property color axisColor: "transparent"
    property real value: 0
    property int decimals: 3
    property string suffix: ""
    property bool editable: true

    signal committed(real v)

    height: 26
    radius: Theme.radiusControl
    color: Theme.surface
    border.color: input.activeFocus ? Theme.accentBorder : Theme.hairline
    border.width: 1
    opacity: editable ? 1.0 : 0.5

    Rectangle {
        visible: root.axisColor.a > 0
        x: 1; y: 1
        width: 2; height: parent.height - 2
        color: root.axisColor
    }
    Text {
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        visible: root.axis !== ""
        text: root.axis
        color: Theme.textMuted
        font.family: Theme.fontMono
        font.pixelSize: 10
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: root.axis !== "" ? 24 : 10
        anchors.rightMargin: 9
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignRight
        enabled: root.editable
        color: Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: 11
        selectByMouse: true
        selectionColor: Theme.accentBorder
        text: root.value.toFixed(root.decimals) + root.suffix
        validator: DoubleValidator { locale: "C"; notation: DoubleValidator.StandardNotation }
        onActiveFocusChanged: if (activeFocus) selectAll()
        onEditingFinished: {
            // Aceita virgula: o teclado numerico brasileiro so tem ela, e o DoubleValidator
            // esta em locale C justamente para nao brigar com o ponto digitado.
            var v = parseFloat(text.replace(",", "."))
            if (!isNaN(v)) root.committed(v)
            focus = false
        }
        // Valor externo (gizmo arrastando) atualiza o campo quando ele nao esta em edicao.
        Binding {
            target: input; property: "text"
            value: root.value.toFixed(root.decimals) + root.suffix
            when: !input.activeFocus
            restoreMode: Binding.RestoreBinding
        }
    }
}
