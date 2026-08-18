pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../components" as C
import ".." as UI

C.Card {
    id: root

    required property var model

    property int presetIndex: 0
    property bool spectrumExpanded: true
    property bool geometryExpanded: false

    title: "Oceano FFT"
    iconName: "waves"
    contentSpacing: 8
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

    headerItem: [
        Row {
            spacing: 5
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: root.model.enabled ? C.Theme.green : C.Theme.textMuted
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.model.enabled ? "Ativo" : "Oculto"
                color: root.model.enabled ? C.Theme.greenText : C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
    ]

    Text {
        width: parent.width
        text: root.model.enabled
              ? "Espectro físico em três cascatas · alterações em tempo real"
              : "Selecione o olho na árvore para exibir a superfície"
        color: C.Theme.textMuted
        font.family: C.Theme.fontFamily
        font.pixelSize: 9
        wrapMode: Text.WordWrap
    }

    Item {
        width: parent.width
        height: 29

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
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
            model: ["Personalizado", "Água calma", "Brisa costeira", "Mar aberto", "Tempestade"]
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
