import QtQuick
import QtQuick.Controls

// Toolbar do viewport e dropdown de view modes. `viewportModel` e o ViewportWidget
// exposto pelo C++; ele aplica os modos/toggles diretamente no Renderer.
Rectangle {
    id: root
    color: "#141511"
    implicitWidth: 900
    implicitHeight: 36

    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textMuted: "#6c6a61"
    readonly property color blue: "#5b9dff"

    component Chevron: Canvas {
        id: chevron
        property color color: root.textMuted
        implicitWidth: 10
        implicitHeight: 7
        onColorChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = color
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

    component ToolButton: Rectangle {
        id: toolButton
        property string label
        property bool active: false
        property bool dropDown: false
        property string tip: ""
        signal tapped()

        implicitHeight: 22
        implicitWidth: labelText.implicitWidth + (dropDown ? 34 : 20)
        radius: 5
        color: active ? "#16233f" : (toolHover.hovered ? "#23241d" : "#1b1c17")
        border.color: active ? "#27406e" : "#2a2b24"
        border.width: 1

        Text {
            id: labelText
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: toolButton.label
            color: toolButton.active ? root.blue : root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Chevron {
            visible: toolButton.dropDown
            anchors.right: parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            color: toolButton.active ? root.blue : "#9a958a"
        }
        HoverHandler { id: toolHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: toolButton.tapped() }
        ToolTip.visible: toolButton.tip !== "" && toolHover.hovered
        ToolTip.text: toolButton.tip
        ToolTip.delay: 500
    }

    component IconToolButton: Rectangle {
        id: iconButton
        property string glyph
        property string tip: ""
        signal tapped()
        implicitWidth: 26
        implicitHeight: 22
        radius: 5
        color: iconHover.hovered ? "#23241d" : "transparent"
        border.color: iconHover.hovered ? "#33342c" : "transparent"
        Text {
            anchors.centerIn: parent
            text: iconButton.glyph
            color: "#9a958a"
            font.family: "Segoe UI Symbol"
            font.pixelSize: 14
        }
        HoverHandler { id: iconHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: iconButton.tapped() }
        ToolTip.visible: iconButton.tip !== "" && iconHover.hovered
        ToolTip.text: iconButton.tip
        ToolTip.delay: 500
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
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
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
            font.family: "Segoe UI"
            font.pixelSize: 10
        }
        Text {
            visible: modeRow.shortcutText !== ""
            anchors.right: parent.right
            anchors.rightMargin: 7
            anchors.verticalCenter: modeLabel.verticalCenter
            text: modeRow.shortcutText
            color: modeRow.selected ? "#8fb4e8" : root.textMuted
            font.family: "Segoe UI"
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
        signal toggled()

        radius: 5
        color: toggleHover.hovered ? "#22231c" : "transparent"

        Text {
            id: toggleLabel
            anchors.left: parent.left
            anchors.leftMargin: 26
            anchors.verticalCenter: parent.verticalCenter
            text: toggleRow.label
            color: toggleHover.hovered ? root.textPrimary : root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 12
        }
        Text {
            visible: toggleRow.detail !== ""
            anchors.left: toggleLabel.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: "· " + toggleRow.detail
            color: root.textMuted
            font.family: "Segoe UI"
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
        HoverHandler { id: toggleHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: toggleRow.toggled() }
    }

    Row {
        id: leftTools
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Row {
            spacing: 1
            IconToolButton { glyph: "＋"; tip: "Adicionar (em breve)" }
            IconToolButton { glyph: "↻"; tip: "Orbitar câmera (em breve)" }
            IconToolButton { glyph: "↗"; tip: "Enquadrar seleção (em breve)" }
        }

        Rectangle {
            width: 1; height: 18
            anchors.verticalCenter: parent.verticalCenter
            color: "#2a2b24"
        }

        ToolButton {
            width: 104
            label: "Perspectiva"
            dropDown: true
            tip: "Projeção da câmera (em breve)"
        }
        ToolButton {
            id: viewModeButton
            width: Math.max(60, implicitWidth)
            label: viewportModel.viewModeLabel
            dropDown: true
            active: true
            onTapped: {
                if (viewModesPopup.opened) viewModesPopup.close()
                else viewModesPopup.open()
            }
        }
        ToolButton {
            width: 84
            label: "Mostrar"
            dropDown: true
            tip: "Filtros de visibilidade (em breve)"
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        ToolButton {
            width: 84
            label: "▣  4,0"
            tip: "Velocidade da câmera (em breve)"
        }
        IconToolButton { glyph: "▦"; tip: "Grade (em breve)" }
        IconToolButton { glyph: "⛶"; tip: "Maximizar viewport (em breve)" }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#23241d"
    }

    Popup {
        id: viewModesPopup
        popupType: Popup.Window
        x: viewModeButton.x + leftTools.x
        y: root.height
        width: 280
        height: 416
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: if (gBufferPopup.opened) gBufferPopup.close()

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
                font.family: "Segoe UI"
                font.pixelSize: 11
            }

            ModeRow {
                x: 8; y: 30; width: 264; height: 26
                label: "Lit"; shortcutText: "Alt+1"
                selected: viewportModel.viewMode === 0
                onTapped: { viewportModel.SelectLit(); viewModesPopup.close() }
            }
            ModeRow {
                x: 8; y: 60; width: 264; height: 44
                label: "Path tracer"; shortcutText: "Alt+2"; beta: true
                sublabel: "desliga Reflexos RT, GTAO e ReSTIR GI"
                selected: viewportModel.viewMode === 1
                onTapped: { viewportModel.SelectPathTracer(); viewModesPopup.close() }
            }
            ModeRow {
                x: 8; y: 108; width: 264; height: 28
                label: "Buffers do GBuffer"; hasSubmenu: true
                selected: viewportModel.viewMode === 2
                onTapped: {
                    if (gBufferPopup.opened) gBufferPopup.close()
                    else gBufferPopup.open()
                }
            }
            ModeRow {
                x: 8; y: 138; width: 264; height: 28
                label: "Heatmap de reflexos"; shortcutText: "Alt+5"
                selected: viewportModel.viewMode === 3
                onTapped: { viewportModel.SelectReflectionHeatmap(); viewModesPopup.close() }
            }

            Rectangle { x: 14; y: 170; width: 252; height: 1; color: "#23241d" }
            Text {
                x: 14; y: 180
                text: "Iluminação global"
                color: root.textMuted
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
            ToggleRow {
                x: 8; y: 198; width: 264; height: 28
                label: "DDGI"; detail: "radiance cache"
                checked: viewportModel.ddgiEnabled
                onToggled: viewportModel.ToggleDDGI()
            }
            ToggleRow {
                x: 8; y: 226; width: 264; height: 28
                label: "ReSTIR GI"
                checked: viewportModel.restirGIEnabled
                onToggled: viewportModel.ToggleReSTIRGI()
            }
            ToggleRow {
                x: 8; y: 254; width: 264; height: 28
                label: "GTAO"
                checked: viewportModel.gtaoEnabled
                onToggled: viewportModel.ToggleGTAO()
            }

            Rectangle { x: 14; y: 286; width: 252; height: 1; color: "#23241d" }
            Text {
                x: 14; y: 296
                text: "Reflexos e denoise"
                color: root.textMuted
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
            ToggleRow {
                x: 8; y: 314; width: 264; height: 28
                label: "Reflexos RT"
                checked: viewportModel.reflectionsEnabled
                onToggled: viewportModel.ToggleReflections()
            }
            ToggleRow {
                x: 8; y: 342; width: 264; height: 28
                label: "NRD REBLUR"; detail: "difuso + especular"
                checked: viewportModel.nrdEnabled
                onToggled: viewportModel.ToggleNrd()
            }

            Rectangle { x: 14; y: 374; width: 252; height: 1; color: "#23241d" }
            Rectangle {
                x: 8; y: 380; width: 264; height: 28; radius: 5
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
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Ctrl+,"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }
                HoverHandler { id: settingsHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        viewModesPopup.close()
                        viewportModel.RequestSettings()
                    }
                }
                ToolTip.visible: settingsHover.hovered
                ToolTip.text: "Abrir configurações de renderização"
                ToolTip.delay: 500
            }
        }
    }

    Popup {
        id: gBufferPopup
        popupType: Popup.Window
        x: viewModesPopup.x + viewModesPopup.width - 4
        y: root.height + 104
        width: 196
        height: 236
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#1b1c17"
            border.color: "#33342c"
            border.width: 1
            radius: 7
        }
        contentItem: Column {
            Repeater {
                model: [
                    "Base Color",
                    "World Normal",
                    "Roughness",
                    "Metallic",
                    "Emissive",
                    "Ambient Occlusion",
                    "Shading Model",
                    "Motion Vectors"
                ]
                delegate: Rectangle {
                    required property string modelData
                    required property int index
                    width: 184
                    height: 28
                    radius: 4
                    color: gBufferHover.hovered ? "#2a2b24" : "transparent"
                    RadioMark {
                        anchors.left: parent.left
                        anchors.leftMargin: 7
                        anchors.verticalCenter: parent.verticalCenter
                        checked: viewportModel.viewMode === 2 && viewportModel.gBufferMode === index + 1
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 27
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: root.textNormal
                        font.family: "Segoe UI"
                        font.pixelSize: 12
                    }
                    HoverHandler { id: gBufferHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            viewportModel.SelectGBuffer(index + 1)
                            gBufferPopup.close()
                            viewModesPopup.close()
                        }
                    }
                }
            }
        }
    }

    Shortcut {
        sequence: "Alt+1"
        context: Qt.ApplicationShortcut
        onActivated: viewportModel.SelectLit()
    }
    Shortcut {
        sequence: "Alt+2"
        context: Qt.ApplicationShortcut
        onActivated: viewportModel.SelectPathTracer()
    }
    Shortcut {
        sequence: "Alt+5"
        context: Qt.ApplicationShortcut
        onActivated: viewportModel.SelectReflectionHeatmap()
    }
}
