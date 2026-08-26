pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Cockpit único para ReSTIR DI/GI, SHaRC/ReGIR e inspeção de RT. A janela consome snapshots
// pequenos da ReSTIRDebugBridge; nenhuma configuração é duplicada em QML.
Rectangle {
    id: root
    required property var restirModel
    required property var debugModel
    required property var restirWindow

    color: C.Theme.shell
    border.color: C.Theme.hairlineStrong
    border.width: 1
    implicitWidth: 1180
    implicitHeight: 780
    focus: true
    Keys.onEscapePressed: {
        if (targetFocusActive)
            clearTargetFocus()
    }

    property int page: 0
    property var overview: restirModel.overview
    property var di: restirModel.di
    property var gi: restirModel.gi
    property var transport: restirModel.transport
    property var inspector: restirModel.sceneInspector
    property var pixelDebug: restirModel.pixelDebug
    // Epsilon controls are edited from a stable snapshot. The 250 ms telemetry refresh must
    // not rebuild their delegates while a scrub gesture is in progress.
    property var epsilonRows: []

    readonly property var pages: [
        { label: "VISÃO GERAL", icon: "grid" },
        { label: "DIRETA · DI", icon: "lightbulb" },
        { label: "INDIRETA · GI", icon: "sparkles" },
        { label: "TRANSPORTE", icon: "waves" },
        { label: "RAIOS", icon: "angle" },
        { label: "VISUALIZAÇÃO", icon: "eye" }
    ]

    readonly property int selectedTargetCount: debugModel.debugSelection.length
    readonly property int targetGridColumns: selectedTargetCount === 0 ? 1
        : (debugModel.debugColumns > 0
           ? debugModel.debugColumns
           : Math.ceil(Math.sqrt(selectedTargetCount)))
    readonly property int targetGridRows:
        Math.max(1, Math.ceil(selectedTargetCount / targetGridColumns))
    property int focusedTargetIndex: -1
    readonly property int focusedTargetSlot: targetSlot(focusedTargetIndex) - 1
    readonly property bool targetFocusActive: focusedTargetSlot >= 0
    readonly property string focusedTargetName:
        focusedTargetIndex >= 0 &&
        focusedTargetIndex < debugModel.debugTargetNames.length
        ? debugModel.debugTargetNames[focusedTargetIndex] : ""
    readonly property var focusedLegend: legendForTarget(focusedTargetName)

    function targetSelected(index) {
        const selection = debugModel.debugSelection
        for (let i = 0; i < selection.length; ++i)
            if (Number(selection[i]) === index) return true
        return false
    }

    function targetSlot(index) {
        const selection = debugModel.debugSelection
        for (let i = 0; i < selection.length; ++i)
            if (Number(selection[i]) === index) return i + 1
        return 0
    }

    function focusTarget(index) {
        if (targetSelected(index))
            focusedTargetIndex = index
    }

    function clearTargetFocus() {
        focusedTargetIndex = -1
    }

    function focusedSourceRect() {
        if (!targetFocusActive)
            return Qt.rect(0, 0, 0, 0)

        const fullWidth = 1600
        const fullHeight = 900
        const tileWidth = fullWidth / targetGridColumns
        const tileHeight = fullHeight / targetGridRows
        const tileX = (focusedTargetSlot % targetGridColumns) * tileWidth
        const tileY = Math.floor(focusedTargetSlot / targetGridColumns) * tileHeight
        const targetAspect = 16 / 9
        const tileAspect = tileWidth / tileHeight

        if (tileAspect > targetAspect) {
            const contentWidth = tileHeight * targetAspect
            return Qt.rect(tileX + (tileWidth - contentWidth) * 0.5,
                           tileY, contentWidth, tileHeight)
        }

        const contentHeight = tileWidth / targetAspect
        return Qt.rect(tileX, tileY + (tileHeight - contentHeight) * 0.5,
                       tileWidth, contentHeight)
    }

    function legendForTarget(name) {
        const n = name.toLowerCase()
        if (n.indexOf("fonte do candidato") >= 0) {
            return [
                { label: "cache", color: "#6ced88" },
                { label: "DDGI", color: "#ffc25a" },
                { label: "ambiente", color: "#6cd2ff" },
                { label: "preto explícito", color: "#e05a5a" },
                { label: "céu", color: "#c2cad9" },
                { label: "sem superfície", color: "#3b3b41" },
                { label: "backface morto", color: "#9e5ab1" }
            ]
        }
        if (n.indexOf("timer") >= 0) {
            return [
                { label: "0 · sem trabalho", color: "#000000" },
                { label: "62,5k", color: "#0d26ff" },
                { label: "125k", color: "#1aff33" },
                { label: "187,5k", color: "#ffe60d" },
                { label: "≥250k ciclos", color: "#ff1405" }
            ]
        }
        if (n === "rt · bvh") {
            if (root.inspector.mode === 0) {
                return [
                    { label: "opaco", color: "#59d959" },
                    { label: "alpha-test / folhagem", color: "#ffd926" },
                    { label: "translúcido", color: "#408cff" },
                    { label: "dessaturado · two-sided", color: "#8f9ba8" },
                    { label: "miss", color: "#050608" }
                ]
            }
            if (root.inspector.mode === 1) {
                return [
                    { label: "cor única por InstanceID", color: "#9b70df" },
                    { label: "miss", color: "#050608" }
                ]
            }
            return [
                { label: "0", color: "#050608" },
                { label: "24", color: "#0d26ff" },
                { label: "48", color: "#1aff33" },
                { label: "72", color: "#ffe60d" },
                { label: "≥96 triângulos", color: "#ff1405" }
            ]
        }
        if (n.indexOf("radiance cache") >= 0) {
            if (root.transport.debugMode === 0) {
                return [
                    { label: "cor · chave da célula", color: "#5d8eff" },
                    { label: "escuro · sem amostra", color: "#111725" }
                ]
            }
            if (root.transport.debugMode === 1) {
                return [
                    { label: "utilizável", color: "#26d940" },
                    { label: "pede refresh", color: "#ff7314" },
                    { label: "aquecendo", color: "#338cf2" },
                    { label: "sem amostra", color: "#f2cc26" },
                    { label: "não preenchida", color: "#e62626" }
                ]
            }
            if (root.transport.debugMode === 2) {
                return [
                    { label: "LOD 0", color: "#2659ff" },
                    { label: "LOD 1", color: "#1ad9d9" },
                    { label: "LOD 2", color: "#33e640" },
                    { label: "LOD 3", color: "#f2e626" },
                    { label: "LOD 4", color: "#ff801a" },
                    { label: "LOD 5+", color: "#ff2626" }
                ]
            }
            if (root.transport.debugMode === 3) {
                return [
                    { label: "0 amostras", color: "#bf1a1a" },
                    { label: "64 · convergido", color: "#26d94d" },
                    { label: "ausente", color: "#0d0d0d" }
                ]
            }
            if (root.transport.debugMode === 4) {
                return [
                    { label: "recente", color: "#26d940" },
                    { label: "refresh · 8–15 frames", color: "#f2e626" },
                    { label: "stale · até 64", color: "#e62626" },
                    { label: "ausente", color: "#0d0d0d" }
                ]
            }
            return [
                { label: "abaixo do piso", color: "#bf1a1a" },
                { label: "≥" + Math.max(1, Number(root.transport.minSamples)) +
                         " amostras", color: "#26d94d" },
                { label: "ausente", color: "#0d0d0d" }
            ]
        }
        return []
    }

    function legendTitle(name) {
        const n = name.toLowerCase()
        if (n.indexOf("fonte do candidato") >= 0)
            return "FONTE DO CANDIDATO NOVO"
        if (n.indexOf("timer") >= 0)
            return "CICLOS GPU POR THREAD · ESCALA FIXA"
        if (n === "rt · bvh") {
            if (root.inspector.mode === 0) return "CATEGORIA DA INSTÂNCIA NA TLAS"
            if (root.inspector.mode === 1) return "HASH DO INSTANCE ID"
            return "TRIÂNGULOS TESTADOS POR RAIO · SATURA EM 96"
        }
        if (n.indexOf("radiance cache") >= 0) {
            return ["CHAVE E OCUPAÇÃO DAS CÉLULAS", "COBERTURA DA CONSULTA",
                    "NÍVEL ESPACIAL DA CÉLULA", "CONVERGÊNCIA · N / 64",
                    "IDADE DESDE A ÚLTIMA AMOSTRA", "CONFIANÇA CONTRA O PISO"][
                        Math.max(0, Math.min(5, Number(root.transport.debugMode)))]
        }
        return ""
    }

    function legendSummary(name) {
        const n = name.toLowerCase()
        if (n.indexOf("fonte do candidato") >= 0)
            return "cache · DDGI · ambiente · zero · céu"
        if (n.indexOf("timer") >= 0)
            return "0 → 250k+ ciclos por thread"
        if (n === "rt · bvh")
            return root.inspector.mode === 0 ? "opaco · alpha · translúcido"
                 : root.inspector.mode === 1 ? "cor por InstanceID"
                                             : "0 → 96+ triângulos / raio"
        if (n.indexOf("radiance cache") >= 0)
            return ["chave · escuro sem amostra", "hit · refresh · warmup · miss",
                    "LOD 0 → 5+", "0 → 64 amostras", "recente → stale",
                    "abaixo → acima do piso"][Number(root.transport.debugMode)]
        return ""
    }

    function targetRelevant(name) {
        const n = name.toLowerCase()
        return n.indexOf("restir") >= 0 ||
               n.indexOf("radiance cache") >= 0 ||
               n.indexOf("gi · fonte") >= 0 ||
               n === "rt · bvh"
    }

    function setTargetSelected(name, selected) {
        const names = debugModel.debugTargetNames
        const index = names.indexOf(name)
        if (index >= 0 && targetSelected(index) !== selected)
            debugModel.ToggleDebugSelection(index)
    }

    function selectCoreTargets() {
        clearTargetFocus()
        let wanted = ["ReSTIR DI", "ReSTIR GI", "ReSTIR GI · NRD",
                      "Radiance cache · ocupacao"]
        if (root.inspector.enabled)
            wanted.push("RT · BVH")
        if (root.inspector.rtTimerEnabled)
            wanted.push("RT · timer · ReSTIR GI")
        if (root.gi.sourceDebug)
            wanted.push("GI · fonte do candidato tracado")
        debugModel.ClearDebugSelection()
        const names = debugModel.debugTargetNames
        for (let i = 0; i < names.length; ++i)
            if (wanted.indexOf(names[i]) >= 0)
                debugModel.ToggleDebugSelection(i)
    }

    function sanitizeReSTIRTargets() {
        const names = debugModel.debugTargetNames
        const current = []
        const selection = debugModel.debugSelection
        for (let i = 0; i < selection.length; ++i)
            current.push(Number(selection[i]))

        let relevantCount = 0
        for (let i = 0; i < current.length; ++i) {
            const index = current[i]
            if (index >= 0 && index < names.length &&
                targetRelevant(names[index])) {
                ++relevantCount
            } else {
                debugModel.ToggleDebugSelection(index)
            }
        }
        if (relevantCount === 0)
            selectCoreTargets()
    }

    function stateColor(active, requested) {
        if (active) return C.Theme.green
        if (requested) return C.Theme.warn
        return C.Theme.textMuted
    }

    function fmt(value, decimals) {
        const n = Number(value)
        return isNaN(n) ? "—" : n.toFixed(decimals === undefined ? 1 : decimals).replace(".", ",")
    }

    function reloadEpsilonRows() {
        epsilonRows = restirModel.rayEpsilons
    }

    onPageChanged: {
        if (page !== 5)
            clearTargetFocus()
        if (page === 4)
            reloadEpsilonRows()
        if (page === 5)
            sanitizeReSTIRTargets()
        restirModel.SetVisualizationActive(page === 5)
    }

    Component.onCompleted: {
        reloadEpsilonRows()
        restirModel.SetVisualizationActive(page === 5)
    }
    Component.onDestruction: restirModel.SetVisualizationActive(false)

    Timer {
        interval: 250
        repeat: true
        running: root.visible
        onTriggered: root.restirModel.Refresh()
    }

    component ActionButton: Rectangle {
        id: action
        property string label
        property string iconName: ""
        property bool primary: false
        property bool interactive: true
        signal tapped()

        width: actionRow.implicitWidth + 22
        height: 28
        radius: C.Theme.radiusControl
        opacity: interactive ? 1.0 : 0.42
        color: primary ? C.Theme.accentBgStrong
                       : (actionHover.hovered ? C.Theme.surfaceRaised : C.Theme.surface)
        border.color: primary ? C.Theme.accentBorder : C.Theme.controlBorder
        border.width: 1

        Row {
            id: actionRow
            anchors.centerIn: parent
            spacing: 7
            LucideIcon {
                visible: action.iconName !== ""
                name: action.iconName
                size: 12
                color: action.primary ? C.Theme.accent : C.Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: action.label
                color: action.primary ? C.Theme.accentText : C.Theme.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
                font.weight: action.primary ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        HoverHandler { id: actionHover; cursorShape: Qt.PointingHandCursor; enabled: action.interactive }
        TapHandler { enabled: action.interactive; onTapped: action.tapped() }
    }

    component DebugField: Item {
        id: debugField
        property string label
        property string value
        property color valueColor: C.Theme.textSecondary

        height: 18
        Text {
            id: debugFieldLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 70
            text: debugField.label
            color: C.Theme.textMuted
            font.family: C.Theme.fontMono
            font.pixelSize: 8
            font.letterSpacing: 0.4
        }
        Text {
            anchors.left: debugFieldLabel.right
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: debugField.value
            color: debugField.valueColor
            elide: Text.ElideRight
            font.family: C.Theme.fontMono
            font.pixelSize: 9
        }
    }

    component StatusCell: Rectangle {
        id: status
        property string label
        property string value
        property string detail
        property color tone: C.Theme.textMuted

        height: 72
        radius: C.Theme.radiusCard
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1

        Rectangle {
            x: 12; y: 14
            width: 5; height: 5; radius: 2.5
            color: status.tone
        }
        Text {
            x: 26; y: 10
            text: status.label
            color: C.Theme.textMuted
            font.family: C.Theme.fontMono
            font.pixelSize: 9
            font.letterSpacing: 1.0
        }
        Text {
            x: 12; y: 29
            text: status.value
            color: C.Theme.textPrimary
            font.family: C.Theme.fontMono
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        Text {
            x: 12; y: 50
            width: parent.width - 24
            text: status.detail
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 9
            elide: Text.ElideRight
        }
    }

    component Metric: Item {
        id: metric
        property string label
        property string value
        property string detail: ""
        property color tone: C.Theme.textPrimary
        width: parent ? parent.width : 0
        height: detail === "" ? 42 : 54

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            text: metric.label
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }
        Text {
            anchors.right: parent.right
            anchors.top: parent.top
            text: metric.value
            color: metric.tone
            font.family: C.Theme.fontMono
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
        Text {
            visible: metric.detail !== ""
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 20
            text: metric.detail
            color: C.Theme.textMuted
            font.family: C.Theme.fontMono
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: C.Theme.hairline
        }
    }

    component Note: Rectangle {
        id: note
        property string title
        property string body
        property color tone: C.Theme.blue
        width: parent ? parent.width : 0
        height: noteColumn.implicitHeight + 22
        radius: C.Theme.radiusCard
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1
        Rectangle { x: 0; y: 0; width: 2; height: parent.height; color: note.tone }
        Column {
            id: noteColumn
            x: 14; y: 10
            width: parent.width - 28
            spacing: 5
            Text {
                text: note.title
                color: C.Theme.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: note.body
                color: C.Theme.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
                lineHeight: 1.3
                wrapMode: Text.WordWrap
            }
        }
    }

    C.FramelessTitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: 40
        windowBridge: root.restirWindow
        title: "ReSTIR Lab"
        subtitle: root.restirModel.available
            ? "reservoirs · transporte · inspeção de ray tracing"
            : "aguardando o renderer…"
        iconName: "sparkles"
        iconColor: C.Theme.blue
        showMaximize: true
    }

    Rectangle {
        id: rail
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 190
        color: C.Theme.shell
        border.color: C.Theme.hairline
        border.width: 0

        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: C.Theme.hairline
        }

        Column {
            x: 12; y: 14
            width: parent.width - 24
            spacing: 4

            Text {
                text: "DIAGNÓSTICO"
                color: C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 9
                font.letterSpacing: 1.4
                bottomPadding: 8
            }

            Repeater {
                model: root.pages
                delegate: Rectangle {
                    id: nav
                    required property int index
                    required property var modelData
                    width: parent.width
                    height: 34
                    radius: C.Theme.radiusControl
                    color: root.page === nav.index ? C.Theme.accentBg
                          : (navHover.hovered ? C.Theme.surfaceRaised : "transparent")
                    border.color: root.page === nav.index ? C.Theme.accentBorder : "transparent"
                    border.width: 1
                    LucideIcon {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        name: nav.modelData.icon
                        size: 13
                        color: root.page === nav.index ? C.Theme.accent : C.Theme.textMuted
                    }
                    Text {
                        x: 34
                        anchors.verticalCenter: parent.verticalCenter
                        text: nav.modelData.label
                        color: root.page === nav.index ? C.Theme.textPrimary : C.Theme.textSecondary
                        font.family: C.Theme.fontMono
                        font.pixelSize: 9
                        font.weight: root.page === nav.index ? Font.DemiBold : Font.Normal
                        font.letterSpacing: 0.7
                    }
                    HoverHandler { id: navHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.page = nav.index }
                }
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 6
            Text {
                text: "PIXEL DEBUG"
                color: C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 9
                font.letterSpacing: 1.2
            }
            Row {
                spacing: 7
                Rectangle {
                    width: 5; height: 5; radius: 2.5
                    color: root.restirModel.pixelDebugAvailable ? C.Theme.green : C.Theme.warn
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: root.restirModel.pixelDebugStatus
                    color: C.Theme.textSecondary
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                }
            }
        }
    }

    Item {
        id: body
        anchors.top: titleBar.bottom
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        Grid {
            id: statusGrid
            visible: root.page === 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 14
            columns: 4
            columnSpacing: 8
            rowSpacing: 8

            StatusCell {
                width: (statusGrid.width - 24) / 4
                label: "DIRETA · DI"
                value: root.overview.diStatus || "—"
                detail: root.overview.diResolution || "sem alvo"
                tone: root.stateColor(root.overview.diActive, root.overview.diRequested)
            }
            StatusCell {
                width: (statusGrid.width - 24) / 4
                label: "INDIRETA · GI"
                value: root.overview.giStatus || "—"
                detail: root.overview.giResolution || "sem alvo"
                tone: root.stateColor(root.overview.giActive, root.overview.giRequested)
            }
            StatusCell {
                width: (statusGrid.width - 24) / 4
                label: "REUSO · REGIR"
                value: root.overview.reGIRStatus || "—"
                detail: "hits secundários"
                tone: root.stateColor(root.overview.reGIRActive, root.overview.reGIRRequested)
            }
            StatusCell {
                width: (statusGrid.width - 24) / 4
                label: "MUNDO · SHARC"
                value: root.overview.cacheStatus || "—"
                detail: root.transport.warmup || "radiance cache"
                tone: root.overview.cacheActive ? C.Theme.green : C.Theme.textMuted
            }
        }

        Rectangle {
            visible: root.page === 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: statusGrid.bottom
            anchors.topMargin: 14
            height: 1
            color: C.Theme.hairline
        }

        Flickable {
            id: scroll
            anchors.top: root.page === 0 ? statusGrid.bottom : parent.top
            anchors.topMargin: root.page === 0 ? 24 : 14
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 18
            anchors.rightMargin: 7
            anchors.bottomMargin: 12
            contentWidth: width - 17
            contentHeight: pageLoader.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar { width: thickness }

            Loader {
                id: pageLoader
                width: scroll.contentWidth
                sourceComponent: root.page === 0 ? overviewPage
                               : root.page === 1 ? diPage
                               : root.page === 2 ? giPage
                               : root.page === 3 ? transportPage
                               : root.page === 4 ? raysPage
                               : targetsPage
            }
        }
    }

    Component {
        id: overviewPage
        Item {
            implicitHeight: overviewColumn.implicitHeight + 8
            Column {
                id: overviewColumn
                width: parent.width
                spacing: 14

                C.SectionHeader { label: "PIPELINE ATIVO"; trailing: "REQUESTED → EFFECTIVE"; firstInColumn: true }
                Note {
                    title: "Uma janela, uma fonte de verdade"
                    body: "Os controles abaixo escrevem no mesmo FRenderSettings usado por captura, MCP e frame. A bridge só monta snapshots de apresentação; fechar a janela não altera nem duplica estado."
                }

                Grid {
                    width: parent.width
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 12

                    Rectangle {
                        width: (parent.width - 12) / 2
                        height: quickDI.implicitHeight + 26
                        radius: C.Theme.radiusCard
                        color: C.Theme.surface
                        border.color: C.Theme.hairline
                        border.width: 1
                        Column {
                            id: quickDI
                            x: 14; y: 13; width: parent.width - 28; spacing: 5
                            C.SectionHeader { label: "RESTIR DI"; trailing: root.di.resolution || "—"; firstInColumn: true }
                            C.SwitchRow {
                                label: "Direta local por reservoir"
                                hint: root.di.ready ? "pipeline pronto" : "indisponível"
                                checked: root.di.enabled || false
                                interactive: root.di.ready || false
                                onToggled: root.restirModel.ToggleDI()
                            }
                            Metric {
                                label: "Candidatas efetivas"
                                value: (root.di.effectiveAnalyticCandidates || 0) + " + "
                                     + (root.di.effectiveMeshCandidates || 0)
                                detail: "analíticas + mesh lights"
                            }
                        }
                    }

                    Rectangle {
                        width: (parent.width - 12) / 2
                        height: quickGI.implicitHeight + 26
                        radius: C.Theme.radiusCard
                        color: C.Theme.surface
                        border.color: C.Theme.hairline
                        border.width: 1
                        Column {
                            id: quickGI
                            x: 14; y: 13; width: parent.width - 28; spacing: 5
                            C.SectionHeader { label: "RESTIR GI"; trailing: root.gi.resolution || "—"; firstInColumn: true }
                            C.SwitchRow {
                                label: "Produtor indireto"
                                hint: root.gi.halfResEffective ? "meia-res efetiva" : "resolução cheia"
                                checked: root.gi.enabled || false
                                interactive: root.gi.ready || false
                                onToggled: root.restirModel.ToggleGI()
                            }
                            C.SwitchRow {
                                label: "Reuso de visibilidade"
                                hint: "1 shadow ray"
                                checked: root.gi.visibility || false
                                interactive: root.gi.enabled || false
                                onToggled: root.restirModel.ToggleGIVisibility()
                            }
                        }
                    }
                }

                C.SectionHeader { label: "LEITURA RÁPIDA"; trailing: "LIVE · 4 HZ" }
                Grid {
                    width: parent.width
                    columns: 3
                    columnSpacing: 12
                    Metric {
                        width: (parent.width - 24) / 3
                        label: "Cache ocupado"
                        value: root.fmt(root.transport.occupancy, 1) + "%"
                    }
                    Metric {
                        width: (parent.width - 24) / 3
                        label: "Cache hit rate"
                        value: root.fmt(root.transport.hitRate, 1) + "%"
                        tone: Number(root.transport.hitRate) > 50 ? C.Theme.green : C.Theme.textPrimary
                    }
                    Metric {
                        width: (parent.width - 24) / 3
                        label: "Memória SHaRC"
                        value: root.fmt(root.transport.memoryMB, 1) + " MB"
                    }
                }
                Text {
                    width: parent.width
                    text: root.transport.summary || "Ative a telemetria do radiance cache para preencher o resumo."
                    color: C.Theme.textSecondary
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    lineHeight: 1.35
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    Component {
        id: diPage
        Item {
            implicitHeight: diColumn.implicitHeight + 8
            Column {
                id: diColumn
                width: parent.width
                spacing: 10
                C.SectionHeader { label: "RESTIR DI · DIRETA LOCAL"; trailing: root.di.resolution || "—"; firstInColumn: true }
                C.SwitchRow {
                    label: "Ativar ReSTIR DI"
                    hint: root.di.ready ? "substitui o loop raster local" : "pipeline indisponível"
                    checked: root.di.enabled || false
                    interactive: root.di.ready || false
                    onToggled: root.restirModel.ToggleDI()
                }
                Note {
                    title: "Requested e effective aparecem juntos"
                    body: "O orçamento pedido pode virar zero quando não existe luz do domínio ou a alias table ainda não está pronta. Mudar candidatas, pool ou visibilidade limpa o histórico para o A/B não misturar configurações."
                    tone: C.Theme.warn
                }
                C.SectionHeader { label: "PROPOSTAS INICIAIS"; trailing: "PEDIDO / EFETIVO" }
                C.StepperRow {
                    label: "Luzes analíticas · efetivo " + (root.di.effectiveAnalyticCandidates || 0)
                    from: 1; to: 64; stepSize: 1
                    boundValue: root.di.analyticCandidates || 1
                    interactive: root.di.enabled || false
                    onMoved: value => root.restirModel.SetDIAnalyticCandidates(value)
                }
                C.StepperRow {
                    label: "Mesh lights · efetivo " + (root.di.effectiveMeshCandidates || 0)
                    from: 0; to: 64; stepSize: 1
                    boundValue: root.di.meshCandidates || 0
                    interactive: root.di.enabled && root.di.meshLightsInPool
                    onMoved: value => root.restirModel.SetDIMeshCandidates(value)
                }
                C.SwitchRow {
                    label: "Publicar mesh lights no pool"
                    hint: (root.di.effectiveMeshLights || 0) + " triângulos efetivos"
                    checked: root.di.meshLightsInPool || false
                    interactive: root.di.enabled || false
                    onToggled: root.restirModel.ToggleDIMeshLights()
                }
                C.SectionHeader { label: "VISIBILIDADE E DENOISE" }
                C.SwitchRow {
                    label: "Visibilidade nas candidatas iniciais"
                    hint: "mais rays · menos amostra ocluída"
                    checked: root.di.initialVisibility || false
                    interactive: root.di.enabled || false
                    onToggled: root.restirModel.ToggleDIInitialVisibility()
                }
                C.SwitchRow {
                    label: "BRDF-ratio demodulation"
                    hint: "pós-RELAX"
                    checked: root.di.brdfRatio || false
                    interactive: root.di.enabled || false
                    onToggled: root.restirModel.ToggleDIBrdfRatio()
                }
                C.SwitchRow {
                    label: "Suporte compacto de mesh lights"
                    hint: "A/B de estrutura"
                    checked: root.di.meshCompactSupport || false
                    interactive: root.di.enabled || false
                    onToggled: root.restirModel.ToggleDIMeshCompactSupport()
                }
            }
        }
    }

    Component {
        id: giPage
        Item {
            implicitHeight: giColumn.implicitHeight + 8
            Column {
                id: giColumn
                width: parent.width
                spacing: 10
                C.SectionHeader { label: "RESTIR GI · INDIRETA"; trailing: root.gi.resolution || "—"; firstInColumn: true }
                Text {
                    text: "Estimador primário"
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    font.letterSpacing: 0.8
                }
                Row {
                    width: parent.width
                    spacing: 8
                    C.Chip { label: "ReSTIR + SHaRC"; active: root.gi.indirectPrimary === 0; onTapped: root.restirModel.SetIndirectPrimary(0) }
                    C.Chip { label: "DDGI"; active: root.gi.indirectPrimary === 1; onTapped: root.restirModel.SetIndirectPrimary(1) }
                    C.Chip { label: "Off"; active: root.gi.indirectPrimary === 2; onTapped: root.restirModel.SetIndirectPrimary(2) }
                }
                Text {
                    text: "Fallback do miss secundário"
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    font.letterSpacing: 0.8
                }
                Row {
                    spacing: 8
                    C.Chip { label: "DDGI"; active: root.gi.indirectFallback === 0; onTapped: root.restirModel.SetIndirectFallback(0) }
                    C.Chip { label: "Preto · medição"; active: root.gi.indirectFallback === 2; onTapped: root.restirModel.SetIndirectFallback(2) }
                }
                C.SwitchRow {
                    label: "Produtor ReSTIR GI"
                    hint: root.gi.ready ? "reservoirs disponíveis" : "pipeline indisponível"
                    checked: root.gi.enabled || false
                    interactive: root.gi.ready || false
                    onToggled: root.restirModel.ToggleGI()
                }
                C.SwitchRow {
                    label: "Meia resolução"
                    hint: root.gi.halfResEffective ? "efetiva" : "pedido"
                    checked: root.gi.halfRes || false
                    interactive: root.gi.enabled || false
                    onToggled: root.restirModel.ToggleGIHalfRes()
                }
                C.SectionHeader { label: "REUSO E VALIDAÇÃO" }
                C.SwitchRow {
                    label: "Reuso de visibilidade"
                    hint: "shadow ray de conexão"
                    checked: root.gi.visibility || false
                    interactive: root.gi.enabled || false
                    onToggled: root.restirModel.ToggleGIVisibility()
                }
                C.SwitchRow {
                    label: "Aceitar backface no gather"
                    hint: "A/B de geometria fina"
                    checked: root.gi.backfacePolicy || false
                    interactive: root.gi.enabled || false
                    onToggled: root.restirModel.ToggleGIBackfacePolicy()
                }
                C.SwitchRow {
                    label: "Cortar DDGI como terminador"
                    hint: "medição, não qualidade"
                    checked: root.gi.measureTerminatorOff || false
                    onToggled: root.restirModel.ToggleGIMeasureTerminatorOff()
                }
                Note {
                    title: "Históricos são invalidados por domínio"
                    body: "Backface e terminador alteram a distribuição transportada e reiniciam os acumuladores dependentes. Visibilidade atua no resolve e preserva o reservoir; a bridge não inventa resets adicionais."
                }
            }
        }
    }

    Component {
        id: transportPage
        Item {
            implicitHeight: transportColumn.implicitHeight + 8
            Column {
                id: transportColumn
                width: parent.width
                spacing: 10
                C.SectionHeader { label: "TRANSPORTE DE MUNDO"; trailing: root.transport.warmup || "—"; firstInColumn: true }
                C.SwitchRow {
                    label: "ReGIR nos hits secundários"
                    hint: "compartilhado com reflexões"
                    checked: root.transport.reGIR || false
                    onToggled: root.restirModel.ToggleReGIR()
                }
                C.SwitchRow {
                    label: "World radiance cache"
                    hint: "SHaRC"
                    checked: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCache()
                }
                C.SwitchRow {
                    label: "Consultar cache"
                    hint: "desligar mantém o produtor"
                    checked: root.transport.cacheQuery || false
                    interactive: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCacheQuery()
                }
                C.SwitchRow {
                    label: "Warmup automático"
                    hint: root.transport.warmup || "—"
                    checked: root.transport.autoWarmup || false
                    interactive: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCacheAutoWarmup()
                }
                C.SectionHeader { label: "ATUALIZAÇÃO" }
                C.SwitchRow {
                    label: "Produtor dedicado"
                    hint: "independe do gather"
                    checked: root.transport.dedicatedUpdate || false
                    interactive: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCacheDedicatedUpdate()
                }
                C.SwitchRow {
                    label: "Atualização compacta"
                    hint: "somente células ativas"
                    checked: root.transport.compactUpdate || false
                    interactive: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCacheCompactUpdate()
                }
                C.SwitchRow {
                    label: "Reusar terminal anterior"
                    hint: "multi-bounce temporal"
                    checked: root.transport.prevTerminal || false
                    interactive: root.transport.cacheEnabled || false
                    onToggled: root.restirModel.ToggleRadianceCachePrevTerminal()
                }
                C.ScrubRow {
                    label: "Fração atualizada"
                    from: 0.01; to: 1.0; stepSize: 0.01
                    boundValue: root.transport.updateFraction || 0.25
                    defaultValue: 0.25
                    decimals: 0; displayScale: 100; suffix: "%"
                    interactive: root.transport.cacheEnabled || false
                    onMoved: value => root.restirModel.SetRadianceCacheUpdateFraction(value)
                }
                C.StepperRow {
                    label: "Vértices por caminho"
                    from: 1; to: 16; stepSize: 1
                    boundValue: root.transport.maxVertices || 1
                    interactive: root.transport.cacheEnabled || false
                    onMoved: value => root.restirModel.SetRadianceCacheMaxVertices(value)
                }
                C.ScrubRow {
                    label: "Roughness mínima"
                    from: 0; to: 1; stepSize: 0.01
                    boundValue: root.transport.minRoughness || 0
                    decimals: 2
                    interactive: root.transport.cacheEnabled || false
                    onMoved: value => root.restirModel.SetRadianceCacheMinRoughness(value)
                }
                C.StepperRow {
                    label: "Amostras para confiança"
                    from: 1; to: 64; stepSize: 1
                    boundValue: root.transport.minSamples || 1
                    interactive: root.transport.cacheEnabled || false
                    onMoved: value => root.restirModel.SetRadianceCacheMinSamples(value)
                }
                C.ScrubRow {
                    label: "Célula-base"
                    from: 0.1; to: 8.0; stepSize: 0.1
                    boundValue: root.transport.cellSize || 1
                    decimals: 2; suffix: " m"
                    interactive: root.transport.cacheEnabled || false
                    onCommitted: value => root.restirModel.SetRadianceCacheCellSize(value)
                }
                C.ScrubRow {
                    label: "Distância de LOD"
                    from: 1; to: 256; stepSize: 1
                    boundValue: root.transport.lodDistance || 32
                    decimals: 0; suffix: " m"
                    interactive: root.transport.cacheEnabled || false
                    onCommitted: value => root.restirModel.SetRadianceCacheLodDistance(value)
                }
                C.SectionHeader { label: "INSTRUMENTAÇÃO"; trailing: root.fmt(root.transport.memoryMB, 1) + " MB" }
                C.SwitchRow {
                    label: "Estatísticas"
                    hint: "contadores-base"
                    checked: root.transport.stats || false
                    onToggled: root.restirModel.ToggleRadianceCacheStats()
                }
                C.SwitchRow {
                    label: "Detalhe de misses"
                    hint: "mais atomics"
                    checked: root.transport.statsDetail || false
                    interactive: root.transport.stats || false
                    onToggled: root.restirModel.ToggleRadianceCacheStatsDetail()
                }
                C.SwitchRow {
                    label: "Fonte dos terminais"
                    hint: "céu · cache · morto"
                    checked: root.transport.statsSource || false
                    interactive: root.transport.stats || false
                    onToggled: root.restirModel.ToggleRadianceCacheStatsSource()
                }
                Text {
                    width: parent.width
                    text: root.transport.sourceBreakdown || "Sem breakdown de fonte."
                    color: C.Theme.textSecondary
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    lineHeight: 1.35
                    wrapMode: Text.WordWrap
                }
                Text {
                    width: parent.width
                    text: root.transport.missBreakdown || "Sem detalhe de misses."
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    lineHeight: 1.35
                    wrapMode: Text.WordWrap
                }
                Row {
                    spacing: 8
                    ActionButton { label: "Resetar cache"; iconName: "rotate-ccw"; onTapped: root.restirModel.ResetRadianceCache() }
                }
            }
        }
    }

    Component {
        id: raysPage
        Item {
            implicitHeight: raysColumn.implicitHeight + 8
            Column {
                id: raysColumn
                width: parent.width
                spacing: 8
                C.SectionHeader { label: "CALIBRAÇÃO DE RAIOS"; trailing: "ALTERAR INVALIDA HISTÓRICO"; firstInColumn: true }
                Note {
                    title: "Perfil único para consumidores RT"
                    body: "Os valores alimentam ReSTIR DI/GI, reflexões e os demais consumidores do perfil compartilhado. Unidades são convertidas pela bridge; o renderer permanece em metros."
                    tone: C.Theme.warn
                }
                Repeater {
                    model: root.epsilonRows
                    delegate: Column {
                        id: epsDelegate
                        required property var modelData
                        width: parent.width
                        spacing: 2
                        C.ScrubRow {
                            width: parent.width
                            label: epsDelegate.modelData.label
                            from: epsDelegate.modelData.min
                            to: epsDelegate.modelData.max
                            stepSize: epsDelegate.modelData.decimals === 0 ? 1 : Math.pow(10, -epsDelegate.modelData.decimals)
                            boundValue: epsDelegate.modelData.value
                            defaultValue: Number.NaN
                            decimals: epsDelegate.modelData.decimals
                            suffix: epsDelegate.modelData.unit === "frames" ? " fr" : " " + epsDelegate.modelData.unit
                            onCommitted: value => {
                                root.restirModel.SetRayEpsilon(epsDelegate.modelData.key, value)
                                root.reloadEpsilonRows()
                            }
                        }
                        Text {
                            width: parent.width
                            text: epsDelegate.modelData.hint
                            color: C.Theme.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                            lineHeight: 1.25
                            wrapMode: Text.WordWrap
                            bottomPadding: 6
                        }
                    }
                }
                ActionButton {
                    label: "Restaurar perfil padrão"
                    iconName: "restore"
                    onTapped: {
                        root.restirModel.ResetRayEpsilons()
                        root.reloadEpsilonRows()
                    }
                }
            }
        }
    }

    Component {
        id: targetsPage
        Item {
            implicitHeight: Math.max(900, scroll.height - 8)

            Rectangle {
                id: visualizationToolbar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 140
                radius: C.Theme.radiusCard
                color: C.Theme.surface
                border.color: C.Theme.hairline
                border.width: 1

                C.SectionHeader {
                    id: visualizationHeader
                    x: 12
                    y: 8
                    width: parent.width - 24
                    label: "INSTRUMENTAÇÃO DA VISUALIZAÇÃO"
                    trailing: "PIXEL DEBUG · " +
                              (root.restirModel.pixelDebugAvailable ? "PRONTO" : "PENDENTE")
                    firstInColumn: true
                }

                Item {
                    id: bvhControls
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.top: visualizationHeader.bottom
                    anchors.topMargin: 3
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    width: 276

                    C.SwitchRow {
                        width: parent.width
                        label: "BVH / TLAS"
                        hint: root.inspector.available ? "RayQuery full-screen" : "indisponível"
                        checked: root.inspector.enabled || false
                        interactive: root.inspector.available || false
                        onToggled: {
                            const enabling = !(root.inspector.enabled || false)
                            root.restirModel.ToggleSceneInspector()
                            root.setTargetSelected("RT · BVH", enabling)
                        }
                    }
                    Text {
                        y: 34
                        text: "TIPO DE VISUALIZAÇÃO"
                        color: C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 8
                        font.letterSpacing: 0.9
                    }
                    Row {
                        y: 52
                        spacing: 5
                        Repeater {
                            model: ["Categoria", "Instância", "Complexidade"]
                            delegate: C.Chip {
                                required property int index
                                required property string modelData
                                label: modelData
                                active: root.inspector.mode === index
                                enabled: root.inspector.available && root.inspector.enabled
                                opacity: enabled ? 1.0 : 0.42
                                onTapped: root.restirModel.SetSceneInspectorMode(index)
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.left: bvhControls.right
                    anchors.leftMargin: 12
                    anchors.top: visualizationHeader.bottom
                    anchors.topMargin: 7
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 11
                    width: 1
                    color: C.Theme.hairline
                }

                Item {
                    id: traceControls
                    anchors.left: bvhControls.right
                    anchors.leftMargin: 26
                    anchors.top: bvhControls.top
                    anchors.bottom: bvhControls.bottom
                    width: 264

                    C.SwitchRow {
                        width: parent.width
                        label: "RT shader timer"
                        hint: "ReSTIR GI · NVAPI"
                        checked: root.inspector.rtTimerEnabled || false
                        interactive: root.inspector.rtTimerAvailable || false
                        onToggled: {
                            const enabling = !(root.inspector.rtTimerEnabled || false)
                            root.restirModel.ToggleRtShaderTimer()
                            root.setTargetSelected("RT · timer · ReSTIR GI", enabling)
                        }
                    }
                    C.SwitchRow {
                        y: 32
                        width: parent.width
                        label: "Fonte do candidato"
                        hint: "ReSTIR GI"
                        checked: root.gi.sourceDebug || false
                        interactive: root.gi.enabled || false
                        onToggled: {
                            const enabling = !(root.gi.sourceDebug || false)
                            root.restirModel.ToggleGISourceDebug()
                            root.setTargetSelected("GI · fonte do candidato tracado", enabling)
                        }
                    }
                    ActionButton {
                        y: 65
                        label: root.restirModel.pixelDebugArmed
                               ? "Cancelar pixel"
                               : root.restirModel.pixelDebugPending
                                 ? "Cancelar leitura" : "Capturar pixel"
                        iconName: root.restirModel.pixelDebugArmed ? "x"
                                  : root.restirModel.pixelDebugPending ? "loader"
                                    : "crosshair"
                        primary: !root.restirModel.pixelDebugArmed &&
                                 !root.restirModel.pixelDebugPending
                        interactive: root.restirModel.pixelDebugAvailable
                        onTapped: {
                            if (root.restirModel.pixelDebugArmed ||
                                root.restirModel.pixelDebugPending)
                                root.restirModel.ClearPixelDebug()
                            else
                                root.restirModel.ArmPixelDebug()
                        }
                    }
                }

                Rectangle {
                    anchors.left: traceControls.right
                    anchors.leftMargin: 12
                    anchors.top: visualizationHeader.bottom
                    anchors.topMargin: 7
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 11
                    width: 1
                    color: C.Theme.hairline
                }

                Item {
                    anchors.left: traceControls.right
                    anchors.leftMargin: 26
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: bvhControls.top
                    anchors.bottom: bvhControls.bottom

                    Text {
                        id: sharcModeLabel
                        text: "SHARC · TIPO DE VISUALIZAÇÃO"
                        color: C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 8
                        font.letterSpacing: 0.9
                    }
                    Flow {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: sharcModeLabel.bottom
                        anchors.topMargin: 9
                        spacing: 6
                        Repeater {
                            model: ["Células", "Cobertura", "Nível", "Convergência",
                                    "Idade", "Confiança"]
                            delegate: C.Chip {
                                required property int index
                                required property string modelData
                                label: modelData
                                active: root.transport.debugMode === index
                                enabled: root.transport.cacheEnabled || false
                                opacity: enabled ? 1.0 : 0.42
                                onTapped: {
                                    root.restirModel.SetRadianceCacheDebugMode(index)
                                    root.setTargetSelected("Radiance cache · ocupacao", true)
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: pixelDebugCard
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: visualizationToolbar.bottom
                anchors.topMargin: 10
                height: 176
                radius: C.Theme.radiusCard
                color: C.Theme.surface
                border.color: root.restirModel.pixelDebugArmed
                              ? C.Theme.accentBorder : C.Theme.hairline
                border.width: 1

                readonly property bool hasSnapshot:
                    (root.pixelDebug.ready || false)
                readonly property var surfaceData:
                    hasSnapshot ? root.pixelDebug.surface : ({})
                readonly property var diData:
                    hasSnapshot ? root.pixelDebug.di : ({})
                readonly property var giData:
                    hasSnapshot ? root.pixelDebug.gi : ({})

                C.SectionHeader {
                    id: pixelDebugHeader
                    x: 12
                    y: 8
                    width: parent.width - 24
                    label: "PIXEL DEBUG · SNAPSHOT DO FRAME"
                    trailing: root.restirModel.pixelDebugStatus.toUpperCase()
                    firstInColumn: true
                }

                Item {
                    id: pixelDebugSummary
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.top: pixelDebugHeader.bottom
                    anchors.topMargin: 8
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    width: 196

                    Text {
                        text: pixelDebugCard.hasSnapshot
                              ? "PIXEL " + root.pixelDebug.pixel : "PIXEL —"
                        color: pixelDebugCard.hasSnapshot
                               ? C.Theme.textPrimary : C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    Text {
                        y: 22
                        width: parent.width
                        text: pixelDebugCard.hasSnapshot
                              ? "render " + root.pixelDebug.renderPixel +
                                " · frame " + root.pixelDebug.frame
                              : "arme e clique no viewport"
                        color: C.Theme.textSecondary
                        elide: Text.ElideRight
                        font.family: C.Theme.fontMono
                        font.pixelSize: 8
                    }
                    ActionButton {
                        y: 49
                        label: root.restirModel.pixelDebugArmed
                               ? "Cancelar"
                               : root.restirModel.pixelDebugPending
                                 ? "Cancelar leitura" : "Capturar"
                        iconName: root.restirModel.pixelDebugArmed ? "x"
                                  : root.restirModel.pixelDebugPending ? "loader"
                                    : "crosshair"
                        primary: !root.restirModel.pixelDebugArmed &&
                                 !root.restirModel.pixelDebugPending
                        interactive: root.restirModel.pixelDebugAvailable
                        onTapped: {
                            if (root.restirModel.pixelDebugArmed ||
                                root.restirModel.pixelDebugPending)
                                root.restirModel.ClearPixelDebug()
                            else
                                root.restirModel.ArmPixelDebug()
                        }
                    }
                    ActionButton {
                        x: 100
                        y: 49
                        label: "Limpar"
                        iconName: "trash-2"
                        interactive: pixelDebugCard.hasSnapshot
                        onTapped: root.restirModel.ClearPixelDebug()
                    }
                    Text {
                        y: 86
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: root.restirModel.pixelDebugArmed
                              ? "O próximo clique esquerdo captura. Direito cancela."
                              : "Readback 1×1 assíncrono; não bloqueia a GPU."
                        color: root.restirModel.pixelDebugArmed
                               ? C.Theme.accent : C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 8
                    }
                }

                Rectangle {
                    anchors.left: pixelDebugSummary.right
                    anchors.leftMargin: 12
                    anchors.top: pixelDebugHeader.bottom
                    anchors.topMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    width: 1
                    color: C.Theme.hairline
                }

                Item {
                    id: pixelDebugSurface
                    anchors.left: pixelDebugSummary.right
                    anchors.leftMargin: 26
                    anchors.top: pixelDebugHeader.bottom
                    anchors.topMargin: 7
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 8
                    width: Math.max(210, (parent.width - 250) / 3)

                    Text {
                        text: "SUPERFÍCIE"
                        color: pixelDebugCard.surfaceData.valid
                               ? C.Theme.green : C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 9
                        font.letterSpacing: 0.8
                    }
                    Column {
                        y: 22
                        width: parent.width
                        DebugField { width: parent.width; label: "posição"; value: pixelDebugCard.surfaceData.worldPosition || "—" }
                        DebugField { width: parent.width; label: "normal"; value: pixelDebugCard.surfaceData.normal || "—" }
                        DebugField { width: parent.width; label: "albedo"; value: pixelDebugCard.surfaceData.baseColor || "—" }
                        DebugField { width: parent.width; label: "material"; value: pixelDebugCard.surfaceData.material || "—" }
                        DebugField { width: parent.width; label: "device z"; value: pixelDebugCard.surfaceData.depth || "—" }
                    }
                }

                Rectangle {
                    anchors.left: pixelDebugSurface.right
                    anchors.leftMargin: 10
                    anchors.top: pixelDebugHeader.bottom
                    anchors.topMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    width: 1
                    color: C.Theme.hairline
                }

                Item {
                    id: pixelDebugDI
                    anchors.left: pixelDebugSurface.right
                    anchors.leftMargin: 24
                    anchors.top: pixelDebugSurface.top
                    anchors.bottom: pixelDebugSurface.bottom
                    width: pixelDebugSurface.width

                    Text {
                        text: "ReSTIR DI"
                        color: pixelDebugCard.diData.available
                               ? (pixelDebugCard.diData.valid
                                  ? C.Theme.green : C.Theme.warn)
                               : C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 9
                        font.letterSpacing: 0.8
                    }
                    Column {
                        y: 22
                        width: parent.width
                        DebugField { width: parent.width; label: "saída"; value: pixelDebugCard.diData.available ? pixelDebugCard.diData.output : "não executou" }
                        DebugField { width: parent.width; label: "reservoir"; value: pixelDebugCard.diData.available ? pixelDebugCard.diData.reservoir : "—" }
                        DebugField { width: parent.width; label: "amostra"; value: pixelDebugCard.diData.available ? pixelDebugCard.diData.sample : "—" }
                        DebugField { width: parent.width; label: "n1"; value: pixelDebugCard.diData.available ? pixelDebugCard.diData.normal : "—" }
                    }
                }

                Rectangle {
                    anchors.left: pixelDebugDI.right
                    anchors.leftMargin: 10
                    anchors.top: pixelDebugHeader.bottom
                    anchors.topMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    width: 1
                    color: C.Theme.hairline
                }

                Item {
                    anchors.left: pixelDebugDI.right
                    anchors.leftMargin: 24
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: pixelDebugSurface.top
                    anchors.bottom: pixelDebugSurface.bottom

                    Text {
                        text: "ReSTIR GI" +
                              (pixelDebugCard.giData.halfRes ? " · HALF-RES" : "")
                        color: pixelDebugCard.giData.available
                               ? (pixelDebugCard.giData.valid
                                  ? C.Theme.green : C.Theme.warn)
                               : C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 9
                        font.letterSpacing: 0.8
                    }
                    Column {
                        y: 22
                        width: parent.width
                        DebugField { width: parent.width; label: "saída"; value: pixelDebugCard.giData.available ? pixelDebugCard.giData.output : "não executou" }
                        DebugField { width: parent.width; label: "reservoir"; value: pixelDebugCard.giData.available ? pixelDebugCard.giData.reservoir : "—" }
                        DebugField { width: parent.width; label: "domínio"; value: pixelDebugCard.giData.available ? pixelDebugCard.giData.tracePixel : "—" }
                        DebugField { width: parent.width; label: "x2"; value: pixelDebugCard.giData.available ? pixelDebugCard.giData.hit : "—" }
                        DebugField { width: parent.width; label: "Lo"; value: pixelDebugCard.giData.available ? pixelDebugCard.giData.radiance : "—" }
                    }
                }
            }

            Rectangle {
                id: targetSelector
                anchors.left: parent.left
                anchors.top: pixelDebugCard.bottom
                anchors.topMargin: 10
                anchors.bottom: parent.bottom
                width: 252
                radius: C.Theme.radiusCard
                color: C.Theme.surface
                border.color: C.Theme.hairline
                border.width: 1

                C.SectionHeader {
                    id: targetHeader
                    x: 12
                    y: 10
                    width: parent.width - 24
                    label: "TARGETS RESTIR"
                    trailing: root.selectedTargetCount + " ATIVOS"
                    firstInColumn: true
                }

                Row {
                    id: targetActions
                    x: 12
                    y: targetHeader.y + targetHeader.height + 6
                    spacing: 6
                    ActionButton {
                        label: "Núcleo ReSTIR"
                        iconName: "sparkles"
                        primary: true
                        onTapped: root.selectCoreTargets()
                    }
                }

                Flickable {
                    id: targetList
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.top: targetActions.bottom
                    anchors.topMargin: 9
                    anchors.bottom: targetControls.top
                    anchors.bottomMargin: 8
                    contentHeight: targetListColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar { width: thickness }

                    Column {
                        id: targetListColumn
                        width: targetList.width - 8
                        spacing: 3

                        Repeater {
                            model: root.debugModel.debugTargetNames
                            delegate: Rectangle {
                                id: targetRow
                                required property int index
                                required property string modelData
                                readonly property bool relevant:
                                    root.targetRelevant(modelData)
                                readonly property bool selected:
                                    root.targetSelected(index)
                                visible: relevant
                                width: targetListColumn.width
                                height: visible ? 31 : 0
                                radius: C.Theme.radiusControl
                                color: selected ? C.Theme.blueBg
                                                : (targetHover.hovered
                                                   ? C.Theme.surfaceRaised
                                                   : "transparent")
                                border.color: selected ? C.Theme.blueBorder : "transparent"
                                border.width: 1

                                Rectangle {
                                    x: 7
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 18
                                    height: 18
                                    radius: C.Theme.radiusChip
                                    color: targetRow.selected ? C.Theme.blue
                                                              : C.Theme.controlBg
                                    border.color: targetRow.selected ? C.Theme.blue
                                                                    : C.Theme.controlBorder
                                    Text {
                                        anchors.centerIn: parent
                                        text: targetRow.selected
                                              ? root.targetSlot(targetRow.index) : "+"
                                        color: targetRow.selected ? C.Theme.shell
                                                                  : C.Theme.textMuted
                                        font.family: C.Theme.fontMono
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                    }
                                }

                                Text {
                                    x: 33
                                    anchors.right: parent.right
                                    anchors.rightMargin: 7
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: targetRow.modelData
                                    elide: Text.ElideRight
                                    color: targetRow.selected ? C.Theme.textPrimary
                                                              : C.Theme.textNormal
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 9
                                }
                                HoverHandler {
                                    id: targetHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler {
                                    onTapped: root.debugModel.ToggleDebugSelection(
                                        targetRow.index)
                                }
                            }
                        }

                        Text {
                            visible: root.debugModel.debugTargetNames.length === 0
                            width: parent.width
                            topPadding: 20
                            text: "Nenhum target publicado.\nCarregue uma cena."
                            horizontalAlignment: Text.AlignHCenter
                            color: C.Theme.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }
                }

                Item {
                    id: targetControls
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    height: 102

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 1
                        color: C.Theme.hairline
                    }
                    C.ScrubRow {
                        y: 8
                        width: parent.width
                        label: "Exposição"
                        from: 0.1
                        to: 8.0
                        stepSize: 0.05
                        boundValue: root.debugModel.debugExposure
                        defaultValue: 1.0
                        decimals: 2
                        fieldRatio: 0.52
                        onMoved: value => root.debugModel.SetDebugExposure(value)
                    }
                    Text {
                        x: 0
                        y: 43
                        text: "COLUNAS"
                        color: C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 8
                        font.letterSpacing: 0.9
                    }
                    Row {
                        y: 60
                        spacing: 5
                        C.Chip {
                            label: "Auto"
                            active: root.debugModel.debugColumns === 0
                            onTapped: root.debugModel.SetDebugColumns(0)
                        }
                        Repeater {
                            model: [1, 2, 3, 4]
                            delegate: C.Chip {
                                required property int modelData
                                label: String(modelData)
                                active: root.debugModel.debugColumns === modelData
                                onTapped: root.debugModel.SetDebugColumns(modelData)
                            }
                        }
                    }
                    ActionButton {
                        anchors.right: parent.right
                        y: 57
                        label: "Limpar"
                        onTapped: root.debugModel.ClearDebugSelection()
                    }
                }
            }

            Rectangle {
                id: embeddedPreview
                anchors.left: targetSelector.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.top: pixelDebugCard.bottom
                anchors.topMargin: 10
                anchors.bottom: parent.bottom
                radius: C.Theme.radiusCard
                color: C.Theme.canvas
                border.color: C.Theme.hairline
                border.width: 1

                Text {
                    x: 14
                    y: 11
                    text: root.targetFocusActive
                          ? "TARGET AMPLIADO"
                          : root.selectedTargetCount === 0
                          ? "PREVIEW OFFSCREEN"
                          : "PREVIEW OFFSCREEN  ·  " + root.targetGridColumns +
                            " × " + root.targetGridRows
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: 9
                    font.letterSpacing: 1.0
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    y: 11
                    text: root.debugModel.debugPreviewReady
                          ? "READBACK PRONTO" : "AGUARDANDO GPU"
                    color: root.debugModel.debugPreviewReady
                           ? C.Theme.green : C.Theme.warn
                    font.family: C.Theme.fontMono
                    font.pixelSize: 8
                    font.letterSpacing: 0.8
                }

                Rectangle {
                    id: previewShell
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 34
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 12
                    radius: C.Theme.radiusControl
                    color: "#090a08"
                    border.color: C.Theme.controlBorder
                    border.width: 1
                    clip: true

                    Item {
                        id: embeddedCanvas
                        anchors.centerIn: parent
                        width: Math.max(1, Math.min(parent.width - 2,
                                                   (parent.height - 2) * 16 / 9))
                        height: width * 9 / 16
                        clip: true

                        Image {
                            id: embeddedPreviewImage
                            readonly property rect focusRect: root.focusedSourceRect()
                            x: root.targetFocusActive
                               ? -focusRect.x / Math.max(focusRect.width, 1) *
                                 embeddedCanvas.width : 0
                            y: root.targetFocusActive
                               ? -focusRect.y / Math.max(focusRect.height, 1) *
                                 embeddedCanvas.height : 0
                            width: root.targetFocusActive
                                   ? embeddedCanvas.width * 1600 /
                                     Math.max(focusRect.width, 1)
                                   : embeddedCanvas.width
                            height: root.targetFocusActive
                                    ? embeddedCanvas.height * 900 /
                                      Math.max(focusRect.height, 1)
                                    : embeddedCanvas.height
                            visible: root.selectedTargetCount > 0 &&
                                     root.debugModel.debugPreviewReady
                            source: visible
                                ? "image://debugtargetpreview/frame_" +
                                  root.debugModel.debugPreviewSeq
                                : ""
                            cache: false
                            asynchronous: false
                            fillMode: Image.Stretch
                            smooth: false
                        }

                        Repeater {
                            visible: !root.targetFocusActive
                            model: root.debugModel.debugSelection
                            delegate: Item {
                                id: targetOverlay
                                required property var modelData
                                required property int index
                                readonly property int targetIndex: Number(modelData)
                                readonly property string targetName:
                                    targetIndex >= 0 &&
                                    targetIndex < root.debugModel.debugTargetNames.length
                                    ? root.debugModel.debugTargetNames[targetIndex] : ""
                                readonly property var targetLegend:
                                    root.legendForTarget(targetName)
                                x: (index % root.targetGridColumns) *
                                   embeddedCanvas.width / root.targetGridColumns
                                y: Math.floor(index / root.targetGridColumns) *
                                   embeddedCanvas.height / root.targetGridRows
                                width: embeddedCanvas.width / root.targetGridColumns
                                height: embeddedCanvas.height / root.targetGridRows
                                visible: embeddedPreviewImage.visible &&
                                         !root.targetFocusActive

                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    border.color: targetFocusHover.hovered
                                                  ? C.Theme.blue : "#55ffffff"
                                    border.width: targetFocusHover.hovered ? 2 : 1
                                }
                                Rectangle {
                                    x: 6
                                    y: 6
                                    width: Math.min(overlayName.implicitWidth + 32,
                                                    parent.width - 12)
                                    height: 22
                                    radius: C.Theme.radiusChip
                                    color: "#d010110f"
                                    border.color: C.Theme.blueBorder
                                    Text {
                                        x: 7
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: (targetOverlay.index + 1) + "."
                                        color: C.Theme.blue
                                        font.family: C.Theme.fontMono
                                        font.pixelSize: 9
                                    }
                                    Text {
                                        id: overlayName
                                        x: 24
                                        anchors.right: parent.right
                                        anchors.rightMargin: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: targetOverlay.targetName
                                        elide: Text.ElideRight
                                        color: C.Theme.textPrimary
                                        font.family: C.Theme.fontFamily
                                        font.pixelSize: 9
                                    }
                                }
                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    y: 6
                                    width: 22
                                    height: 22
                                    radius: C.Theme.radiusChip
                                    color: targetFocusHover.hovered
                                           ? C.Theme.blueBg : "#b010110f"
                                    border.color: targetFocusHover.hovered
                                                  ? C.Theme.blueBorder : "#44ffffff"
                                    LucideIcon {
                                        anchors.centerIn: parent
                                        name: "maximize"
                                        size: 11
                                        color: targetFocusHover.hovered
                                               ? C.Theme.blue : C.Theme.textSecondary
                                    }
                                }
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 6
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 6
                                    height: 20
                                    visible: targetOverlay.targetLegend.length > 0
                                    radius: C.Theme.radiusChip
                                    color: "#c810110f"
                                    border.color: "#33ffffff"
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 7
                                        anchors.right: parent.right
                                        anchors.rightMargin: 7
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.legendSummary(targetOverlay.targetName)
                                        elide: Text.ElideRight
                                        color: C.Theme.textSecondary
                                        font.family: C.Theme.fontMono
                                        font.pixelSize: 8
                                    }
                                }
                                HoverHandler {
                                    id: targetFocusHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler {
                                    onTapped: root.focusTarget(targetOverlay.targetIndex)
                                }
                            }
                        }
                    }

                    Item {
                        anchors.fill: parent
                        visible: root.targetFocusActive &&
                                 embeddedPreviewImage.visible

                        ActionButton {
                            id: focusBack
                            x: 12
                            y: 12
                            label: "Grade"
                            iconName: "undo"
                            primary: true
                            onTapped: root.clearTargetFocus()
                        }
                        Rectangle {
                            anchors.left: focusBack.right
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            y: 12
                            height: 28
                            radius: C.Theme.radiusControl
                            color: "#d010110f"
                            border.color: C.Theme.blueBorder
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.focusedTargetName
                                elide: Text.ElideRight
                                color: C.Theme.textPrimary
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                            }
                        }

                        Rectangle {
                            id: focusedLegendPanel
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 12
                            height: focusedLegendFlow.implicitHeight + 35
                            visible: root.focusedLegend.length > 0
                            radius: C.Theme.radiusControl
                            color: "#e010110f"
                            border.color: C.Theme.hairlineStrong

                            Text {
                                id: focusedLegendTitle
                                x: 10
                                y: 7
                                text: root.legendTitle(root.focusedTargetName)
                                color: C.Theme.textMuted
                                font.family: C.Theme.fontMono
                                font.pixelSize: 8
                                font.letterSpacing: 0.9
                            }
                            Flow {
                                id: focusedLegendFlow
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.top: focusedLegendTitle.bottom
                                anchors.topMargin: 7
                                spacing: 6

                                Repeater {
                                    model: root.focusedLegend
                                    delegate: Rectangle {
                                        id: legendEntry
                                        required property var modelData
                                        width: legendEntryRow.implicitWidth + 14
                                        height: 22
                                        radius: C.Theme.radiusChip
                                        color: C.Theme.controlBg
                                        border.color: C.Theme.controlBorder

                                        Row {
                                            id: legendEntryRow
                                            anchors.centerIn: parent
                                            spacing: 6
                                            Rectangle {
                                                width: 9
                                                height: 9
                                                radius: 2
                                                color: legendEntry.modelData.color
                                                border.color: "#66ffffff"
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                            Text {
                                                text: legendEntry.modelData.label
                                                color: C.Theme.textSecondary
                                                font.family: C.Theme.fontMono
                                                font.pixelSize: 8
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Column {
                        anchors.centerIn: parent
                        spacing: 7
                        visible: root.selectedTargetCount === 0
                        LucideIcon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            name: "eye"
                            size: 24
                            color: C.Theme.blue
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Selecione um target ReSTIR"
                            color: C.Theme.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 12
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "A composição não altera o viewport principal."
                            color: C.Theme.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: root.selectedTargetCount > 0 &&
                                 !root.debugModel.debugPreviewReady
                        text: "Preparando preview…"
                        color: C.Theme.textMuted
                        font.family: C.Theme.fontMono
                        font.pixelSize: 10
                    }
                }
            }
        }
    }

}
