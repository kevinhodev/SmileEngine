import QtQuick
import ".." // LucideIcon.qml mora na raiz de Qml/

// Trio X/Y/Z do Inspector numa linha so: rotulo na coluna de rotulos, tres AxisField de
// largura igual na coluna de valores. `showLink` acende o elo da escala uniforme, encostado
// no fim da coluna de rotulo (accent quando ligado — e estado ativo).
Item {
    id: root

    property string label
    property bool showLink: false
    property bool linked: false
    property int decimals: 3
    property real xValue: 0
    property real yValue: 0
    property real zValue: 0
    // Escala nao tem convencao de cor por eixo: sem tinta, os campos mostram a letra.
    property bool tinted: true

    signal linkToggled()
    signal xCommitted(real v)
    signal yCommitted(real v)
    signal zCommitted(real v)

    width: parent ? parent.width : 0
    height: Theme.controlHeight + 6

    Text {
        id: rowLabel
        anchors.left: parent.left
        anchors.right: linkButton.visible ? linkButton.left : fields.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textBody
        elide: Text.ElideRight
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Item {
        id: linkButton
        visible: root.showLink
        anchors.right: fields.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18
        LucideIcon {
            anchors.centerIn: parent
            name: "link"; size: 13
            color: root.linked ? Theme.accent
                 : linkHover.hovered ? Theme.textSecondary
                 : Theme.textMuted
        }
        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.linkToggled() }
        // ToolTip aqui e o compartilhado do editor (components/ToolTip.qml), nao o attached
        // do Controls: dentro desta pasta o nome local vence.
        ToolTip {
            active: linkHover.hovered
            text: root.linked ? "Escala uniforme (ligada)" : "Escala por eixo"
        }
    }

    Row {
        id: fields
        x: Math.round(parent.width * Theme.labelRatio)
        width: parent.width - x
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        readonly property real fieldWidth: (width - 2 * spacing) / 3

        AxisField {
            width: fields.fieldWidth
            axis: "X"; axisColor: root.tinted ? Theme.axisX : "transparent"
            decimals: root.decimals
            value: root.xValue
            onCommitted: (v) => root.xCommitted(v)
        }
        AxisField {
            width: fields.fieldWidth
            axis: "Y"; axisColor: root.tinted ? Theme.axisY : "transparent"
            decimals: root.decimals
            value: root.yValue
            onCommitted: (v) => root.yCommitted(v)
        }
        AxisField {
            width: fields.fieldWidth
            axis: "Z"; axisColor: root.tinted ? Theme.axisZ : "transparent"
            decimals: root.decimals
            value: root.zValue
            onCommitted: (v) => root.zCommitted(v)
        }
    }
}
