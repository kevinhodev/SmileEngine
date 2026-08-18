pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../components" as C
import ".." as UI

C.Card {
    id: root

    required property var model

    property int presetIndex: 0
    property bool precipitationExpanded: true
    property bool surfacesExpanded: false
    property bool environmentExpanded: false

    title: "Clima"
    iconName: "cloud-rain"
    contentSpacing: 8
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

    headerItem: [
        Row {
            spacing: 5
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                radius: 3
                color: root.model.raining ? C.Theme.green : C.Theme.textMuted
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.model.raining ? "Chovendo" : "Seco"
                color: root.model.raining ? C.Theme.greenText : C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
    ]

    Text {
        width: parent.width
        text: root.model.raining
              ? Math.round(root.model.rainAmount * 100) + "% de chuva · alterações em tempo real"
              : "Tempo seco · a superfície molhada seca gradualmente"
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
            model: ["Personalizado", "Seco", "Garoa", "Chuva", "Tempestade"]
            currentIndex: root.presetIndex
            onActivated: index => root.applyPreset(index)
            font.family: C.Theme.fontFamily
            font.pixelSize: 10

            contentItem: Text {
                leftPadding: 10
                rightPadding: 24
                verticalAlignment: Text.AlignVCenter
                text: presetCombo.displayText
                color: C.Theme.textNormal
                font: presetCombo.font
                elide: Text.ElideRight
            }
            indicator: UI.LucideIcon {
                anchors.right: parent.right
                anchors.rightMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                size: 11
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
                    radius: 6
                    color: "#181a15"
                    border.color: C.Theme.controlBorder
                    border.width: 1
                }
            }
            delegate: ItemDelegate {
                id: presetDelegate
                required property int index
                required property var modelData
                width: presetCombo.width - 8
                height: 26
                highlighted: presetCombo.highlightedIndex === index
                contentItem: Text {
                    text: presetDelegate.modelData
                    color: presetDelegate.highlighted ? C.Theme.textPrimary : C.Theme.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
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
        C.ToggleRow {
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

        C.ToggleRow {
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
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }
    }
}
