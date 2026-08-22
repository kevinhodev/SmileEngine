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

    // Picker HSV: quadrado SV + barra de matiz + presets.
    Item {
        id: picker
        width: parent.width
        height: 146

        property real hue: 0
        property real sat: 0
        property real val: 1
        property bool syncing: false

        function syncFromModel() {
            var c = root.lightsModel.color
            syncing = true
            hue = c.hsvHue >= 0 ? c.hsvHue : hue // -1 = acromatico: preserva o matiz
            sat = c.hsvSaturation
            val = c.hsvValue
            syncing = false
        }
        function push() {
            if (!syncing) root.lightsModel.color = Qt.hsva(hue, sat, val, 1.0)
        }
        // Conexoes imperativas: os sinais SelectionChanged/LightChanged (maiuscula) nao casam
        // com handlers declarativos de Connections — sem isto o picker nao re-sincroniza ao
        // trocar de luz (bug latente herdado do LightsPanel).
        Component.onCompleted: {
            syncFromModel()
            root.lightsModel.SelectionChanged.connect(picker.syncFromModel)
            root.lightsModel.LightChanged.connect(function() {
                if (!svDrag.active && !hueDrag.active) picker.syncFromModel()
            })
        }

        // quadrado SV
        Rectangle {
            id: svBox
            x: 0; y: 0
            width: parent.width - 32
            height: 108
            radius: C.Theme.radiusControl
            clip: true
            border.color: C.Theme.hairline
            border.width: 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#ffffff" }
                GradientStop { position: 1.0; color: Qt.hsva(picker.hue, 1, 1, 1) }
            }
            Rectangle {
                anchors.fill: parent
                radius: C.Theme.radiusControl
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: "#000000" }
                }
            }
            // cursor S/V
            Rectangle {
                x: picker.sat * (svBox.width - 1) - width / 2
                y: (1 - picker.val) * (svBox.height - 1) - height / 2
                width: 13; height: 13; radius: 6.5
                color: "transparent"
                border.color: picker.val > 0.55 && picker.sat < 0.5 ? "#00000090" : "#ffffffd0"
                border.width: 2
            }
            MouseArea {
                id: svDrag
                property bool active: pressed
                anchors.fill: parent
                cursorShape: Qt.CrossCursor
                function apply(mx, my) {
                    picker.sat = Math.max(0, Math.min(1, mx / (width - 1)))
                    picker.val = 1 - Math.max(0, Math.min(1, my / (height - 1)))
                    picker.push()
                }
                onPressed: (e) => apply(e.x, e.y)
                onPositionChanged: (e) => { if (pressed) apply(e.x, e.y) }
            }
        }

        // barra de matiz (vertical, ao lado do quadrado)
        Rectangle {
            id: hueBar
            anchors.right: parent.right
            y: 0
            width: 22
            height: 108
            radius: C.Theme.radiusControl
            clip: true
            border.color: C.Theme.hairline
            border.width: 1
            gradient: Gradient {
                GradientStop { position: 0.000; color: "#ff0000" }
                GradientStop { position: 0.167; color: "#ffff00" }
                GradientStop { position: 0.333; color: "#00ff00" }
                GradientStop { position: 0.500; color: "#00ffff" }
                GradientStop { position: 0.667; color: "#0000ff" }
                GradientStop { position: 0.833; color: "#ff00ff" }
                GradientStop { position: 1.000; color: "#ff0000" }
            }
            Rectangle {
                x: 1
                y: picker.hue * (hueBar.height - 5) + 1
                width: parent.width - 2
                height: 4
                radius: 2
                color: "transparent"
                border.color: "#ffffffd0"
                border.width: 1.5
            }
            MouseArea {
                id: hueDrag
                property bool active: pressed
                anchors.fill: parent
                function apply(my) {
                    picker.hue = Math.max(0, Math.min(1, my / (height - 1)))
                    picker.push()
                }
                onPressed: (e) => apply(e.y)
                onPositionChanged: (e) => { if (pressed) apply(e.y) }
            }
        }

        // preview + presets
        Row {
            y: 118
            spacing: 6
            Rectangle {
                width: 40; height: 20; radius: C.Theme.radiusChip
                color: Qt.hsva(picker.hue, picker.sat, picker.val, 1)
                border.color: C.Theme.hairlineStrong
                border.width: 1
            }
            Repeater {
                model: ["#ffffff", "#ffd9a0", "#ffb066", "#ff6a4d",
                        "#cfe0ff", "#8fd0ff", "#8dff9c", "#d08bff"]
                delegate: Rectangle {
                    required property string modelData
                    width: 20; height: 20; radius: C.Theme.radiusChip
                    color: modelData
                    border.color: Qt.darker(color, 1.5)
                    border.width: 1
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            root.lightsModel.color = modelData
                            picker.syncFromModel()
                        }
                    }
                }
            }
        }
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
