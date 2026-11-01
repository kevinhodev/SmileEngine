pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Cada instância recebe a viewport ativa e os modelos de render/debug compostos pelo MainWindow.
Rectangle {
    id: root

    required property var viewportModel
    required property var renderModel
    required property var debugModel
    required property var restirModel

    color: C.Theme.shell
    implicitWidth: 900
    implicitHeight: 38

    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color accent: C.Theme.accent
    readonly property bool compact: width < 1040
    readonly property bool narrow: width < 760

    // Metrica UNICA da faixa. Botao de icone e chip tinham alturas diferentes (28 x 24) e a
    // Row alinha pelo topo: os chips ficavam 2 px acima do centro. Com um valor so nao ha
    // como divergir de novo, e cada controle novo herda o alinhamento de graca.
    readonly property int controlH: 26
    readonly property int iconPx: 14
    readonly property int dividerH: 16

    // "Este preset é o valor vigente?" — com TOLERÂNCIA, e não com ===.
    //
    // O passo é guardado como float no C++ e chega aqui promovido a double, então 0.10f vira
    // 0.10000000149…, que não é o double do literal 0.10 deste arquivo. A comparação estrita
    // reprovava justamente os presets pequenos (1 cm, 5 cm, 10 cm, 1%, 5%, 10%) e a marca de
    // seleção sumia neles; os de rotação escapavam por serem inteiros, exatos nos dois lados.
    //
    // Tolerância RELATIVA porque o erro do float ESCALA com a magnitude: ~1e-9 em 0,01 e ~1e-6
    // em 10. Um epsilon fixo tem de cobrir o maior deles, e aí fica preso à maior entrada da
    // lista — um preset de 1000 m (erro ~1e-4) passaria a reprovar sozinho. Relativo acompanha.
    //
    // Para a lista de HOJE um 1e-5 fixo também serviria: os presets vizinhos estão a 5× de
    // distância (0,01 vs 0,05 = 0,04 de folga), ordens de grandeza acima de qualquer epsilon
    // plausível. A escolha aqui é por não precisar revisitar isto quando a lista crescer.
    function samePreset(a, b) { return Math.abs(a - b) <= Math.abs(b) * 1e-5 }

    // Separador de grupo. A caixa e mais larga que o fio de proposito: e ela que cria o
    // respiro entre grupos de controle, que o spacing de 3 px da Row nao dava.
    component Divider: Item {
        width: 11
        height: root.controlH
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        Rectangle {
            anchors.centerIn: parent
            width: 1; height: root.dividerH
            color: C.Theme.hairline
        }
    }

    component IconToolButton: Rectangle {
        id: iconButton
        property string iconName
        property string tip: ""
        property bool active: false
        property bool enabledVisual: true
        property string tipShortcut: ""
        signal tapped()
        implicitWidth: root.controlH
        implicitHeight: root.controlH
        radius: C.Theme.radiusControl
        opacity: enabledVisual ? 1.0 : 0.38
        // Ligado = segmento afundado em cinza claro com o icone no accent (ferramenta ativa).
        color: active ? C.Theme.raisedOn
                      : (iconHover.hovered && enabledVisual ? C.Theme.raisedHover
                                                            : "transparent")
        C.ToolbarIcon {
            anchors.centerIn: parent
            width: root.iconPx; height: root.iconPx
            name: iconButton.iconName
            color: iconButton.active ? root.accent : C.Theme.textSecondary
        }
        HoverHandler {
            id: iconHover
            cursorShape: iconButton.enabledVisual ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler { enabled: iconButton.enabledVisual; onTapped: iconButton.tapped() }
        C.ToolTip {
            active: iconButton.tip !== "" && iconHover.hovered
            text: iconButton.tip
            shortcut: iconButton.tipShortcut
        }
    }

    component RadioMark: Item {
        id: radioMark
        property bool checked: false
        implicitWidth: 12
        implicitHeight: 12
        onCheckedChanged: radioCanvas.requestPaint()
        Canvas {
            id: radioCanvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                const cx = width / 2
                const cy = height / 2
                ctx.reset()
                ctx.lineWidth = 1.25
                ctx.strokeStyle = radioMark.checked ? root.accent : C.Theme.hairlineStrong
                ctx.beginPath()
                ctx.arc(cx, cy, 4.75, 0, Math.PI * 2)
                ctx.stroke()

                if (radioMark.checked) {
                    ctx.fillStyle = root.accent
                    ctx.beginPath()
                    ctx.arc(cx, cy, 2.25, 0, Math.PI * 2)
                    ctx.fill()
                }
            }
        }
    }

    component ModeRow: Rectangle {
        id: modeRow
        property string label
        property string shortcutText: ""
        property string sublabel: ""
        property bool selected: false
        property bool beta: false
        property bool hasSubmenu: false
        signal tapped()

        radius: C.Theme.radiusControl
        color: selected ? C.Theme.accentBg : (modeHover.hovered ? C.Theme.raisedHover : "transparent")

        RadioMark {
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.top: parent.top
            anchors.topMargin: modeRow.sublabel === "" ? (parent.height - height) / 2 : 8
            checked: modeRow.selected
        }
        Text {
            id: modeLabel
            anchors.left: parent.left
            anchors.leftMargin: 26
            anchors.top: parent.top
            anchors.topMargin: modeRow.sublabel === "" ? (parent.height - height) / 2 : 4
            text: modeRow.label
            color: modeRow.selected ? root.textPrimary : root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
        }
        Rectangle {
            visible: modeRow.beta
            anchors.left: modeLabel.right
            anchors.leftMargin: 8
            anchors.verticalCenter: modeLabel.verticalCenter
            width: betaText.implicitWidth + 10; height: 16; radius: C.Theme.radiusChip
            color: C.Theme.warnRow
            Text {
                id: betaText
                anchors.centerIn: parent
                text: "beta"
                color: C.Theme.amber
                font.family: C.Theme.fontFamily
                font.pixelSize: C.Theme.textMeta
                font.weight: Font.Medium
            }
        }
        Text {
            visible: modeRow.sublabel !== ""
            anchors.left: parent.left
            anchors.leftMargin: 26
            anchors.top: modeLabel.bottom
            anchors.topMargin: 1
            text: modeRow.sublabel
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textMeta
        }
        Text {
            visible: modeRow.shortcutText !== ""
            anchors.right: parent.right
            anchors.rightMargin: 7
            anchors.verticalCenter: modeLabel.verticalCenter
            text: modeRow.shortcutText
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textMeta
        }
        Canvas {
            visible: modeRow.hasSubmenu
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 7; height: 12
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = root.textMuted
                ctx.lineWidth = 1.3
                ctx.lineCap = "square"
                ctx.beginPath()
                ctx.moveTo(1.5, 1.5)
                ctx.lineTo(5.5, 6)
                ctx.lineTo(1.5, 10.5)
                ctx.stroke()
            }
        }
        HoverHandler { id: modeHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: modeRow.tapped() }
    }

    component ToggleRow: Rectangle {
        id: toggleRow
        property string label
        property string detail: ""
        property bool checked: false
        property bool interactive: true
        signal toggled()

        radius: C.Theme.radiusControl
        opacity: interactive ? 1.0 : 0.48
        color: toggleHover.hovered && interactive ? C.Theme.raisedHover : "transparent"

        Text {
            id: toggleLabel
            anchors.left: parent.left
            anchors.leftMargin: 26
            anchors.verticalCenter: parent.verticalCenter
            text: toggleRow.label
            color: toggleHover.hovered && toggleRow.interactive ? root.textPrimary : root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
        }
        Text {
            visible: toggleRow.detail !== ""
            anchors.left: toggleLabel.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: "· " + toggleRow.detail
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textMeta
        }
        // Mesmo checkbox do resto do editor (components/Toggle.qml), so que passivo: quem
        // recebe o clique e a linha inteira.
        C.Toggle {
            id: toggleTrack
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            checked: toggleRow.checked
            interactive: false
            opacity: 1.0
        }
        HoverHandler {
            id: toggleHover
            cursorShape: toggleRow.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler {
            enabled: toggleRow.interactive
            onTapped: toggleRow.toggled()
        }
    }

        // Os tres snaps. O botao inteiro abre a lista, e a lista tem "Desligado" no topo — mesmo
        // padrao do popup de render targets logo abaixo neste arquivo. A alternativa (corpo
        // alterna, seta abre) exigiria dois TapHandler disputando o mesmo item, e um deles
        // ganharia o gesto de forma nao especificada.
        //
        // Os valores saem dos arrays default da UE (BaseEditorPerProjectUserSettings.ini),
        // convertidos de cm para metros na translacao — a Smile trabalha em metros.
        component SnapButton: Item {
            id: snapButton
            // 1 mover · 2 rotacionar · 3 escalar (mesmos numeros de gizmoMode)
            required property int mode
            required property string iconName
            required property string label
            required property bool on
            required property var presets      // [{ v: número, t: "rótulo" }]
            required property real currentValue // valor vigente; a lista marca o preset ativo
            property string tip: ""

            implicitWidth: body.implicitWidth
            implicitHeight: root.controlH

            C.ToolbarButton {
                id: body
                anchors.fill: parent
                iconName: snapButton.iconName
                label: snapButton.label
                monoLabel: true   // valor de snap e dado, nao comando
                dropDown: true
                active: snapButton.on
                tip: snapButton.tip
                tipShortcut: "Ctrl inverte"
                onTapped: snapPopup.opened ? snapPopup.close() : snapPopup.open()
            }

            Popup {
                id: snapPopup
                popupType: Popup.Window
                y: snapButton.height + 4
                width: 132
                height: 8 + (snapButton.presets.length + 1) * 26
                padding: 4
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                background: Rectangle {
                    color: C.Theme.surfaceRaised; border.color: C.Theme.hairlineStrong; border.width: 1
                    radius: C.Theme.radiusCard
                }
                contentItem: Column {
                    SnapRow {
                        text: "Desligado"
                        muted: true
                        checked: !snapButton.on
                        onPicked: {
                            if (snapButton.on) root.viewportModel.ToggleSnap(snapButton.mode)
                            snapPopup.close()
                        }
                    }
                    Repeater {
                        model: snapButton.presets
                        delegate: SnapRow {
                            required property var modelData
                            text: modelData.t
                            checked: snapButton.on && root.samePreset(snapButton.currentValue, modelData.v)
                            onPicked: {
                                root.viewportModel.SetSnapValue(snapButton.mode, modelData.v)
                                if (!snapButton.on) root.viewportModel.ToggleSnap(snapButton.mode)
                                snapPopup.close()
                            }
                        }
                    }
                }
            }
        }

        // Linha do popup de snap. Fora do SnapButton para o Repeater nao precisar alcancar ids de
        // um componente externo — que e o que gera os avisos de acesso nao qualificado.
        component SnapRow: Rectangle {
            id: snapRow
            property alias text: snapRowLabel.text
            property bool checked: false
            property bool muted: false
            signal picked()
            width: 124; height: 26; radius: 4
            color: snapRowHover.hovered ? C.Theme.raisedOn : "transparent"
            RadioMark {
                anchors.left: parent.left; anchors.leftMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                checked: snapRow.checked
            }
            Text {
                id: snapRowLabel
                anchors.left: parent.left; anchors.leftMargin: 27
                anchors.verticalCenter: parent.verticalCenter
                color: snapRow.muted ? root.textMuted : root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
            }
            HoverHandler { id: snapRowHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: snapRow.picked() }
        }


    // Já ligados ao editor: o seletor Lit e o grupo Selecionar/Mover/Rotacionar/Escalar.
    // Ainda camada visual (conectados em etapas próprias): desfazer/refazer, Global/Local,
    // os três snaps, câmera, Mostrar, enquadrar, velocidade, stats, grade e maximizar.
    Row {
        id: leftTools
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1
            IconToolButton { iconName: "undo"; tip: "Desfazer"; tipShortcut: "Ctrl+Z" }
            IconToolButton {
                iconName: "redo"
                tip: "Nada para refazer"
                enabledVisual: false
            }
        }

        Divider {}

        Item {
            width: root.controlH * 4 + 3; height: root.controlH
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.fill: parent
                radius: C.Theme.radiusControl
                color: C.Theme.surface
            }
            // Grupo exclusivo: o estado vive no GizmoController (C++), nao aqui — os atalhos
            // Q/W/E/R chegam pelo ViewportWidget e a marcacao acompanha pelo mesmo binding.
            Row {
                anchors.fill: parent
                spacing: 1
                IconToolButton {
                    width: root.controlH; iconName: "select"; tip: "Selecionar"; tipShortcut: "Q"
                    active: root.viewportModel.gizmoMode === 0
                    onTapped: root.viewportModel.SetGizmoMode(0)
                }
                IconToolButton {
                    width: root.controlH; iconName: "move"; tip: "Mover"; tipShortcut: "W"
                    active: root.viewportModel.gizmoMode === 1
                    onTapped: root.viewportModel.SetGizmoMode(1)
                }
                IconToolButton {
                    width: root.controlH; iconName: "rotate"; tip: "Rotacionar"; tipShortcut: "E"
                    active: root.viewportModel.gizmoMode === 2
                    onTapped: root.viewportModel.SetGizmoMode(2)
                }
                IconToolButton {
                    width: root.controlH; iconName: "scale"; tip: "Escalar"; tipShortcut: "R"
                    active: root.viewportModel.gizmoMode === 3
                    onTapped: root.viewportModel.SetGizmoMode(3)
                }
            }
        }

        // Alternância de dois estados, não dropdown: só existem Global e Local, e um menu de dois
        // itens custa dois cliques pelo que um resolve. Vira dropdown no dia em que houver um
        // terceiro espaço (a Cry tem Parent, View e UserDefined).
        //
        // O botão mostra o espaço EFETIVO, não o escolhido, e quando os dois divergem ele aparece
        // travado com o motivo no tooltip. As duas restrições vêm do C++ (gizmoSpaceRestriction)
        // em vez de serem deduzidas aqui: deduzir é o que fazia a luz operar em mundo enquanto o
        // botão dizia "Local".
        C.ToolbarButton {
            anchors.verticalCenter: parent.verticalCenter
            // 0 = nenhuma · 1 = escala é sempre local · 2 = luz não tem base local
            readonly property int restriction: root.viewportModel.gizmoSpaceRestriction
            readonly property bool isLocal: root.viewportModel.gizmoSpaceEffective === 1
            visible: !root.narrow
            label: isLocal ? "Local" : "Global"
            enabledVisual: restriction === 0
            tip: restriction === 1 ? "Escala é sempre no espaço local do objeto"
               : restriction === 2 ? "Luz não tem espaço local: só tem direção, sem roll"
               : (isLocal ? "Espaço de transformação: Local (eixos do objeto)"
                          : "Espaço de transformação: Global (eixos do mundo)")
            tipShortcut: restriction === 0 ? "Ctrl+`" : ""
            onTapped: root.viewportModel.ToggleGizmoSpace()
        }

        Divider { visible: !root.narrow }

        SnapButton {
            anchors.verticalCenter: parent.verticalCenter
            mode: 1
            currentValue: root.viewportModel.snapTranslateM
            iconName: "magnet"
            label: root.viewportModel.snapTranslateM < 1.0
                       ? Math.round(root.viewportModel.snapTranslateM * 100) + " cm"
                       : root.viewportModel.snapTranslateM + " m"
            on: root.viewportModel.snapTranslateOn
            tip: "Snap de movimento"
            presets: [{ v: 0.01, t: "1 cm" }, { v: 0.05, t: "5 cm" }, { v: 0.10, t: "10 cm" },
                      { v: 0.25, t: "25 cm" }, { v: 0.50, t: "50 cm" }, { v: 1.0, t: "1 m" },
                      { v: 5.0, t: "5 m" }, { v: 10.0, t: "10 m" }]
        }
        SnapButton {
            visible: !root.compact
            anchors.verticalCenter: parent.verticalCenter
            mode: 2
            currentValue: root.viewportModel.snapRotateDeg
            iconName: "angle"
            label: root.viewportModel.snapRotateDeg + "°"
            on: root.viewportModel.snapRotateOn
            tip: "Snap de rotação"
            presets: [{ v: 5, t: "5°" }, { v: 10, t: "10°" }, { v: 15, t: "15°" },
                      { v: 30, t: "30°" }, { v: 45, t: "45°" }, { v: 90, t: "90°" }]
        }
        SnapButton {
            visible: !root.compact
            anchors.verticalCenter: parent.verticalCenter
            mode: 3
            currentValue: root.viewportModel.snapScaleStep
            iconName: "percent"
            label: Math.round(root.viewportModel.snapScaleStep * 100) + "%"
            on: root.viewportModel.snapScaleOn
            tip: "Snap de escala"
            presets: [{ v: 0.01, t: "1%" }, { v: 0.05, t: "5%" }, { v: 0.10, t: "10%" },
                      { v: 0.25, t: "25%" }, { v: 0.50, t: "50%" }, { v: 1.0, t: "100%" }]
        }

        Divider {}

        C.ToolbarButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.compact
            flat: true
            label: "Perspectiva"
            dropDown: true
            tip: "Projeção e vistas da câmera"
        }
        // O modo de visualizacao nao usa o accent: ele nao e "ativo", e o VALOR vigente de um
        // seletor. O ponto ambar carrega o sentido (luz) e o rotulo forte a enfase.
        C.ToolbarButton {
            anchors.verticalCenter: parent.verticalCenter
            id: viewModeButton
            label: root.debugModel.viewModeLabel
            dotColor: C.Theme.amber
            strongLabel: true
            dropDown: true
            onTapped: {
                if (viewModesPopup.opened) viewModesPopup.close()
                else viewModesPopup.open()
            }
        }
        C.ToolbarButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.narrow
            flat: true
            label: "Mostrar"
            dropDown: true
            tip: "Filtros de visibilidade"
        }
    }

    Row {
        id: rightTools
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.compact ? 2 : 4

        IconToolButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.narrow
            iconName: "focus"
            tip: "Enquadrar seleção"
            tipShortcut: "F"
        }
        C.ToolbarButton {
            anchors.verticalCenter: parent.verticalCenter
            flat: true
            label: "4.0×"
            monoLabel: true
            dropDown: true
            tip: "Velocidade da câmera: 4,0×"
        }

        Divider {}

        IconToolButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "grid"
            active: true
            tip: "Grade visível"
        }
        IconToolButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.narrow
            iconName: "stats"
            tip: "Estatísticas de desempenho"
        }
        IconToolButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "maximize"; tip: "Maximizar viewport"
        }
        IconToolButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "more"; tip: "Mais opções"
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: C.Theme.hairlineStrong
    }

    Popup {
        id: viewModesPopup
        popupType: Popup.Window
        x: leftTools.x + viewModeButton.x
        y: root.height
        width: 280
        height: 424
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Estado que muda sem toggle (o 2-state de OMM bloqueia/libera no load de cena).
        onAboutToShow: root.renderModel.RefreshGISettings()
        onClosed: {
            if (debugTargetPopup.opened) debugTargetPopup.close()
        }

        background: Rectangle {
            color: C.Theme.surfaceRaised
            border.color: C.Theme.hairlineStrong
            border.width: 1
            radius: C.Theme.radiusCard
        }

        contentItem: Item {
            Text {
                x: 14; y: 11
                text: "Modo de visualização"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }

            // Os modos sao mutuamente exclusivos: um alvo de debug ativo desmarca Lit/Heatmap
            // (e vice-versa), porque o C++ limpa o outro estado ao selecionar.
            ModeRow {
                x: 8; y: 30; width: 264; height: 26
                label: "Lit"; shortcutText: "Alt+1"
                selected: root.viewportModel.viewMode === 0 && root.debugModel.debugTargetIndex < 0
                onTapped: { root.viewportModel.SelectLit(); viewModesPopup.close() }
            }
            ModeRow {
                x: 8; y: 60; width: 264; height: 28
                label: "Heatmap de reflexos"; shortcutText: "Alt+5"
                selected: root.viewportModel.viewMode === 3 && root.debugModel.debugTargetIndex < 0
                onTapped: { root.viewportModel.SelectReflectionHeatmap(); viewModesPopup.close() }
            }
            ModeRow {
                x: 8; y: 90; width: 264; height: 28
                label: "Render targets"; hasSubmenu: true
                selected: root.debugModel.debugTargetIndex >= 0
                onTapped: {
                    if (debugTargetPopup.opened) debugTargetPopup.close()
                    else debugTargetPopup.open()
                }
            }

            Rectangle { x: 14; y: 122; width: 252; height: 1; color: C.Theme.hairline }
            Text {
                x: 14; y: 132
                text: "Iluminação global"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
            ToggleRow {
                x: 8; y: 150; width: 264; height: 28
                label: "DDGI"; detail: "radiance cache"
                checked: root.renderModel.ddgiEnabled
                onToggled: root.renderModel.ToggleDDGI()
            }
            ModeRow {
                x: 8; y: 178; width: 264; height: 28
                label: "ReSTIR Lab"; shortcutText: "Ctrl+Shift+R"
                onTapped: { root.restirModel.OpenWindow(); viewModesPopup.close() }
            }
            ToggleRow {
                x: 8; y: 206; width: 264; height: 28
                label: "Folhagem sombreia GI"; detail: "alpha-test"
                checked: root.renderModel.giFoliageShadows
                onToggled: root.renderModel.ToggleGIFoliageShadows()
            }
            // Pedido x valendo: o C++ so liga com a cena toda coberta por micro-map (.somm).
            ToggleRow {
                x: 8; y: 234; width: 264; height: 28
                label: "OMM 2-state no GI"
                detail: !root.renderModel.giOmm2State ? "sem alpha-test"
                        : (root.renderModel.giOmm2StateActive ? "ativo" : "bloqueado: sem .somm")
                checked: root.renderModel.giOmm2State
                onToggled: root.renderModel.ToggleGIOmm2State()
            }
            ToggleRow {
                x: 8; y: 262; width: 264; height: 28
                label: "GTAO"
                checked: root.renderModel.gtaoEnabled
                onToggled: root.renderModel.ToggleGTAO()
            }
            ToggleRow {
                x: 8; y: 290; width: 264; height: 28
                label: "GTAO meia-res"; detail: "upsample bilateral"
                checked: root.renderModel.gtaoHalfRes
                onToggled: root.renderModel.ToggleGTAOHalfRes()
            }

            Rectangle { x: 14; y: 322; width: 252; height: 1; color: C.Theme.hairline }
            Text {
                x: 14; y: 332
                text: "Reflexos"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
            ToggleRow {
                x: 8; y: 350; width: 264; height: 28
                label: "Reflexos RT"
                checked: root.renderModel.reflectionsEnabled
                onToggled: root.renderModel.ToggleReflections()
            }
            // O denoiser (Nenhum/NRD/DLSS Ray Reconstruction) saiu daqui: virou seletor na pagina
            // Renderizacao (Configuracoes), pois RR e uma escolha de 3 estados que acopla o upscaler.

            Rectangle { x: 14; y: 382; width: 252; height: 1; color: C.Theme.hairline }
            Rectangle {
                x: 8; y: 388; width: 264; height: 28; radius: 5
                color: settingsHover.hovered ? C.Theme.raisedOn : "transparent"
                Text {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "☼"
                    color: C.Theme.textSecondary
                    font.family: "Segoe UI Symbol"
                    font.pixelSize: 14
                }
                Text {
                    x: 26
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Configurações de renderização…"
                    color: C.Theme.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Ctrl+,"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }
                HoverHandler { id: settingsHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        viewModesPopup.close()
                        root.viewportModel.RequestSettings()
                    }
                }
                C.ToolTip {
                    active: settingsHover.hovered
                    text: "Abrir configurações de renderização"
                }
            }
        }
    }

    // Submenu do visualizador generico. A lista NAO e hardcoded: vem de debugTargetNames, que
    // o bridge le do registro DebugTargets. Passe que se registre aparece aqui sozinho.
    Popup {
        id: debugTargetPopup
        popupType: Popup.Window
        x: viewModesPopup.x + viewModesPopup.width - 4
        y: root.height + 56
        width: 232
        // Cresce com a lista (+1 pela linha "Desligado"), com teto p/ nao estourar a tela.
        height: Math.min(12 + (root.debugModel.debugTargetNames.length + 1) * 28, 460)
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: C.Theme.surfaceRaised
            border.color: C.Theme.hairlineStrong
            border.width: 1
            radius: C.Theme.radiusCard
        }
        contentItem: Flickable {
            contentHeight: debugTargetCol.height
            clip: true
            Column {
                id: debugTargetCol
                Rectangle {
                    width: 220; height: 28; radius: 4
                    color: offHover.hovered ? C.Theme.raisedOn : "transparent"
                    RadioMark {
                        anchors.left: parent.left; anchors.leftMargin: 7
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.debugModel.debugTargetIndex < 0
                    }
                    Text {
                        anchors.left: parent.left; anchors.leftMargin: 27
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Desligado"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                    }
                    HoverHandler { id: offHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            root.debugModel.SelectDebugTarget(-1)
                            debugTargetPopup.close()
                            viewModesPopup.close()
                        }
                    }
                }
                Repeater {
                     model: root.debugModel.debugTargetNames
                     delegate: Rectangle {
                         id: debugTargetOption
                         required property string modelData
                         required property int index
                        width: 220; height: 28; radius: 4
                        color: targetHover.hovered ? C.Theme.raisedOn : "transparent"
                        RadioMark {
                            anchors.left: parent.left; anchors.leftMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                             checked: root.debugModel.debugTargetIndex === debugTargetOption.index
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 27
                            anchors.right: parent.right; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                             text: debugTargetOption.modelData
                            elide: Text.ElideRight
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 12
                        }
                        HoverHandler { id: targetHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                 root.debugModel.SelectDebugTarget(debugTargetOption.index)
                                debugTargetPopup.close()
                                viewModesPopup.close()
                            }
                        }
                    }
                }
            }
        }
    }

}
