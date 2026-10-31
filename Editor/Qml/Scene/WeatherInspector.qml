pragma ComponentBehavior: Bound

import QtQuick
import "../components" as C

// Conteudo contextual do clima. Identidade e estado principal vivem no InspectorPanel.
Column {
    id: root

    required property var model

    property int presetIndex: 0
    property bool precipitationExpanded: true
    property bool surfacesExpanded: false
    property bool environmentExpanded: false

    spacing: 6
    opacity: model.available ? 1 : 0.45

    function markCustom() {
        presetIndex = 0
    }

    function applyPreset(index) {
        presetIndex = index
        if (index === 1) {
            model.SetRainAmount(0.0)
        } else if (index === 2) {
            model.SetRainAmount(0.25); model.SetCurtainAmount(0.18)
            model.SetPuddleAmount(0.30); model.SetPuddleScale(14.0)
            model.SetRippleStrength(0.35); model.SetWetDarkening(0.40)
            model.SetRainOcclusionEnabled(true)
            model.SetDrivesSky(false)
        } else if (index === 3) {
            model.SetRainAmount(0.60); model.SetCurtainAmount(0.55)
            model.SetPuddleAmount(0.65); model.SetPuddleScale(8.0)
            model.SetRippleStrength(1.0); model.SetWetDarkening(0.70)
            model.SetRainOcclusionEnabled(true)
            model.SetDrivesSky(true)
        } else if (index === 4) {
            model.SetRainAmount(1.0); model.SetCurtainAmount(1.0)
            model.SetPuddleAmount(0.92); model.SetPuddleScale(5.0)
            model.SetRippleStrength(1.65); model.SetWetDarkening(0.90)
            model.SetRainOcclusionEnabled(true)
            model.SetDrivesSky(true)
        }
    }

    C.PresetRow {
        width: parent.width
        options: ["Personalizado", "Seco", "Garoa", "Chuva", "Tempestade"]
        currentIndex: root.presetIndex
        onActivated: index => root.applyPreset(index)
    }

    C.PrimarySliderRow {
        label: "Intensidade"
        valueText: Math.round(root.model.rainAmount * 100) + "%"
        from: 0; to: 1; stepSize: 0.01
        boundValue: root.model.rainAmount
        onMoved: value => { root.markCustom(); root.model.SetRainAmount(value) }
    }

    C.InspectorSectionHeader {
        label: "Precipitação"
        summary: root.precipitationExpanded ? "" : "cortina · partículas · oclusão"
        expanded: root.precipitationExpanded
        onToggled: root.precipitationExpanded = !root.precipitationExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.precipitationExpanded

        C.ScrubRow {
            label: "Cortina de gotas"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.curtainAmount
            defaultValue: 1.0
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetCurtainAmount(value) }
        }
        C.SwitchRow {
            label: "Oclusão"
            hint: "interiores secos"
            checked: root.model.rainOcclusion
            onToggled: {
                root.markCustom()
                root.model.SetRainOcclusionEnabled(!checked)
            }
        }
    }

    C.InspectorSectionHeader {
        label: "Superfícies molhadas"
        summary: root.surfacesExpanded ? "" : "poças · ondulação · escurecimento"
        expanded: root.surfacesExpanded
        onToggled: root.surfacesExpanded = !root.surfacesExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.surfacesExpanded

        C.ScrubRow {
            label: "Poças"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.puddleAmount
            defaultValue: 0.65
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetPuddleAmount(value) }
        }
        C.ScrubRow {
            label: "Escala das poças"
            from: 1; to: 64; stepSize: 1
            boundValue: root.model.puddleScale
            defaultValue: 8.0
            decimals: 0; suffix: " m"
            onMoved: value => { root.markCustom(); root.model.SetPuddleScale(value) }
        }
        C.ScrubRow {
            label: "Ondulação"
            from: 0; to: 2; stepSize: 0.01
            boundValue: root.model.rippleStrength
            defaultValue: 1.0
            decimals: 2
            onMoved: value => { root.markCustom(); root.model.SetRippleStrength(value) }
        }
        C.ScrubRow {
            label: "Escurecimento"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.wetDarkening
            defaultValue: 0.85
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetWetDarkening(value) }
        }
    }

    C.InspectorSectionHeader {
        label: "Integração com o ambiente"
        summary: root.environmentExpanded ? "" : root.model.drivesSky ? "céu acompanha" : "independente"
        expanded: root.environmentExpanded
        onToggled: root.environmentExpanded = !root.environmentExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.environmentExpanded

        C.SwitchRow {
            label: "Dirigir o céu"
            hint: "luz · fog · indireta"
            checked: root.model.drivesSky
            onToggled: {
                root.markCustom()
                root.model.SetDrivesSky(!checked)
            }
        }
        Text {
            width: parent.width
            text: "A intensidade da chuva reduz a radiância do céu, a luz-chave e o ambiente."
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textMeta
            wrapMode: Text.WordWrap
        }
    }
}
