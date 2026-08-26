pragma ComponentBehavior: Bound

import QtQuick
import "../components" as C

// Conteudo contextual do oceano. Identidade e estado principal vivem no InspectorPanel.
Column {
    id: root

    required property var model

    property int presetIndex: 0
    property bool spectrumExpanded: true
    property bool geometryExpanded: false

    spacing: 6
    opacity: model.available ? 1 : 0.45

    function markCustom() { presetIndex = 0 }

    function applyPreset(index) {
        presetIndex = index
        if (index === 1) {
            model.SetWindDirectionDegrees(35); model.SetWindSpeed(2.0)
            model.SetFetchKm(25); model.SetDepthM(60); model.SetSwell(0.12)
            model.SetWavesAmount(0.55); model.SetDisplacement(0.50); model.SetChoppy(0.65)
        } else if (index === 2) {
            model.SetWindDirectionDegrees(60); model.SetWindSpeed(6.0)
            model.SetFetchKm(80); model.SetDepthM(120); model.SetSwell(0.30)
            model.SetWavesAmount(1.15); model.SetDisplacement(0.90); model.SetChoppy(1.20)
        } else if (index === 3) {
            model.SetWindDirectionDegrees(75); model.SetWindSpeed(10.0)
            model.SetFetchKm(300); model.SetDepthM(800); model.SetSwell(0.55)
            model.SetWavesAmount(1.70); model.SetDisplacement(1.20); model.SetChoppy(1.60)
        } else if (index === 4) {
            model.SetWindDirectionDegrees(225); model.SetWindSpeed(22.0)
            model.SetFetchKm(700); model.SetDepthM(1500); model.SetSwell(0.85)
            model.SetWavesAmount(2.80); model.SetDisplacement(1.85); model.SetChoppy(2.40)
        }
    }

    C.PresetRow {
        width: parent.width
        options: ["Personalizado", "Água calma", "Brisa costeira", "Mar aberto", "Tempestade"]
        currentIndex: root.presetIndex
        onActivated: index => root.applyPreset(index)
    }

    C.PrimarySliderRow {
        label: "Velocidade do vento"
        valueText: root.model.windSpeed.toFixed(1).replace(".", ",") + " m/s"
        from: 0.1; to: 40; stepSize: 0.1
        boundValue: root.model.windSpeed
        onMoved: value => { root.markCustom(); root.model.SetWindSpeed(value) }
    }
    C.PrimarySliderRow {
        label: "Energia das ondas"
        valueText: "×" + root.model.wavesAmount.toFixed(2).replace(".", ",")
        from: 0; to: 4; stepSize: 0.05
        boundValue: root.model.wavesAmount
        onMoved: value => { root.markCustom(); root.model.SetWavesAmount(value) }
    }

    C.InspectorSectionHeader {
        label: "Espectro"
        summary: root.spectrumExpanded ? "" : "direção · fetch · profundidade · swell"
        expanded: root.spectrumExpanded
        onToggled: root.spectrumExpanded = !root.spectrumExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.spectrumExpanded

        C.ScrubRow {
            label: "Direção do vento"
            from: 0; to: 360; stepSize: 1
            boundValue: root.model.windDirectionDegrees
            defaultValue: 57
            decimals: 0; suffix: "°"
            onMoved: value => { root.markCustom(); root.model.SetWindDirectionDegrees(value) }
        }
        C.ScrubRow {
            label: "Fetch"
            from: 1; to: 1000; stepSize: 1
            boundValue: root.model.fetchKm
            defaultValue: 100
            decimals: 0; suffix: " km"
            onMoved: value => { root.markCustom(); root.model.SetFetchKm(value) }
        }
        C.ScrubRow {
            label: "Profundidade"
            from: 1; to: 5000; stepSize: 1
            boundValue: root.model.depthM
            defaultValue: 100
            decimals: 0; suffix: " m"
            onMoved: value => { root.markCustom(); root.model.SetDepthM(value) }
        }
        C.ScrubRow {
            label: "Swell"
            from: 0; to: 1; stepSize: 0.01
            boundValue: root.model.swell
            defaultValue: 0.25
            displayScale: 100; decimals: 0; suffix: "%"
            onMoved: value => { root.markCustom(); root.model.SetSwell(value) }
        }
    }

    C.InspectorSectionHeader {
        label: "Geometria FFT"
        summary: root.geometryExpanded ? "" : "deslocamento · cristas"
        expanded: root.geometryExpanded
        onToggled: root.geometryExpanded = !root.geometryExpanded
    }
    Column {
        width: parent.width
        spacing: 6
        visible: root.geometryExpanded

        C.ScrubRow {
            label: "Deslocamento"
            from: 0; to: 4; stepSize: 0.05
            boundValue: root.model.displacement
            defaultValue: 1.0
            prefix: "×"; decimals: 2
            onMoved: value => { root.markCustom(); root.model.SetDisplacement(value) }
        }
        C.ScrubRow {
            label: "Cristas horizontais"
            from: 0; to: 4; stepSize: 0.05
            boundValue: root.model.choppy
            defaultValue: 1.5
            prefix: "×"; decimals: 2
            onMoved: value => { root.markCustom(); root.model.SetChoppy(value) }
        }
    }
}
