import QtQuick
import QtQuick.Controls
import "components" as C

// Janela flutuante do visualizador de render targets.
// A imagem e uma composicao offscreen produzida pelo renderer; o viewport principal nao muda.
Rectangle {
    id: root
    required property var viewportModel
    required property var debugWindow

    color: "#10110f"
    border.color: "#2e2f28"
    border.width: 1
    implicitWidth: 1180
    implicitHeight: 720

    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color blueBg: C.Theme.blueBg
    readonly property color blueBorder: C.Theme.blueBorder
    readonly property color green: C.Theme.green

    property string filterText: ""
    readonly property int selectedCount: viewportModel.debugSelection.length
    readonly property int gridColumns: selectedCount === 0 ? 1
        : (viewportModel.debugColumns > 0
           ? viewportModel.debugColumns
           : Math.ceil(Math.sqrt(selectedCount)))
    readonly property int gridRows: Math.max(1, Math.ceil(selectedCount / gridColumns))
    readonly property bool hasSelectedDDGI: {
        const selection = viewportModel.debugSelection
        const names = viewportModel.debugTargetNames
        for (let i = 0; i < selection.length; ++i) {
            const index = Number(selection[i])
            if (index >= 0 && index < names.length &&
                names[index].toLowerCase().indexOf("ddgi") === 0)
                return true
        }
        return false
    }

    function isSelected(idx) {
        const selection = viewportModel.debugSelection
        for (let i = 0; i < selection.length; ++i)
            if (selection[i] === idx) return true
        return false
    }

    function slotOf(idx) {
        const selection = viewportModel.debugSelection
        for (let i = 0; i < selection.length; ++i)
            if (selection[i] === idx) return i + 1
        return 0
    }

    function matchesFilter(name) {
        return filterText === "" ||
               name.toLowerCase().indexOf(filterText.toLowerCase()) !== -1
    }

    component WindowButton: C.WindowButton {}
    component ProbeButton: Rectangle {
        property string label: ""
        signal tapped()
        width: 34
        height: 24
        radius: 5
        color: probeButtonHover.hovered ? "#242720" : "#181a15"
        border.color: probeButtonHover.hovered ? root.blueBorder : root.borderColor
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: probeButtonHover.hovered ? root.blue : root.textSecondary
            font.family: C.Theme.fontMono
            font.pixelSize: 10
        }
        HoverHandler { id: probeButtonHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: parent.tapped() }
    }

    Item {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 42

        Rectangle {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: 4
            color: root.blueBg
            border.color: root.blueBorder
            Text {
                anchors.centerIn: parent
                text: "▦"
                color: root.blue
                font.pixelSize: 12
            }
        }

        Text {
            id: titleText
            x: 44
            anchors.verticalCenter: parent.verticalCenter
            text: "Render targets"
            color: root.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.Medium
        }

        Text {
            anchors.left: titleText.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: root.selectedCount === 0
                  ? "nenhum target selecionado"
                  : root.selectedCount + (root.selectedCount === 1
                      ? " target exibido"
                      : " targets exibidos simultaneamente")
            color: root.selectedCount === 0 ? root.textMuted : root.green
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
                    debugWindow.startSystemMove()
                }
            }
            onReleased: moving = false
            onDoubleClicked: debugWindow.toggleMaximize()
        }

        Row {
            id: windowButtons
            anchors.right: parent.right
            anchors.top: parent.top
            WindowButton { glyph: "−"; onTapped: debugWindow.minimize() }
            WindowButton {
                glyph: debugWindow.maximized ? "❐" : "□"
                onTapped: debugWindow.toggleMaximize()
            }
            WindowButton { glyph: "×"; danger: true; onTapped: debugWindow.close() }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.divider
        }
    }

    Rectangle {
        id: sidebar
        anchors.left: parent.left
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        width: 296
        color: "#141511"

        Rectangle {
            id: searchBox
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.top: parent.top
            anchors.topMargin: 14
            height: 32
            radius: 6
            color: "#1a1b15"
            border.color: searchInput.activeFocus ? root.blueBorder : root.borderColor

            Text {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "⌕"
                color: root.textMuted
                font.pixelSize: 13
            }

            TextInput {
                id: searchInput
                anchors.left: parent.left
                anchors.leftMargin: 30
                anchors.right: clearFilter.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
                clip: true
                onTextChanged: root.filterText = text

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: searchInput.text === ""
                    text: "Filtrar targets..."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
            }

            Text {
                id: clearFilter
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                visible: searchInput.text !== ""
                text: "×"
                color: clearHover.hovered ? root.textPrimary : root.textMuted
                font.pixelSize: 14
                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: searchInput.text = "" }
            }
        }

        Flickable {
            id: listArea
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.top: searchBox.bottom
            anchors.topMargin: 10
            anchors.bottom: sidebarControls.top
            anchors.bottomMargin: 10
            contentHeight: listColumn.height
            clip: true

            Column {
                id: listColumn
                width: listArea.width
                spacing: 3

                Repeater {
                    model: viewportModel.debugTargetNames
                    delegate: Rectangle {
                        id: targetDelegate
                        required property string modelData
                        required property int index
                        readonly property bool selected: root.isSelected(index)
                        readonly property int slot: root.slotOf(index)

                        visible: root.matchesFilter(modelData)
                        height: visible ? 34 : 0
                        width: listColumn.width
                        radius: 6
                        color: selected ? root.blueBg
                                        : (targetHover.hovered ? "#1e1f19" : "transparent")
                        border.color: selected ? root.blueBorder : "transparent"

                        Rectangle {
                            id: slotBadge
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            radius: 5
                            color: targetDelegate.selected ? root.blue : "#23241d"
                            border.color: targetDelegate.selected ? root.blue : root.borderColor
                            Text {
                                anchors.centerIn: parent
                                text: targetDelegate.selected ? targetDelegate.slot : "+"
                                color: targetDelegate.selected ? "#10110f" : root.textMuted
                                font.family: C.Theme.fontFamily
                                font.pixelSize: targetDelegate.selected ? 11 : 13
                                font.weight: Font.DemiBold
                            }
                        }

                        Text {
                            anchors.left: slotBadge.right
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: targetDelegate.modelData
                            elide: Text.ElideRight
                            color: targetDelegate.selected ? root.textPrimary : root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 11
                        }

                        HoverHandler { id: targetHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: viewportModel.ToggleDebugSelection(targetDelegate.index)
                        }
                    }
                }

                Text {
                    width: listColumn.width
                    visible: viewportModel.debugTargetNames.length === 0
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: "Nenhum target publicado.\nCarregue uma cena."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
            }
        }

        Item {
            id: sidebarControls
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            height: 132

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: root.divider
            }

            Text {
                y: 14
                text: "Colunas"
                color: root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }

            Row {
                anchors.right: parent.right
                y: 10
                spacing: 4
                Repeater {
                    model: [
                        { label: "Auto", value: 0 },
                        { label: "1", value: 1 },
                        { label: "2", value: 2 },
                        { label: "3", value: 3 },
                        { label: "4", value: 4 }
                    ]
                    delegate: Rectangle {
                        id: columnDelegate
                        required property var modelData
                        readonly property bool current:
                            viewportModel.debugColumns === columnDelegate.modelData.value
                        width: modelData.label === "Auto" ? 38 : 24
                        height: 22
                        radius: 5
                        color: current ? root.blueBg
                                       : (columnHover.hovered ? "#23241d" : "transparent")
                        border.color: current ? root.blueBorder : root.borderColor
                        Text {
                            anchors.centerIn: parent
                            text: columnDelegate.modelData.label
                            color: columnDelegate.current ? root.blue : root.textSecondary
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                        }
                        HoverHandler { id: columnHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: viewportModel.SetDebugColumns(
                                columnDelegate.modelData.value)
                        }
                    }
                }
            }

            Text {
                y: 51
                text: "Exposição"
                color: root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }

            Text {
                anchors.right: parent.right
                y: 51
                text: viewportModel.debugExposure.toFixed(2).replace(".", ",") + "×"
                color: root.blue
                font.family: C.Theme.fontMono
                font.pixelSize: 11
            }

            Slider {
                id: exposureSlider
                anchors.left: parent.left
                anchors.right: parent.right
                y: 68
                height: 20
                from: 0.05
                to: 8.0
                value: viewportModel.debugExposure
                onMoved: viewportModel.SetDebugExposure(value)
            }

            Rectangle {
                id: clearButton
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: 106
                height: 26
                radius: 6
                enabled: root.selectedCount > 0
                opacity: enabled ? 1.0 : 0.42
                color: clearButtonHover.hovered && enabled ? "#23241d" : "transparent"
                border.color: root.borderColor
                Text {
                    anchors.centerIn: parent
                    text: "Limpar seleção"
                    color: root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }
                HoverHandler { id: clearButtonHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: if (clearButton.enabled) viewportModel.ClearDebugSelection()
                }
            }

            Text {
                anchors.left: clearButton.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.verticalCenter: clearButton.verticalCenter
                text: root.selectedCount + "/16 selecionados"
                horizontalAlignment: Text.AlignRight
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: root.divider
        }
    }

    Rectangle {
        id: previewPane
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        color: "#0c0d0b"

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.top: parent.top
            anchors.topMargin: 14
            visible: !viewportModel.debugProbeInspecting
            text: root.selectedCount === 0 ? "PREVIEW" :
                  "PREVIEW  •  " + root.gridColumns + " × " + root.gridRows
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
            font.letterSpacing: 1.2
        }

        Item {
            id: probeInspectorHeader
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.top: parent.top
            height: viewportModel.debugProbeContributors.length > 0 ? 248
                  : viewportModel.debugProbePointSummary !== "" ? 112 : 88
            visible: viewportModel.debugProbeInspecting

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.topMargin: 9
                text: "PROBE #" + viewportModel.debugProbeIndex + "  •  " +
                      viewportModel.debugProbeCoord
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.topMargin: 29
                text: viewportModel.debugProbeWorld + "  •  " + viewportModel.debugProbeGrid
                color: root.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 9
            }

            Text {
                anchors.left: parent.left
                anchors.right: pointDiagnosticButton.left
                anchors.rightMargin: 8
                anchors.top: parent.top
                anchors.topMargin: 48
                text: viewportModel.debugProbeDirection === ""
                      ? "passe o mouse sobre um oct-map para ler a direção"
                      : viewportModel.debugProbeDirection
                color: viewportModel.debugProbeDirection === "" ? root.textMuted : root.blue
                font.family: C.Theme.fontMono
                font.pixelSize: 9
            }

            Text {
                anchors.left: parent.left
                anchors.right: pointDiagnosticButton.left
                anchors.rightMargin: 8
                anchors.top: parent.top
                anchors.topMargin: 64
                visible: viewportModel.debugProbeDirection !== ""
                text: viewportModel.debugProbeSample === ""
                      ? "lendo valores do texel..."
                      : viewportModel.debugProbeSample
                color: viewportModel.debugProbeSample === "" ? root.textMuted : root.green
                font.family: C.Theme.fontMono
                font.pixelSize: 9
            }

            Row {
                anchors.right: backToAtlas.left
                anchors.rightMargin: 8
                anchors.top: parent.top
                anchors.topMargin: 9
                spacing: 4
                ProbeButton { label: "X−"; onTapped: viewportModel.StepDebugProbe(-1, 0, 0) }
                ProbeButton { label: "X+"; onTapped: viewportModel.StepDebugProbe( 1, 0, 0) }
                ProbeButton { label: "Y−"; onTapped: viewportModel.StepDebugProbe(0, -1, 0) }
                ProbeButton { label: "Y+"; onTapped: viewportModel.StepDebugProbe(0,  1, 0) }
                ProbeButton { label: "Z−"; onTapped: viewportModel.StepDebugProbe(0, 0, -1) }
                ProbeButton { label: "Z+"; onTapped: viewportModel.StepDebugProbe(0, 0,  1) }
            }

            Rectangle {
                id: backToAtlas
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 9
                width: 94
                height: 24
                radius: 5
                color: backHover.hovered ? root.blueBg : "#181a15"
                border.color: backHover.hovered ? root.blueBorder : root.borderColor
                Text {
                    anchors.centerIn: parent
                    text: "Voltar ao atlas"
                    color: backHover.hovered ? root.blue : root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 9
                }
                HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: viewportModel.ClearDebugProbeInspection() }
            }

            Rectangle {
                id: pointDiagnosticButton
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 44
                width: 132
                height: 25
                radius: 5
                color: viewportModel.debugProbePointPickArmed
                       ? "#302a18"
                       : pointDiagnosticHover.hovered ? root.blueBg : "#181a15"
                border.color: viewportModel.debugProbePointPickArmed
                              ? "#8f762d"
                              : pointDiagnosticHover.hovered
                                ? root.blueBorder : root.borderColor
                Text {
                    anchors.centerIn: parent
                    text: viewportModel.debugProbePointPickArmed
                          ? "Cancelar captura"
                          : "Diagnosticar ponto"
                    color: viewportModel.debugProbePointPickArmed
                           ? "#e7c65c"
                           : pointDiagnosticHover.hovered
                             ? root.blue : root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 9
                }
                HoverHandler {
                    id: pointDiagnosticHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: viewportModel.ArmDebugProbePointPick()
                }
            }

            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 91
                visible: viewportModel.debugProbePointSummary !== ""
                text: viewportModel.debugProbePointSummary
                elide: Text.ElideRight
                color: viewportModel.debugProbePointPickArmed
                       ? "#e7c65c" : root.textSecondary
                font.family: C.Theme.fontMono
                font.pixelSize: 10
            }

            Grid {
                id: contributorsGrid
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 112
                columns: 4
                columnSpacing: 6
                rowSpacing: 6
                visible: viewportModel.debugProbeContributors.length > 0

                Repeater {
                    model: viewportModel.debugProbeContributors

                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool contributorActive:
                            Boolean(modelData.active)
                        readonly property bool contributorRisk:
                            Boolean(modelData.risk)
                        readonly property bool contributorDominant:
                            Boolean(modelData.dominant)
                        readonly property bool contributorSelected:
                            viewportModel.debugProbeIndex ===
                            Number(modelData.probeIndex)
                        width: (contributorsGrid.width -
                                contributorsGrid.columnSpacing * 3) / 4
                        height: 60
                        radius: 5
                        opacity: contributorActive ? 1.0 : 0.58
                        color: contributorSelected ? "#262722"
                             : contributorRisk ? "#2c1c12"
                             : contributorDominant ? "#14271d"
                             : "#151713"
                        border.color: contributorSelected ? "#f1f1e8"
                                    : contributorRisk ? "#b8662e"
                                    : contributorDominant ? "#3e8f61"
                                    : root.borderColor
                        border.width: contributorSelected ? 2 : 1

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.top: parent.top
                            anchors.topMargin: 6
                            text: "#" + Number(parent.modelData.probeIndex) +
                                  "  " + parent.modelData.coord +
                                  (parent.contributorSelected ? "  S" : "") +
                                  (parent.contributorDominant ? "  D" : "") +
                                  (parent.contributorRisk ? "  R" : "")
                            elide: Text.ElideRight
                            color: parent.contributorRisk ? "#f0a261"
                                 : parent.contributorDominant ? root.green
                                 : root.textPrimary
                            font.family: C.Theme.fontMono
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.top: parent.top
                            anchors.topMargin: 25
                            text: parent.contributorActive
                                  ? "peso " +
                                    (Number(parent.modelData.weight) * 100).toFixed(1) +
                                    "%  ·  vis " +
                                    (Number(parent.modelData.visibility) * 100).toFixed(0) +
                                    "%"
                                  : "inativa · descartada"
                            color: root.textNormal
                            font.family: C.Theme.fontMono
                            font.pixelSize: 10
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.top: parent.top
                            anchors.topMargin: 43
                            text: "d " + Number(parent.modelData.distance).toFixed(1) +
                                  "  ·  μ " + Number(parent.modelData.mean).toFixed(1) +
                                  "  ·  risco " +
                                  (Number(parent.modelData.leakRisk) * 100).toFixed(1) + "%"
                            elide: Text.ElideRight
                            color: root.textSecondary
                            font.family: C.Theme.fontMono
                            font.pixelSize: 9
                        }

                        HoverHandler {
                            id: contributorHover
                            enabled: parent.contributorActive
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            enabled: parent.contributorActive
                            onTapped: viewportModel.SelectDebugProbeContributor(
                                Number(parent.modelData.probeIndex))
                        }
                    }
                }
            }
        }

        Rectangle {
            id: canvasShell
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.top: parent.top
            anchors.topMargin: viewportModel.debugProbeInspecting
                               ? probeInspectorHeader.height + 4 : 42
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 18
            radius: 7
            color: "#080907"
            border.color: root.borderColor
            clip: true

            Item {
                id: previewCanvas
                anchors.centerIn: parent
                width: Math.max(1, Math.min(parent.width - 2, (parent.height - 2) * 16 / 9))
                height: width * 9 / 16

                Image {
                    id: previewImage
                    anchors.fill: parent
                    visible: root.selectedCount > 0 && viewportModel.debugPreviewReady
                    source: visible
                        ? "image://debugtargetpreview/frame_" + viewportModel.debugPreviewSeq
                        : ""
                    cache: false
                    asynchronous: false
                    fillMode: Image.Stretch
                    smooth: !viewportModel.debugProbeInspecting
                }

                Repeater {
                    model: viewportModel.debugSelection
                    delegate: Item {
                        id: previewTile
                        required property var modelData
                        required property int index
                        readonly property int targetIndex: Number(modelData)
                        readonly property string targetName:
                            targetIndex >= 0 && targetIndex < viewportModel.debugTargetNames.length
                            ? viewportModel.debugTargetNames[targetIndex] : ""
                        readonly property bool ddgiTarget:
                            targetName.toLowerCase().indexOf("ddgi") === 0
                        readonly property bool distanceTarget:
                            targetName.toLowerCase().indexOf("dist") >= 0

                        x: (previewTile.index % root.gridColumns) *
                           previewCanvas.width / root.gridColumns
                        y: Math.floor(previewTile.index / root.gridColumns) *
                           previewCanvas.height / root.gridRows
                        width: previewCanvas.width / root.gridColumns
                        height: previewCanvas.height / root.gridRows
                        visible: previewImage.visible

                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            border.color: "#55ffffff"
                            border.width: 1
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 7
                            height: 24
                            width: Math.min(nameText.implicitWidth + 38, parent.width - 14)
                            radius: 5
                            color: "#cc10110f"
                            border.color: "#553f87ff"

                            Text {
                                x: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: (previewTile.index + 1) + "."
                                color: root.blue
                                font.family: C.Theme.fontMono
                                font.pixelSize: 10
                            }

                            Text {
                                id: nameText
                                x: 27
                                anchors.right: parent.right
                                anchors.rightMargin: 7
                                anchors.verticalCenter: parent.verticalCenter
                                text: previewTile.targetIndex >= 0 &&
                                      previewTile.targetIndex < viewportModel.debugTargetNames.length
                                      ? viewportModel.debugTargetNames[previewTile.targetIndex]
                                      : ""
                                elide: Text.ElideRight
                                color: root.textPrimary
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 10
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            height: previewTile.distanceTarget ? 28 : 19
                            radius: 4
                            color: "#c6080907"
                            visible: viewportModel.debugProbeInspecting && previewTile.ddgiTarget

                            Text {
                                anchors.centerIn: parent
                                visible: !previewTile.distanceTarget
                                text: "irradiância decodificada  •  interior 6 × 6  •  sem padding"
                                color: root.textSecondary
                                font.family: C.Theme.fontMono
                                font.pixelSize: 9
                            }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                visible: previewTile.distanceTarget
                                text: viewportModel.debugProbeDistanceRange
                                color: root.textSecondary
                                font.family: C.Theme.fontMono
                                font.pixelSize: 8
                            }

                            Rectangle {
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                visible: previewTile.distanceTarget
                                width: Math.min(150, parent.width * 0.34)
                                height: 7
                                radius: 2
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.00; color: "#0d26ff" }
                                    GradientStop { position: 0.25; color: "#00e5ff" }
                                    GradientStop { position: 0.50; color: "#19ff33" }
                                    GradientStop { position: 0.75; color: "#ffe50d" }
                                    GradientStop { position: 1.00; color: "#ff1405" }
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: viewportModel.debugProbeInspecting
                            cursorShape: !viewportModel.debugProbeInspecting &&
                                         previewTile.ddgiTarget
                                         ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: (mouse) => {
                                if (!viewportModel.debugProbeInspecting &&
                                    previewTile.ddgiTarget) {
                                    viewportModel.InspectDDGIProbe(
                                        previewTile.targetIndex,
                                        mouse.x / width, mouse.y / height,
                                        width / Math.max(height, 1))
                                }
                            }
                            onPositionChanged: (mouse) => {
                                if (!viewportModel.debugProbeInspecting ||
                                    !previewTile.ddgiTarget) return
                                const side = Math.min(width, height)
                                const ox = (width - side) * 0.5
                                const oy = (height - side) * 0.5
                                viewportModel.UpdateDebugProbeDirection(
                                    (mouse.x - ox) / side,
                                    (mouse.y - oy) / side)
                            }
                            onExited: {
                                if (viewportModel.debugProbeInspecting)
                                    viewportModel.UpdateDebugProbeDirection(-1, -1)
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    visible: !viewportModel.debugProbeInspecting &&
                             root.selectedCount > 0 && root.hasSelectedDDGI &&
                             previewImage.visible
                    height: 24
                    width: hintText.implicitWidth + 20
                    radius: 5
                    color: "#cc10110f"
                    border.color: root.borderColor
                    Text {
                        id: hintText
                        anchors.centerIn: parent
                        text: "Clique em uma célula de um atlas DDGI para inspecionar a probe"
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 8
                visible: root.selectedCount === 0
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "▦"
                    color: root.blue
                    font.pixelSize: 28
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Selecione os targets na lista"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Eles serão exibidos juntos aqui, sem alterar o viewport."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }
            }

            Text {
                anchors.centerIn: parent
                visible: root.selectedCount > 0 && !viewportModel.debugPreviewReady
                text: "Preparando preview..."
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
        }
    }
}
