pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import ".." as UI
import "../components" as C

// Propriedades da luz selecionada. Saiu do SceneOutlinerPanel na migracao para o split
// outliner|viewport|inspector do Signal Graphite; o contrato com o LightsBridge e o mesmo,
// so a linguagem visual mudou (secoes com hairline no lugar de cards).
Column {
    id: root
    required property var lightsModel

    spacing: 0

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }

    readonly property bool isSpot: lightsModel.lightType === 1

    // ---- Identidade ----
    C.SectionHeader { firstInColumn: true; label: "IDENTIFICAÇÃO" }

    Rectangle {
        width: parent.width
        height: C.Theme.controlHeight
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        border.color: nameInput.activeFocus ? C.Theme.accentBorder : C.Theme.hairline
        border.width: 1
        TextInput {
            id: nameInput
            anchors.fill: parent
            anchors.leftMargin: 10; anchors.rightMargin: 10
            verticalAlignment: TextInput.AlignVCenter
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
            selectByMouse: true
            selectionColor: C.Theme.accentBorder
            text: root.lightsModel.name
            onEditingFinished: { root.lightsModel.name = text; focus = false }
            Binding {
                target: nameInput; property: "text"
                value: root.lightsModel.name
                when: !nameInput.activeFocus
                restoreMode: Binding.RestoreBinding
            }
        }
    }

    // ---- Cor ----
    C.SectionHeader { label: "COR" }

    C.HsvColorPicker {
        width: parent.width
        fieldHeight: 108
        value: root.lightsModel.color
        presets: ["#ffffff", "#ffd9a0", "#ffb066", "#ff6a4d",
                  "#cfe0ff", "#8fd0ff", "#8dff9c", "#d08bff"]
        onPushed: value => root.lightsModel.color = value
    }

    // ---- Emissao ----
    C.SectionHeader { label: "EMISSÃO" }

    C.SliderRow {
        label: "Intensidade"
        valueText: root.fmt(root.lightsModel.intensity)
        from: 0; to: 150
        boundValue: root.lightsModel.intensity
        onMoved: v => root.lightsModel.intensity = v
    }
    C.SliderRow {
        label: "Raio de atenuação"
        valueText: root.fmt(root.lightsModel.radius) + " m"
        from: 0.5; to: 60
        boundValue: root.lightsModel.radius
        onMoved: v => root.lightsModel.radius = v
    }
    C.SliderRow {
        label: "Raio da fonte (área)"
        valueText: root.fmt(root.lightsModel.sourceRadius, 2) + " m"
        from: 0.01; to: 1.5
        boundValue: root.lightsModel.sourceRadius
        onMoved: v => root.lightsModel.sourceRadius = v
    }
    // Peso da luz SO no indireto. Serve para quando a luz existe apenas para representar uma
    // malha emissiva que, com ray tracing, ja ilumina sozinha.
    C.SliderRow {
        label: "Peso no indireto"
        valueText: root.lightsModel.rtWeight <= 0.001
                     ? "fora do RT" : root.fmt(root.lightsModel.rtWeight, 2) + "×"
        from: 0; to: 1
        boundValue: root.lightsModel.rtWeight
        onMoved: v => root.lightsModel.rtWeight = v
    }

    // ---- Cone (so spot) ----
    C.SectionHeader { visible: root.isSpot; label: "CONE" }

    C.SliderRow {
        visible: root.isSpot
        label: "Cone interno"
        valueText: Math.round(root.lightsModel.innerConeDeg) + "°"
        from: 0; to: 89
        boundValue: root.lightsModel.innerConeDeg
        onMoved: v => root.lightsModel.innerConeDeg = v
    }
    C.SliderRow {
        visible: root.isSpot
        label: "Cone externo"
        valueText: Math.round(root.lightsModel.outerConeDeg) + "°"
        from: 1; to: 89
        boundValue: root.lightsModel.outerConeDeg
        onMoved: v => root.lightsModel.outerConeDeg = v
    }
    C.SliderRow {
        visible: root.isSpot
        label: "Direção — azimute"
        valueText: Math.round(root.lightsModel.spotAzimuthDeg) + "°"
        from: 0; to: 360
        boundValue: root.lightsModel.spotAzimuthDeg
        onMoved: v => root.lightsModel.spotAzimuthDeg = v
    }
    C.SliderRow {
        visible: root.isSpot
        label: "Direção — inclinação"
        valueText: Math.round(root.lightsModel.spotElevationDeg) + "°"
        from: -90; to: 90
        boundValue: root.lightsModel.spotElevationDeg
        onMoved: v => root.lightsModel.spotElevationDeg = v
    }

    // ---- Transform ----
    C.SectionHeader { label: "TRANSFORM" }

    // A luz nao tem rotacao nem escala editaveis (spot orienta pelo azimute/inclinacao acima),
    // entao o trio de posicao vem sozinho, com o atalho de reposicionar na camera ao lado.
    Item {
        width: parent.width
        height: 48

        Text {
            id: posLabel
            anchors.left: parent.left
            anchors.top: parent.top
            text: "Posição"
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Item {
            anchors.left: posLabel.right
            anchors.leftMargin: 8
            anchors.verticalCenter: posLabel.verticalCenter
            width: 18; height: 18
            UI.LucideIcon {
                anchors.centerIn: parent
                name: "video"; size: 12
                color: camHover.hovered ? C.Theme.textPrimary : C.Theme.textMuted
            }
            HoverHandler { id: camHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.lightsModel.placeAtCamera() }
            ToolTip.visible: camHover.hovered
            ToolTip.delay: 600
            ToolTip.text: "Reposicionar na frente da câmera"
        }

        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 8
            readonly property real fieldWidth: (width - 16) / 3

            C.AxisField {
                width: parent.fieldWidth
                axis: "X"; axisColor: C.Theme.axisX
                value: root.lightsModel.posX
                onCommitted: (v) => root.lightsModel.posX = v
            }
            C.AxisField {
                width: parent.fieldWidth
                axis: "Y"; axisColor: C.Theme.axisY
                value: root.lightsModel.posY
                onCommitted: (v) => root.lightsModel.posY = v
            }
            C.AxisField {
                width: parent.fieldWidth
                axis: "Z"; axisColor: C.Theme.axisZ
                value: root.lightsModel.posZ
                onCommitted: (v) => root.lightsModel.posZ = v
            }
        }
    }

    // ---- Sombras ----
    C.SectionHeader {
        label: "SOMBRAS"
        // Orcamento por quadro do atlas: e a informacao que decide se vale ligar mais uma.
        trailing: root.isSpot ? "8 SPOTS/QUADRO" : "4 POINTS/QUADRO"
    }

    C.SwitchRow {
        width: parent.width
        label: "Projeta sombras"
        checked: root.lightsModel.castShadows
        onToggled: root.lightsModel.castShadows = !root.lightsModel.castShadows
    }

    Item { width: 1; height: 12 }
}
