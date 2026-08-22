import QtQuick

// Botão denso da toolbar: a largura abraça o conteúdo e todos os controles seguem
// exatamente a mesma métrica horizontal.
Rectangle {
    id: control

    property string label: ""
    property string iconName: ""
    property bool active: false
    property bool dropDown: false
    // Rotulo que carrega DADO (valor de snap, contagem) vai em mono; texto de comando fica sans.
    property bool monoLabel: false
    // Ponto SEMANTICO a esquerda do rotulo (ambar = luz, azul = info, verde = ok). Existe
    // para o botao poder sinalizar um estado sem recorrer ao accent, que e reservado a
    // selecao/ativo/dirty — e o caso do seletor de modo de visualizacao (Lit).
    property color dotColor: "transparent"
    // Rotulo em texto principal: o controle esta "cheio" (carrega um valor escolhido) sem
    // estar no estado ativo do accent.
    property bool strongLabel: false
    // Sem chip: so texto e chevron, com o hover marcando a area clicavel. A referencia usa
    // isto nos controles que abrem MENU (Perspectiva, Mostrar) e reserva a moldura para os
    // que exibem um VALOR vigente (Local, snaps, Lit).
    property bool flat: false
    property bool enabledVisual: true
    property string tip: ""
    property string tipShortcut: ""

    signal tapped()

    implicitWidth: contents.implicitWidth + 13
    implicitHeight: 24
    radius: Theme.radiusControl
    opacity: enabledVisual ? 1.0 : 0.42
    color: active ? Theme.accentBg
         : hover.hovered && enabledVisual ? Theme.surfaceRaised
         : flat ? "transparent"
         : Theme.surface
    border.color: active ? Theme.accentBorder
                : flat ? "transparent"
                : (hover.hovered && enabledVisual ? Theme.hairlineStrong : Theme.hairline)
    border.width: 1

    Row {
        id: contents
        anchors.centerIn: parent
        spacing: 5

        Rectangle {
            visible: control.dotColor.a > 0
            anchors.verticalCenter: parent.verticalCenter
            width: 6; height: 6; radius: 3
            color: control.dotColor
        }

        ToolbarIcon {
            visible: control.iconName !== ""
            width: 14; height: 14
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            color: control.active ? Theme.accent : Theme.textSecondary
        }

        Text {
            visible: control.label !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.label
            color: control.active ? Theme.accent
                 : control.strongLabel ? Theme.textPrimary
                 : Theme.textSecondary
            font.family: control.monoLabel ? Theme.fontMono : Theme.fontFamily
            font.pixelSize: control.monoLabel ? 10 : 11
            renderType: Text.NativeRendering
        }

        Canvas {
            visible: control.dropDown
            width: 10; height: 7
            anchors.verticalCenter: parent.verticalCenter
            property color strokeColor: control.active ? Theme.accent : Theme.textMuted
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
