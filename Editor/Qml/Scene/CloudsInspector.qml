pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../components" as C
import ".." as UI

// Inspector autoral das nuvens. A API chega por CloudsBridge; hoje ela adapta FRenderSettings,
// mas o QML nao depende desse detalhe e pode sobreviver à futura componentizacao da cena.
C.Card {
    id: root
    required property var model

    property int presetIndex: 0
    property bool shapeExpanded: true
    property bool layerExpanded: false
    property bool lightingExpanded: false
    property bool distributionExpanded: false

    title: "Nuvens Volumétricas"
    iconName: "cloud"
    contentSpacing: 8
    opacity: model.available ? 1.0 : 0.45

    function markCustom() { presetIndex = 0 }
    function applyPreset(index) {
        presetIndex = index
        if (index === 1) { // Cumulus medio
            model.SetCoverage(0.62); model.SetDensity(1.8); model.SetErosion(0.53)
            model.SetTypeBias(0.12); model.SetPeakVariation(0.36)
            model.SetAltitude(1.8, 3.2); model.SetPhaseG(0.77)
            model.SetPowder(0.60); model.SetAmbient(1.0); model.SetShadowStrength(0.70)
        } else if (index === 2) { // Camadas
            model.SetCoverage(0.78); model.SetDensity(1.2); model.SetErosion(0.24)
            model.SetTypeBias(-0.35); model.SetPeakVariation(0.12)
            model.SetAltitude(1.5, 1.1); model.SetPhaseG(0.68)
            model.SetPowder(0.35); model.SetAmbient(1.25); model.SetShadowStrength(0.45)
        } else if (index === 3) { // Tempestade
            model.SetCoverage(0.92); model.SetDensity(3.6); model.SetErosion(0.70)
            model.SetTypeBias(0.42); model.SetPeakVariation(0.82)
            model.SetAltitude(1.1, 5.8); model.SetPhaseG(0.72)
            model.SetPowder(0.78); model.SetAmbient(0.55); model.SetShadowStrength(0.92)
        } else if (index === 4) { // Cirrus
            model.SetCoverage(0.35); model.SetDensity(0.6); model.SetErosion(0.86)
            model.SetTypeBias(-0.10); model.SetPeakVariation(0.22)
            model.SetAltitude(6.0, 1.0); model.SetPhaseG(0.86)
            model.SetPowder(0.18); model.SetAmbient(1.45); model.SetShadowStrength(0.25)
        }
    }

    headerItem: [
        Row {
            spacing: 5
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: root.model.enabled ? "#61c894" : C.Theme.textMuted
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.model.enabled ? "Ativa" : "Desativada"
                color: root.model.enabled ? "#8fd6ae" : C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
    ]

    component SectionHeader: Item {
        id: section
        property string label
        property string summary
        property bool expanded: false
        signal toggled()
        width: parent.width
        height: 31

        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top
            height: 1; color: C.Theme.divider
        }
        UI.LucideIcon {
            x: 0; y: 10
            name: "chevron-right"; size: 11
            rotation: section.expanded ? 90 : 0
            color: C.Theme.textSecondary
            Behavior on rotation { NumberAnimation { duration: 100 } }
        }
        Text {
            x: 18; y: 8
            text: section.label
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
            font.weight: Font.DemiBold
        }
        Text {
            anchors.right: parent.right; y: 9
            width: parent.width * 0.48
            horizontalAlignment: Text.AlignRight
            text: section.summary
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 8
            elide: Text.ElideLeft
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: section.toggled() }
    }

    component ActionButton: Rectangle {
        id: button
        property string label
        property string iconName
        signal clicked()
        implicitWidth: buttonRow.implicitWidth + 18
        height: 26; radius: 6
        color: buttonHover.hovered ? C.Theme.controlHover : "#171912"
        border.color: C.Theme.controlBorder
        border.width: 1
        Row {
            id: buttonRow
            anchors.centerIn: parent
            spacing: 5
            UI.LucideIcon {
                visible: button.iconName !== ""
                anchors.verticalCenter: parent.verticalCenter
                name: button.iconName; size: 11
                color: C.Theme.textSecondary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: button.label
                color: C.Theme.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
        HoverHandler { id: buttonHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: button.clicked() }
    }

    Text {
        width: parent.width
        text: root.model.enabled
              ? "Componente de ambiente · alterações em tempo real"
              : "Selecione o olho na árvore para exibir as nuvens"
        color: C.Theme.textMuted
        font.family: C.Theme.fontFamily
        font.pixelSize: 9
        wrapMode: Text.WordWrap
    }

    Item {
        width: parent.width
        height: 29
        Text {
            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
            text: "Preset"
            color: C.Theme.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }
        ComboBox {
            id: presetCombo
            anchors.right: parent.right
            width: Math.min(182, parent.width * 0.62)
            height: 28
            model: ["Personalizado", "Cumulus médio", "Camadas", "Tempestade", "Cirrus"]
            currentIndex: root.presetIndex
            onActivated: index => root.applyPreset(index)
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
            contentItem: Text {
                leftPadding: 10; rightPadding: 24
                verticalAlignment: Text.AlignVCenter
                text: presetCombo.displayText
                color: C.Theme.textNormal
                font: presetCombo.font
                elide: Text.ElideRight
            }
            indicator: UI.LucideIcon {
                anchors.right: parent.right; anchors.rightMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"; size: 11
                color: C.Theme.textSecondary
            }
            background: Rectangle {
                radius: 6
                color: presetCombo.hovered ? C.Theme.controlHover : "#111209"
                border.color: presetCombo.activeFocus ? C.Theme.green : C.Theme.controlBorder
                border.width: 1
            }
            popup: Popup {
                y: presetCombo.height + 3
                width: presetCombo.width
                implicitHeight: contentItem.implicitHeight + 8
                padding: 4
                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: presetCombo.popup.visible ? presetCombo.delegateModel : null
                    currentIndex: presetCombo.highlightedIndex
                }
                background: Rectangle {
                    radius: 6; color: "#181a15"
                    border.color: C.Theme.controlBorder; border.width: 1
                }
            }
            delegate: ItemDelegate {
                id: presetDelegate
                required property int index
                required property var modelData
                width: presetCombo.width - 8; height: 26
                highlighted: presetCombo.highlightedIndex === index
                contentItem: Text {
                    text: presetDelegate.modelData
                    color: presetDelegate.highlighted ? C.Theme.textPrimary : C.Theme.textNormal
                    font.family: C.Theme.fontFamily; font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: presetDelegate.highlighted ? C.Theme.greenBg : "transparent"
                }
            }
        }
    }

    C.PrimarySliderRow {
        label: "Cobertura"
        valueText: Math.round(root.model.coverage * 100) + "%"
        from: 0; to: 1; stepSize: 0.01
        boundValue: root.model.coverage
        onMoved: value => { root.markCustom(); root.model.SetCoverage(value) }
    }
    C.PrimarySliderRow {
        label: "Densidade"
        valueText: root.model.density.toFixed(2).replace(".", ",")
        from: 0.1; to: 10; stepSize: 0.1
        boundValue: root.model.density
        onMoved: value => { root.markCustom(); root.model.SetDensity(value) }
    }

    SectionHeader {
        label: "Forma e detalhe"
        summary: root.shapeExpanded ? "" : "erosão · tipo · picos"
        expanded: root.shapeExpanded
        onToggled: root.shapeExpanded = !root.shapeExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.shapeExpanded
        C.ScrubRow {
            label: "Erosão"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.erosion
            defaultValue: 0.45
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetErosion(value) }
        }
        C.ScrubRow {
            label: "Variação dos picos"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.peakVariation
            defaultValue: 1.0
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetPeakVariation(value) }
        }
        C.BipolarSliderRow {
            label: "Tipo da nuvem"
            valueText: root.model.typeBias < -0.12 ? "Camadas"
                     : root.model.typeBias > 0.12 ? "Vertical" : "Equilibrado"
            minimumLabel: "Camadas"
            maximumLabel: "Vertical"
            centerLabel: "equilibrado"
            from: -0.5; to: 0.5; centerValue: 0
            stepSize: 0.01; boundValue: root.model.typeBias
            onMoved: value => { root.markCustom(); root.model.SetTypeBias(value) }
        }
    }

    SectionHeader {
        label: "Camada e movimento"
        summary: root.layerExpanded ? "" : root.model.bottomKm.toFixed(1) + "–"
                 + Number(root.model.bottomKm + root.model.thicknessKm).toFixed(1) + " km"
        expanded: root.layerExpanded
        onToggled: root.layerExpanded = !root.layerExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.layerExpanded
        C.PairedScrubRow {
            label: "Camada"
            firstCaption: "Base"
            secondCaption: "Espessura"
            firstValue: root.model.bottomKm
            secondValue: root.model.thicknessKm
            firstDefaultValue: 2.0
            secondDefaultValue: 3.0
            firstFrom: 0.5; firstTo: 8; firstStepSize: 0.1
            secondFrom: 0.5; secondTo: 8; secondStepSize: 0.1
            firstDecimals: 1; secondDecimals: 1
            firstSuffix: " km"; secondSuffix: " km"
            onFirstMoved: value => {
                root.markCustom()
                root.model.SetAltitude(value, root.model.thicknessKm)
            }
            onSecondMoved: value => {
                root.markCustom()
                root.model.SetAltitude(root.model.bottomKm, value)
            }
        }
        C.ScrubRow {
            label: "Velocidade do vento"
            from: 0; to: 0.05; stepSize: 0.001
            boundValue: root.model.windSpeed
            defaultValue: 0.01
            displayScale: 1000; decimals: 0; suffix: " m/s"
            onMoved: value => { root.markCustom(); root.model.SetWindSpeed(value) }
        }
    }

    SectionHeader {
        label: "Luz e atmosfera"
        summary: root.lightingExpanded ? "" : "fase · powder · sombras"
        expanded: root.lightingExpanded
        onToggled: root.lightingExpanded = !root.lightingExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.lightingExpanded
        C.ScrubRow {
            label: "Dispersão frontal"
            from: 0; to: 0.95; stepSize: 0.01
            boundValue: root.model.phaseG
            defaultValue: 0.8; decimals: 2
            onMoved: value => { root.markCustom(); root.model.SetPhaseG(value) }
        }
        C.ScrubRow {
            label: "Silver lining"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.powder
            defaultValue: 0.5
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetPowder(value) }
        }
        C.ScrubRow {
            label: "Luz ambiente"
            from: 0; to: 3; stepSize: 0.05
            boundValue: root.model.ambient
            defaultValue: 1.0
            prefix: "×"; decimals: 2
            onMoved: value => { root.markCustom(); root.model.SetAmbient(value) }
        }
        C.ToggleRow {
            label: "Sombras no mundo"
            checked: root.model.shadowsEnabled
            onToggled: root.model.SetShadowsEnabled(!checked)
        }
        C.ScrubRow {
            label: "Intensidade da sombra"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.shadowStrength
            defaultValue: 1.0
            displayScale: 100; decimals: 0; suffix: "%"
            interactive: root.model.shadowsEnabled
            onMoved: value => { root.markCustom(); root.model.SetShadowStrength(value) }
        }
    }

    SectionHeader {
        label: "Distribuição"
        summary: root.distributionExpanded ? ""
                 : root.model.weatherAuthored ? "weather map" : "seed " + root.model.weatherSeed
        expanded: root.distributionExpanded
        onToggled: root.distributionExpanded = !root.distributionExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.distributionExpanded
        C.StepperRow {
            label: "Células do padrão"
            from: 1; to: 8; stepSize: 1
            boundValue: root.model.weatherCells
            suffix: "×"
            interactive: !root.model.weatherAuthored
            onMoved: value => { root.markCustom(); root.model.SetWeatherCells(value) }
        }
        C.StepperRow {
            label: "Semente"
            from: 0; to: 9999; stepSize: 1
            boundValue: root.model.weatherSeed
            interactive: !root.model.weatherAuthored
            onMoved: value => { root.markCustom(); root.model.SetWeatherSeed(value) }
        }
        Flow {
            width: parent.width; spacing: 6
            ActionButton {
                label: "Randomizar"; iconName: "rotate-ccw"
                onClicked: { root.markCustom(); root.model.SetWeatherSeed(Math.floor(Math.random() * 10000)) }
            }
            ActionButton {
                label: "Carregar mapa"; iconName: "folder"
                onClicked: root.model.LoadWeatherTexture()
            }
            ActionButton {
                visible: root.model.weatherAuthored
                label: "Usar procedural"; iconName: "eraser"
                onClicked: root.model.ClearWeatherTexture()
            }
        }
        Text {
            width: parent.width
            text: root.model.weatherAuthored
                  ? "R = cobertura · G = tipo · B = altura do topo"
                  : "Distribuição procedural; trocar seed recompõe o weather map."
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }
    }
}
