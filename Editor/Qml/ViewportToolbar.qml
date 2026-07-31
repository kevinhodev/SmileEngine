import QtQuick
import QtQuick.Controls
import "components" as C

// Toolbar do viewport e dropdown de view modes. Cada instancia recebe explicitamente o
// ViewportWidget que controla; os atalhos globais sao roteados pelo MainWindow para a ativa.
Rectangle {
    id: root

    required property var viewportModel

    color: "#131410"
    implicitWidth: 900
    implicitHeight: 34

    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textMuted: "#6c6a61"
    readonly property color blue: "#5b9dff"
    readonly property bool compact: width < 1040
    readonly property bool narrow: width < 760

    component IconToolButton: Rectangle {
        id: iconButton
        property string iconName
        property string tip: ""
        property bool active: false
        property bool enabledVisual: true
        property bool framed: false
        property string tipShortcut: ""
        signal tapped()
        implicitWidth: 26
        implicitHeight: 22
        radius: 5
        opacity: enabledVisual ? 1.0 : 0.38
        color: active ? "#182c4b"
                      : (iconHover.hovered && enabledVisual ? "#22241e"
                         : (framed ? "#1a1c17" : "transparent"))
        border.color: active ? "#315b91"
                             : (iconHover.hovered && enabledVisual ? "#393b32"
                                : (framed ? "#2d2f28" : "transparent"))
        border.width: 1
        C.ToolbarIcon {
            anchors.centerIn: parent
            width: 14; height: 14
            name: iconButton.iconName
            color: iconButton.active ? "#72adff" : "#a7a397"
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
                ctx.strokeStyle = radioMark.checked ? root.blue : "#4a4b41"
                ctx.beginPath()
                ctx.arc(cx, cy, 4.75, 0, Math.PI * 2)
                ctx.stroke()

                if (radioMark.checked) {
                    ctx.fillStyle = root.blue
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

        radius: 5
        color: selected ? "#16233f" : (modeHover.hovered ? "#22231c" : "transparent")

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
            width: 34; height: 15; radius: 7.5
            color: "#33260f"
            border.color: "#57431a"
            Text {
                anchors.centerIn: parent
                text: "beta"
                color: "#d8a03a"
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
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
            font.pixelSize: 10
        }
        Text {
            visible: modeRow.shortcutText !== ""
            anchors.right: parent.right
            anchors.rightMargin: 7
            anchors.verticalCenter: modeLabel.verticalCenter
            text: modeRow.shortcutText
            color: modeRow.selected ? "#8fb4e8" : root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
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
                ctx.lineCap = "round"
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

        radius: 5
        opacity: interactive ? 1.0 : 0.48
        color: toggleHover.hovered && interactive ? "#22231c" : "transparent"

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
            font.pixelSize: 10
        }
        Rectangle {
            id: toggleTrack
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            width: 28; height: 16; radius: 8
            color: toggleRow.checked ? root.blue : "#383931"
            border.color: toggleRow.checked ? root.blue : "#4a4b41"
            Rectangle {
                width: 11; height: 11; radius: 5.5
                y: 2.5
                x: toggleRow.checked ? 14.5 : 2.5
                color: toggleRow.checked ? "#f2efe6" : "#a29e93"
                Behavior on x { NumberAnimation { duration: 100 } }
            }
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

    // Camada visual apenas. Exceto o seletor Lit, os novos controles serão conectados
    // progressivamente aos bridges do editor em etapas próprias.
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

        Rectangle {
            width: 1; height: 18
            anchors.verticalCenter: parent.verticalCenter
            color: "#2d2f28"
        }

        Item {
            width: 107; height: 22
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "#181a15"
                border.color: "#2d2f28"
            }
            Row {
                anchors.fill: parent
                spacing: 1
                IconToolButton { width: 26; iconName: "select"; tip: "Selecionar"; tipShortcut: "Q" }
                IconToolButton { width: 26; iconName: "move"; active: true; tip: "Mover"; tipShortcut: "W" }
                IconToolButton { width: 26; iconName: "rotate"; tip: "Rotacionar"; tipShortcut: "E" }
                IconToolButton { width: 26; iconName: "scale"; tip: "Escalar"; tipShortcut: "R" }
            }
        }

        C.ToolbarButton {
            visible: !root.narrow
            iconName: "world"
            label: root.compact ? "" : "Global"
            dropDown: true
            tip: "Espaço de transformação: Global"
        }

        Rectangle {
            width: 1; height: 18
            visible: !root.narrow
            anchors.verticalCenter: parent.verticalCenter
            color: "#2d2f28"
        }

        C.ToolbarButton {
            iconName: "magnet"
            label: "10 cm"
            dropDown: true
            active: true
            tip: "Snap de movimento: 10 cm"
        }
        C.ToolbarButton {
            visible: !root.compact
            iconName: "angle"
            label: "15°"
            dropDown: true
            tip: "Snap de rotação: 15°"
        }
        C.ToolbarButton {
            visible: !root.compact
            iconName: "percent"
            label: "10%"
            dropDown: true
            tip: "Snap de escala: 10%"
        }

        Rectangle {
            width: 1; height: 18
            anchors.verticalCenter: parent.verticalCenter
            color: "#2d2f28"
        }

        C.ToolbarButton {
            iconName: "camera"
            label: root.compact ? "" : "Perspectiva"
            dropDown: true
            tip: "Projeção e vistas da câmera"
        }
        C.ToolbarButton {
            id: viewModeButton
            iconName: "lit"
            label: root.compact ? "" : root.viewportModel.viewModeLabel
            dropDown: true
            active: true
            onTapped: {
                if (viewModesPopup.opened) viewModesPopup.close()
                else viewModesPopup.open()
            }
        }
        C.ToolbarButton {
            visible: !root.narrow
            iconName: "eye"
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
            visible: !root.narrow
            iconName: "focus"
            tip: "Enquadrar seleção"
            tipShortcut: "F"
        }
        C.ToolbarButton {
            iconName: "speed"
            label: root.compact ? "" : "4,0×"
            dropDown: true
            tip: "Velocidade da câmera: 4,0×"
        }
        C.ToolbarButton {
            visible: !root.narrow
            iconName: "stats"
            label: root.compact ? "" : "Stats"
            tip: "Estatísticas de desempenho"
        }
        IconToolButton {
            iconName: "grid"
            active: true
            framed: true
            tip: "Grade visível"
        }
        IconToolButton { iconName: "maximize"; tip: "Maximizar viewport" }
        IconToolButton { iconName: "more"; tip: "Mais opções" }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#292b24"
    }

    Popup {
        id: viewModesPopup
        popupType: Popup.Window
        x: leftTools.x + viewModeButton.x
        y: root.height
        width: 280
        height: 452
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: {
            if (debugTargetPopup.opened) debugTargetPopup.close()
        }

        background: Rectangle {
            color: "#1b1c17"
            border.color: "#33342c"
            border.width: 1
            radius: 8
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
                selected: root.viewportModel.viewMode === 0 && root.viewportModel.debugTargetIndex < 0
                onTapped: { root.viewportModel.SelectLit(); viewModesPopup.close() }
            }
            ModeRow {
                x: 8; y: 60; width: 264; height: 28
                label: "Heatmap de reflexos"; shortcutText: "Alt+5"
                selected: root.viewportModel.viewMode === 3 && root.viewportModel.debugTargetIndex < 0
                onTapped: { root.viewportModel.SelectReflectionHeatmap(); viewModesPopup.close() }
            }
            // Visualizador generico: lista TODOS os alvos publicados em DebugTargets, incluindo
            // os campos do G-buffer. A lista vem do bridge (debugTargetNames), entao passe novo
            // que se registre aparece aqui sozinho.
            ModeRow {
                x: 8; y: 90; width: 264; height: 28
                label: "Render targets"; hasSubmenu: true
                selected: root.viewportModel.debugTargetIndex >= 0
                onTapped: {
                    if (debugTargetPopup.opened) debugTargetPopup.close()
                    else debugTargetPopup.open()
                }
            }

            Rectangle { x: 14; y: 122; width: 252; height: 1; color: "#23241d" }
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
                checked: root.viewportModel.ddgiEnabled
                onToggled: root.viewportModel.ToggleDDGI()
            }
            ToggleRow {
                x: 8; y: 178; width: 264; height: 28
                label: "ReSTIR GI"
                checked: root.viewportModel.restirGIEnabled
                onToggled: root.viewportModel.ToggleReSTIRGI()
            }
            ToggleRow {
                x: 8; y: 206; width: 264; height: 28
                label: "ReSTIR visibility"; detail: "raio extra"
                checked: root.viewportModel.restirGIVisibilityEnabled
                interactive: root.viewportModel.restirGIEnabled
                onToggled: root.viewportModel.ToggleReSTIRGIVisibility()
            }
            ToggleRow {
                x: 8; y: 234; width: 264; height: 28
                label: "ReGIR"; detail: "hits secundários"
                checked: root.viewportModel.reGIREnabled
                onToggled: root.viewportModel.ToggleReGIR()
            }
            ToggleRow {
                x: 8; y: 262; width: 264; height: 28
                label: "Folhagem sombreia GI"; detail: "alpha-test"
                checked: root.viewportModel.giFoliageShadows
                onToggled: root.viewportModel.ToggleGIFoliageShadows()
            }
            ToggleRow {
                x: 8; y: 290; width: 264; height: 28
                label: "GTAO"
                checked: root.viewportModel.gtaoEnabled
                onToggled: root.viewportModel.ToggleGTAO()
            }
            ToggleRow {
                x: 8; y: 318; width: 264; height: 28
                label: "GTAO meia-res"; detail: "upsample bilateral"
                checked: root.viewportModel.gtaoHalfRes
                onToggled: root.viewportModel.ToggleGTAOHalfRes()
            }

            Rectangle { x: 14; y: 350; width: 252; height: 1; color: "#23241d" }
            Text {
                x: 14; y: 360
                text: "Reflexos"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
            ToggleRow {
                x: 8; y: 378; width: 264; height: 28
                label: "Reflexos RT"
                checked: root.viewportModel.reflectionsEnabled
                onToggled: root.viewportModel.ToggleReflections()
            }
            // O denoiser (Nenhum/NRD/DLSS Ray Reconstruction) saiu daqui: virou seletor na pagina
            // Renderizacao (Configuracoes), pois RR e uma escolha de 3 estados que acopla o upscaler.

            Rectangle { x: 14; y: 410; width: 252; height: 1; color: "#23241d" }
            Rectangle {
                x: 8; y: 416; width: 264; height: 28; radius: 5
                color: settingsHover.hovered ? "#22231c" : "transparent"
                Text {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "☼"
                    color: "#9a958a"
                    font.family: "Segoe UI Symbol"
                    font.pixelSize: 14
                }
                Text {
                    x: 26
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Configurações de renderização…"
                    color: "#9a958a"
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
        height: Math.min(12 + (root.viewportModel.debugTargetNames.length + 1) * 28, 460)
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#1b1c17"
            border.color: "#33342c"
            border.width: 1
            radius: 7
        }
        contentItem: Flickable {
            contentHeight: debugTargetCol.height
            clip: true
            Column {
                id: debugTargetCol
                Rectangle {
                    width: 220; height: 28; radius: 4
                    color: offHover.hovered ? "#2a2b24" : "transparent"
                    RadioMark {
                        anchors.left: parent.left; anchors.leftMargin: 7
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.viewportModel.debugTargetIndex < 0
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
                            root.viewportModel.SelectDebugTarget(-1)
                            debugTargetPopup.close()
                            viewModesPopup.close()
                        }
                    }
                }
                Repeater {
                    model: root.viewportModel.debugTargetNames
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        width: 220; height: 28; radius: 4
                        color: targetHover.hovered ? "#2a2b24" : "transparent"
                        RadioMark {
                            anchors.left: parent.left; anchors.leftMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            checked: root.viewportModel.debugTargetIndex === index
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 27
                            anchors.right: parent.right; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            elide: Text.ElideRight
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 12
                        }
                        HoverHandler { id: targetHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                root.viewportModel.SelectDebugTarget(index)
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
