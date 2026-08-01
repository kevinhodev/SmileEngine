import QtQuick

// Botão denso da toolbar: a largura abraça o conteúdo e todos os controles seguem
// exatamente a mesma métrica horizontal.
Rectangle {
    id: control

    property string label: ""
    property string iconName: ""
    property bool active: false
    property bool dropDown: false
    property bool enabledVisual: true
    property string tip: ""
    property string tipShortcut: ""

    signal tapped()

    implicitWidth: contents.implicitWidth + 14
    implicitHeight: 22
    radius: 5
    opacity: enabledVisual ? 1.0 : 0.42
    color: active ? Theme.greenBg
                  : (hover.hovered && enabledVisual ? Theme.controlHover : "#1a1c17")
    border.color: active ? Theme.greenBorder
                         : (hover.hovered && enabledVisual ? "#393b32" : "#2d2f28")
    border.width: 1

    Row {
        id: contents
        anchors.centerIn: parent
        spacing: 5

        ToolbarIcon {
            visible: control.iconName !== ""
            width: 14; height: 14
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            color: control.active ? Theme.green : "#a7a397"
        }

        Text {
            visible: control.label !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.label
            color: control.active ? Theme.greenText : Theme.textNormal
            font.family: Theme.fontFamily
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }

        Canvas {
            visible: control.dropDown
            width: 10; height: 7
            anchors.verticalCenter: parent.verticalCenter
            property color strokeColor: control.active ? Theme.green : "#858176"
            onStrokeColorChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = strokeColor
                ctx.lineWidth = 1.35
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.beginPath()
                ctx.moveTo(1.5, 1.5)
                ctx.lineTo(5, 5)
                ctx.lineTo(8.5, 1.5)
                ctx.stroke()
            }
        }
    }

    HoverHandler {
        id: hover
        cursorShape: control.enabledVisual ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler { enabled: control.enabledVisual; onTapped: control.tapped() }

    ToolTip {
        active: control.tip !== "" && hover.hovered
        text: control.tip
        shortcut: control.tipShortcut
    }
}
