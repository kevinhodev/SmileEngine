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

    implicitWidth: contents.implicitWidth + 16
    implicitHeight: 26
    radius: Theme.radiusControl
    opacity: enabledVisual ? 1.0 : 0.42
    // Botao em relevo (um degrau acima do painel); ligado = afundado em cinza claro com o
    // icone no accent. Sem borda: o degrau de luminancia ja recorta o controle.
    color: active ? Theme.raisedOn
         : hover.hovered && enabledVisual ? Theme.raisedHover
         : flat ? "transparent"
         : Theme.surfaceRaised
    border.width: 0

    Rectangle {
        visible: !control.flat || control.active || hover.hovered
        anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 2; rightMargin: 2 }
        height: 1
        color: Theme.raisedTop
    }

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
            color: control.active || control.strongLabel ? Theme.textPrimary
                 : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textBody
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }

        Canvas {
            visible: control.dropDown
            width: 10; height: 7
            anchors.verticalCenter: parent.verticalCenter
            property color strokeColor: Theme.textMuted
            onStrokeColorChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = strokeColor
                ctx.lineWidth = 1.35
                ctx.lineCap = "square"
                ctx.lineJoin = "miter"
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
