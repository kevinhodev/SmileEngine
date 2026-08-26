import QtQuick
import ".." // LucideIcon.qml mora na raiz de Qml/

// Trio X/Y/Z do Inspector: rotulo em cima, tres AxisField de largura igual embaixo.
// `showLink` acende o elo da escala uniforme (accent quando ligado — e estado ativo).
Item {
    id: root

    property string label
    property bool showLink: false
    property bool linked: false
    property int decimals: 3
    property real xValue: 0
    property real yValue: 0
    property real zValue: 0
    // Escala nao tem convencao de cor por eixo: sem tinta, os tres campos ficam neutros.
    property bool tinted: true

    signal linkToggled()
    signal xCommitted(real v)
    signal yCommitted(real v)
    signal zCommitted(real v)

    width: parent ? parent.width : 0
    height: 48

    Text {
        id: rowLabel
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 11
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }
    Item {
        visible: root.showLink
        anchors.left: rowLabel.right
        anchors.leftMargin: 8
        anchors.verticalCenter: rowLabel.verticalCenter
        width: 18; height: 18
        LucideIcon {
            anchors.centerIn: parent
            name: "link"; size: 12
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
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 8
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
