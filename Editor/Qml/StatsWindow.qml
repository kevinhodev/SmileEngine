pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Painel flutuante de telemetria. Os passes GPU chegam agrupados pelo FGpuProfiler:
// cada linha principal e um escopo de frame e seus filhos sao timestamps de subpasses.
Rectangle {
    id: root
    required property var viewportModel
    required property var statsWindow

    color: C.Theme.bg
    border.color: C.Theme.windowBorder
    border.width: 1
    implicitWidth: 880
    implicitHeight: 760

    readonly property color cardBg: C.Theme.cardBg
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color green: C.Theme.green
    readonly property color amber: C.Theme.amber
    readonly property color warn: C.Theme.warn
    property var expandedPasses: ({})
    property var gpuTimingSnapshot: []
    property bool passInteractionActive: false

    function formatMs(value) {
        return Number(value).toFixed(2).replace(".", ",") + " ms"
    }

    function passExpanded(name, index) {
        if (expandedPasses[name] !== undefined)
            return expandedPasses[name]
        return index < 4
    }

    function setPassExpanded(name, expanded) {
        var next = {}
        for (var key in expandedPasses)
            next[key] = expandedPasses[key]
        next[name] = expanded
        expandedPasses = next
    }

    function refreshGpuTimingSnapshot() {
        if (!passInteractionActive)
            gpuTimingSnapshot = viewportModel.gpuTimings
    }

    Component.onCompleted: refreshGpuTimingSnapshot()

    // Os valores do profiler ja usam EMA. Um snapshot local a 10 Hz mantem a leitura fluida sem
    // reconstruir os delegates a cada frame — importante para hover, clique e animacao.
    // Esta arvore tem cadencia propria: nao depende do sinal amplo das demais telemetrias.
    Timer {
        interval: 100
        repeat: true
        running: true
        onTriggered: root.refreshGpuTimingSnapshot()
    }

    function passAccent(name, asyncPass) {
        if (asyncPass)
            return green
        if (name.indexOf("NRD") >= 0 || name.indexOf("DLSS") >= 0)
            return blue
        if (name.indexOf("Sombra") >= 0 || name.indexOf("Z-") >= 0)
            return amber
        if (name.indexOf("ReSTIR") >= 0 || name.indexOf("Reflex") >= 0)
            return "#b48cff"
        return green
    }

    component WindowButton: C.WindowButton {}

    component Card: Rectangle {
        radius: 9
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1
    }

    component MicroBar: Rectangle {
        property real value: 0
        property color fillColor: root.green
        height: 5
        radius: 2.5
        color: C.Theme.controlBg
        Rectangle {
            width: parent.width * Math.max(0, Math.min(1, parent.value))
            height: parent.height
            radius: parent.radius
            color: parent.fillColor
            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        }
    }

    // Barra de titulo: arraste do sistema + controles da janela sem moldura.
    Item {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 48

        Rectangle {
            x: 18
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            radius: 3
            color: "#24344d"
            border.color: root.blue
            border.width: 1

            Rectangle {
                x: 2.25; y: 5
                width: 1.5; height: 4
                radius: 0.75
                color: "#79adff"
            }
            Rectangle {
                x: 5.25; y: 2
                width: 1.5; height: 7
                radius: 0.75
                color: "#79adff"
            }
            Rectangle {
                x: 8.25; y: 7
                width: 1.5; height: 2
                radius: 0.75
                color: "#79adff"
            }
        }

        Text {
            id: titleText
            x: 42
            anchors.verticalCenter: parent.verticalCenter
            text: "Mini Profiler"
            color: root.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }

        Text {
            anchors.right: windowButtons.left
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(250, implicitWidth)
            text: viewportModel.gpuName
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }

        MouseArea {
            anchors.left: parent.left
            anchors.right: windowButtons.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            property bool moving: false
            onPressed: moving = false
            onPositionChanged: {
                if (pressed && !moving) {
                    moving = true
                    statsWindow.startSystemMove()
                }
            }
            onReleased: moving = false
        }

        Row {
            id: windowButtons
            anchors.right: parent.right
            anchors.top: parent.top
            WindowButton { glyph: "−"; onTapped: statsWindow.minimize() }
            WindowButton { glyph: "×"; danger: true; onTapped: statsWindow.close() }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.divider
        }
    }

    Flickable {
        id: scroll
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 14
        anchors.topMargin: 12
        anchors.rightMargin: 6
        readonly property int scrollGutter: 16
        contentWidth: width - scrollGutter
        contentHeight: contentCol.implicitHeight + 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {
            // Fica no gutter entre o card e a borda da janela, sem cobrir o conteudo.
            width: thickness
        }

        Column {
            id: contentCol
            width: scroll.width - scroll.scrollGutter
            spacing: 10

            // Resumo de frame e memoria ficam lado a lado para leitura imediata.
            Row {
                width: parent.width
                height: 142
                spacing: 10

                Card {
                    width: Math.floor((parent.width - parent.spacing) * 0.39)
                    height: parent.height

                    Text {
                        x: 16; y: 14
                        text: "FRAME GPU"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.1
                    }
                    Text {
                        x: 16; y: 37
                        text: viewportModel.gpuFrameText
                        color: root.textPrimary
                        font.family: C.Theme.fontMono
                        font.pixelSize: 26
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        y: 46
                        text: viewportModel.gpuFrameMs > 0
                              ? (1000.0 / viewportModel.gpuFrameMs).toFixed(0) + " FPS GPU"
                              : "— FPS GPU"
                        color: viewportModel.gpuFrameMs <= 16.67 ? root.green : root.warn
                        font.family: C.Theme.fontMono
                        font.pixelSize: 11
                    }
                    MicroBar {
                        x: 16; y: 82
                        width: parent.width - 32
                        value: viewportModel.gpuFrameMs / 16.67
                        fillColor: viewportModel.gpuFrameMs <= 16.67 ? root.green : root.warn
                    }
                    Text {
                        x: 16; y: 99
                        text: "CPU  " + root.formatMs(viewportModel.frameTimeMs)
                        color: root.textSecondary
                        font.family: C.Theme.fontMono
                        font.pixelSize: 10
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        y: 99
                        text: viewportModel.gpuFrameMs <= 16.67
                              ? "+" + root.formatMs(16.67 - viewportModel.gpuFrameMs) + " livres"
                              : root.formatMs(viewportModel.gpuFrameMs - 16.67) + " acima"
                        color: viewportModel.gpuFrameMs <= 16.67 ? root.textMuted : root.warn
                        font.family: C.Theme.fontMono
                        font.pixelSize: 10
                    }
                }

                Card {
                    width: parent.width - x
                    height: parent.height

                    Text {
                        x: 16; y: 14
                        text: "VRAM DO PROCESSO"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.1
                    }
                    Text {
                        x: 16; y: 37
                        text: viewportModel.vramUsageText
                        color: viewportModel.vramOverBudget ? root.warn : root.blue
                        font.family: C.Theme.fontMono
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        y: 18
                        width: budgetLabel.implicitWidth + 16
                        height: 22
                        radius: 11
                        color: viewportModel.vramOverBudget ? "#36251d" : C.Theme.blueBg
                        border.color: viewportModel.vramOverBudget ? root.warn : C.Theme.blueBorder
                        Text {
                            id: budgetLabel
                            anchors.centerIn: parent
                            text: Math.round(viewportModel.vramBudgetFrac * 100) + "% DO BUDGET"
                            color: viewportModel.vramOverBudget ? root.warn : root.blue
                            font.family: C.Theme.fontMono
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                    }
                    MicroBar {
                        x: 16; y: 82
                        width: parent.width - 32
                        value: viewportModel.vramBudgetFrac
                        fillColor: viewportModel.vramOverBudget || viewportModel.vramBudgetFrac > 0.85
                                   ? root.warn : root.blue
                    }
                    Text {
                        x: 16; y: 101
                        width: parent.width - 32
                        text: viewportModel.vramOverBudget
                              ? "Acima do budget do OS — possível paginação para RAM"
                              : "Budget dinâmico do OS  ·  GPU com " + viewportModel.vramText + " dedicados"
                        elide: Text.ElideRight
                        color: viewportModel.vramOverBudget ? root.warn : root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                }
            }

            Row {
                id: mainRow
                width: parent.width
                height: Math.max(gpuCard.height, sideColumn.implicitHeight)
                spacing: 10

                // O bloco principal prioriza tempo de GPU e expande os subpasses in-place.
                Card {
                    id: gpuCard
                    width: Math.floor((parent.width - parent.spacing) * 0.65)
                    height: Math.max(430, gpuRows.implicitHeight + 78)

                    Text {
                        x: 16; y: 15
                        text: "GPU POR PASSE"
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        x: 16; y: 35
                        text: "Clique para expandir  ·  EMA dos timestamps"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        y: 17
                        text: viewportModel.gpuFrameText
                        color: root.textSecondary
                        font.family: C.Theme.fontMono
                        font.pixelSize: 10
                    }

                    Column {
                        id: gpuRows
                        x: 12; y: 59
                        width: parent.width - 24
                        spacing: 3

                        Repeater {
                            model: root.gpuTimingSnapshot
                            delegate: Item {
                                id: passRow
                                required property var modelData
                                required property int index
                                readonly property bool expandable: passRow.modelData.hasChildren === true
                                property bool expanded: expandable && root.passExpanded(passRow.modelData.name,
                                                                                         passRow.index)
                                readonly property color accent: root.passAccent(passRow.modelData.name,
                                                                                 passRow.modelData.async === true)
                                width: gpuRows.width
                                height: 37 + (expanded ? childRows.implicitHeight + 7 : 0)

                                Behavior on height { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }

                                Rectangle {
                                    x: 0; y: 7
                                    width: 3; height: 20; radius: 1.5
                                    color: passRow.accent
                                }

                                Text {
                                    x: 11; y: 6
                                    width: 14
                                    visible: passRow.expandable
                                    text: passRow.expanded ? "−" : "+"
                                    color: root.textMuted
                                    font.family: C.Theme.fontMono
                                    font.pixelSize: 11
                                }
                                Text {
                                    x: passRow.expandable ? 28 : 11
                                    y: 5
                                    width: parent.width - x - 116
                                    text: passRow.modelData.name
                                    elide: Text.ElideRight
                                    color: root.textNormal
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 10
                                    font.weight: Font.Medium
                                }
                                Rectangle {
                                    x: passName.x + Math.min(passName.implicitWidth + 7, passName.width)
                                    y: 4
                                    width: asyncText.implicitWidth + 10
                                    height: 17
                                    radius: 8
                                    visible: passRow.modelData.async === true
                                    color: "#1d2a1c"
                                    border.color: "#35482f"
                                    Text {
                                        id: asyncText
                                        anchors.centerIn: parent
                                        text: "ASYNC"
                                        color: root.green
                                        font.family: C.Theme.fontFamily
                                        font.pixelSize: 7
                                        font.weight: Font.Bold
                                    }
                                }
                                Text {
                                    id: passName
                                    // Item auxiliar invisivel para medir a posicao do chip async.
                                    x: passRow.expandable ? 28 : 11
                                    width: parent.width - x - 116
                                    text: passRow.modelData.name
                                    visible: false
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 10
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 47
                                    y: 5
                                    text: passRow.modelData.text
                                    color: root.textPrimary
                                    font.family: C.Theme.fontMono
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    anchors.right: parent.right
                                    y: 5
                                    text: passRow.modelData.percentText
                                    color: root.textMuted
                                    font.family: C.Theme.fontMono
                                    font.pixelSize: 9
                                }
                                MicroBar {
                                    x: 11; y: 26
                                    width: parent.width - 11
                                    height: 3
                                    value: passRow.modelData.frac
                                    fillColor: passRow.accent
                                }

                                TapHandler {
                                    enabled: passRow.expandable
                                    acceptedButtons: Qt.LeftButton
                                    cursorShape: Qt.PointingHandCursor
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onPressedChanged: root.passInteractionActive = pressed
                                    onCanceled: root.passInteractionActive = false
                                    onTapped: {
                                        var nextExpanded = !passRow.expanded
                                        // Atualiza o delegate na hora e persiste por nome para
                                        // sobreviver ao refresh do QVariantList a cada frame.
                                        passRow.expanded = nextExpanded
                                        root.setPassExpanded(passRow.modelData.name, nextExpanded)
                                    }
                                }

                                Column {
                                    id: childRows
                                    x: 27; y: 35
                                    width: parent.width - x
                                    visible: passRow.expanded
                                    spacing: 1

                                    Repeater {
                                        model: passRow.modelData.children
                                        delegate: Item {
                                            id: childRow
                                            required property var modelData
                                            readonly property bool active: childRow.modelData.active !== false
                                            width: childRows.width
                                            height: 22

                                            Rectangle {
                                                x: 0; y: 0
                                                width: 1; height: parent.height
                                                color: childRow.active
                                                       ? Qt.rgba(passRow.accent.r,
                                                                 passRow.accent.g,
                                                                 passRow.accent.b, 0.35)
                                                       : "#34362f"
                                            }
                                            Rectangle {
                                                x: 0; y: 10
                                                width: 8; height: 1
                                                color: childRow.active
                                                       ? Qt.rgba(passRow.accent.r,
                                                                 passRow.accent.g,
                                                                 passRow.accent.b, 0.35)
                                                       : "#34362f"
                                            }
                                            Text {
                                                x: 14; y: 3
                                                width: parent.width - 117
                                                text: childRow.modelData.name
                                                elide: Text.ElideRight
                                                color: childRow.active ? root.textSecondary : "#686a61"
                                                font.family: C.Theme.fontFamily
                                                font.pixelSize: 9
                                                font.italic: !childRow.active
                                            }
                                            Text {
                                                anchors.right: parent.right
                                                anchors.rightMargin: 47
                                                y: 3
                                                text: childRow.modelData.text
                                                color: childRow.active ? root.textNormal : root.textMuted
                                                font.family: C.Theme.fontMono
                                                font.pixelSize: 9
                                            }
                                            Text {
                                                anchors.right: parent.right
                                                y: 3
                                                text: childRow.modelData.shareText
                                                color: childRow.active ? root.textMuted : "#73776b"
                                                font.family: C.Theme.fontMono
                                                font.pixelSize: 8
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            visible: root.gpuTimingSnapshot.length === 0
                            height: visible ? 42 : 0
                            text: "Coletando timestamps da GPU…"
                            color: root.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }
                }

                Column {
                    id: sideColumn
                    width: parent.width - x
                    spacing: 10

                    Card {
                        width: parent.width
                        height: Math.max(220, categoryRows.implicitHeight + 69)

                        Text {
                            x: 16; y: 15
                            text: "MEMÓRIA POR CATEGORIA"
                            color: root.textPrimary
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 16
                            y: 17
                            text: "% DO USO"
                            color: root.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 8
                        }

                        Column {
                            id: categoryRows
                            x: 16; y: 48
                            width: parent.width - 32
                            spacing: 8

                            Repeater {
                                model: viewportModel.vramBreakdown
                                delegate: Item {
                                    required property var modelData
                                    width: categoryRows.width
                                    height: 29

                                    Text {
                                        x: 0; y: 0
                                        width: parent.width - 85
                                        text: modelData.name
                                        elide: Text.ElideRight
                                        color: modelData.name === "Não rastreado"
                                               ? root.textMuted : root.textNormal
                                        font.family: C.Theme.fontFamily
                                        font.pixelSize: 9
                                    }
                                    Text {
                                        anchors.right: parent.right
                                        y: 0
                                        text: modelData.text
                                        color: root.textNormal
                                        font.family: C.Theme.fontMono
                                        font.pixelSize: 9
                                    }
                                    MicroBar {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        y: 20
                                        height: 3
                                        value: modelData.frac
                                        fillColor: modelData.name === "Não rastreado"
                                                   ? "#4b4b43" : root.blue
                                    }
                                }
                            }

                            Text {
                                visible: viewportModel.vramBreakdown.length === 0
                                height: visible ? 28 : 0
                                text: "Sem dados — carregue uma cena."
                                color: root.textMuted
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 9
                            }
                        }
                    }

                    Card {
                        width: parent.width
                        height: 200

                        Text {
                            x: 16; y: 15
                            text: "CONTEXTO DO FRAME"
                            color: root.textPrimary
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        Column {
                            x: 16; y: 46
                            width: parent.width - 32
                            spacing: 0

                            Repeater {
                                model: [
                                    { label: "Render interno", value: viewportModel.internalResolution },
                                    { label: "Saída", value: viewportModel.outputResolution },
                                    { label: "Draws visíveis", value: viewportModel.visibleDrawCount + " / " + viewportModel.totalDrawCount },
                                    { label: "Ocultados (HZB)", value: viewportModel.occludedDrawCount },
                                    { label: "RAM compartilhada", value: viewportModel.vramNonLocalText }
                                ]
                                delegate: Item {
                                    required property var modelData
                                    width: parent.width
                                    height: 27
                                    Text {
                                        anchors.left: parent.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.label
                                        color: root.textMuted
                                        font.family: C.Theme.fontFamily
                                        font.pixelSize: 9
                                    }
                                    Text {
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.value
                                        color: root.textNormal
                                        font.family: C.Theme.fontMono
                                        font.pixelSize: 9
                                    }
                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        height: 1
                                        color: root.divider
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
