import QtQuick
import QtQuick.Controls

// Painel de Luzes (dock lateral). Liga em `lightsModel` (LightsBridge): lista das luzes da cena
// (add/duplicar/remover/toggle), propriedades da selecionada (cor com picker HSV, intensidade,
// raio, cones do spot, posicao) e persistencia em <cena>.lights.json. Estilo do TimeOfDayPanel
// (dark, cards, accent azul).
Rectangle {
    id: root
    color: "#141511"

    readonly property color cardBg: "#1a1b15"
    readonly property color borderColor: "#2a2b24"
    readonly property color divider: "#23241d"
    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textSecondary: "#9a958a"
    readonly property color textMuted: "#6c6a61"
    readonly property color blue: "#5b9dff"
    readonly property color bulbColor: "#e8c565"

    readonly property bool hasSelection: lightsModel.selectedIndex >= 0

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }

    // ---- Componentes locais (mesma linguagem do TimeOfDayPanel/SettingsWindow) ----
    component Toggle: Item {
        id: toggle
        property bool checked: false
        property bool interactive: true
        signal toggled()
        implicitWidth: 36
        implicitHeight: 20
        opacity: interactive ? 1.0 : 0.48
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: toggle.checked ? root.blue : "#23241d"
            border.color: toggle.checked ? root.blue : "#33342c"
            border.width: 1
            Rectangle {
                width: 14; height: 14; radius: 7
                y: 3
                x: toggle.checked ? 19 : 3
                color: toggle.checked ? "#f2efe6" : root.textMuted
                Behavior on x { NumberAnimation { duration: 100 } }
            }
        }
        HoverHandler { cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
        TapHandler { enabled: toggle.interactive; onTapped: toggle.toggled() }
    }

    component Card: Rectangle {
        default property alias content: inner.data
        property string title
        property string iconName
        property alias headerItem: headerSlot.data
        width: parent.width
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1
        implicitHeight: 40 + inner.implicitHeight + 14

        LucideIcon {
            visible: parent.iconName !== ""
            x: 14; y: 13
            name: parent.iconName
            size: 14
            color: root.textSecondary
        }
        Text {
            x: parent.iconName !== "" ? 36 : 16; y: 12
            text: parent.title
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Item {
            id: headerSlot
            anchors.right: parent.right
            anchors.rightMargin: 14
            y: 10
            width: childrenRect.width
            height: 20
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            y: 36; height: 1
            color: root.divider
        }
        Column {
            id: inner
            x: 14; y: 44
            width: parent.width - 28
            spacing: 10
        }
    }

    component SliderRow: Item {
        id: row
        property string label
        property string valueText
        property real from: 0
        property real to: 1
        property real stepSize: 0
        property real boundValue: 0
        signal moved(real v)
        width: parent.width
        height: 38

        Text {
            x: 0; y: 0
            text: row.label
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Text {
            anchors.right: parent.right; y: 0
            text: row.valueText
            color: root.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Slider {
            id: sl
            x: 0; y: 16
            width: parent.width
            height: 18
            from: row.from
            to: row.to
            stepSize: row.stepSize
            onMoved: row.moved(value)
            Binding {
                target: sl; property: "value"
                value: row.boundValue
                when: !sl.pressed
                restoreMode: Binding.RestoreBinding
            }
            background: Rectangle {
                x: sl.leftPadding
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: sl.availableWidth
                height: 4; radius: 2
                color: "#23241d"
                Rectangle {
                    width: sl.visualPosition * parent.width
                    height: parent.height; radius: 2
                    color: root.blue
                }
            }
            handle: Rectangle {
                x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: 12; height: 12; radius: 6
                color: "#f2efe6"
                border.color: root.blue
                border.width: 1.5
            }
        }
    }

    // Botaozinho de icone com hover (linhas da lista, header).
    component IconButton: Item {
        id: ib
        property string icon
        property color tint: root.textMuted
        property color hoverTint: root.textPrimary
        property string tip
        signal clicked()
        width: 22; height: 22
        Rectangle {
            anchors.fill: parent; radius: 5
            color: ibHover.hovered ? "#23241d" : "transparent"
        }
        LucideIcon {
            anchors.centerIn: parent
            name: ib.icon; size: 13
            color: ibHover.hovered ? ib.hoverTint : ib.tint
        }
        HoverHandler { id: ibHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: ib.clicked() }
        ToolTip.visible: ib.tip !== "" && ibHover.hovered
        ToolTip.delay: 600
        ToolTip.text: ib.tip
    }

    // Campo numerico compacto (posicao XYZ).
    component NumberField: Rectangle {
        id: nf
        property string label
        property real value: 0
        signal committed(real v)
        height: 26
        radius: 5
        color: "#111209"
        border.color: nfInput.activeFocus ? root.blue : "#2a2b24"
        border.width: 1

        Text {
            x: 7
            anchors.verticalCenter: parent.verticalCenter
            text: nf.label
            color: root.textMuted
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.DemiBold
        }
        TextInput {
            id: nfInput
            anchors.fill: parent
            anchors.leftMargin: 20; anchors.rightMargin: 6
            verticalAlignment: TextInput.AlignVCenter
            horizontalAlignment: TextInput.AlignRight
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
            selectByMouse: true
            selectionColor: root.blue
            text: nf.value.toFixed(2)
            validator: DoubleValidator { locale: "C"; notation: DoubleValidator.StandardNotation }
            onActiveFocusChanged: if (activeFocus) selectAll()
            onEditingFinished: {
                var v = parseFloat(text.replace(",", "."))
                if (!isNaN(v)) nf.committed(v)
                focus = false
            }
            // valor externo (gizmo arrastando) atualiza o campo quando nao esta em edicao
            Binding {
                target: nfInput; property: "text"
                value: nf.value.toFixed(2)
                when: !nfInput.activeFocus
                restoreMode: Binding.RestoreBinding
            }
        }
    }

    // ---- Header do painel ----
    Rectangle {
        id: header
        anchors.left: parent.left; anchors.right: parent.right
        height: 34
        color: "#10110f"

        LucideIcon {
            x: 12; anchors.verticalCenter: parent.verticalCenter
            name: "lightbulb"; size: 15
            color: lightsModel.count > 0 ? root.bulbColor : root.textMuted
        }
        Text {
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            text: "Luzes"
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        // badge com a contagem
        Rectangle {
            x: 74
            anchors.verticalCenter: parent.verticalCenter
            visible: lightsModel.count > 0
            width: countLabel.implicitWidth + 12
            height: 16
            radius: 8
            color: "#23241d"
            Text {
                id: countLabel
                anchors.centerIn: parent
                text: lightsModel.count
                color: root.textSecondary
                font.family: "Segoe UI"
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
        }

        // salvar (ponto ambar = mudancas nao salvas)
        Item {
            id: saveBtn
            anchors.right: closeBtn.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24
            opacity: lightsModel.hasSceneFile ? 1.0 : 0.35
            Rectangle {
                anchors.fill: parent; radius: 5
                color: saveHover.hovered && lightsModel.hasSceneFile ? "#23241d" : "transparent"
            }
            LucideIcon {
                anchors.centerIn: parent
                name: "save"; size: 14
                color: saveHover.hovered && lightsModel.hasSceneFile ? root.textPrimary
                                                                     : root.textMuted
            }
            Rectangle {
                visible: lightsModel.dirty
                x: 15; y: 2
                width: 7; height: 7; radius: 3.5
                color: root.bulbColor
                border.color: "#10110f"
                border.width: 1
            }
            HoverHandler {
                id: saveHover
                cursorShape: lightsModel.hasSceneFile ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
            TapHandler { enabled: lightsModel.hasSceneFile; onTapped: lightsModel.saveLights() }
            ToolTip.visible: saveHover.hovered
            ToolTip.delay: 600
            ToolTip.text: lightsModel.hasSceneFile
                          ? "Salvar luzes (<cena>.lights.json)"
                          : "Carregue uma cena para salvar luzes"
        }
        Item {
            id: closeBtn
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24
            Rectangle {
                anchors.fill: parent; radius: 5
                color: closeHover.hovered ? "#23241d" : "transparent"
            }
            LucideIcon {
                anchors.centerIn: parent
                name: "x"; size: 14
                color: closeHover.hovered ? root.textPrimary : root.textMuted
            }
            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: lightsModel.closePanel() }
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1; color: root.divider
        }
    }

    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        anchors.topMargin: 10
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: col
            width: parent.width
            spacing: 10
            opacity: lightsModel.available ? 1.0 : 0.45

            // ---- Lista de luzes ----
            Card {
                title: "Luzes na cena"
                iconName: "lightbulb"
                headerItem: [
                    Row {
                        spacing: 6
                        // adicionar point / spot
                        Rectangle {
                            width: addPointRow.implicitWidth + 16; height: 20; radius: 10
                            color: addPointHover.hovered ? "#20304e" : "#1c2438"
                            border.color: "#2c3f63"
                            border.width: 1
                            Row {
                                id: addPointRow
                                anchors.centerIn: parent
                                spacing: 3
                                LucideIcon {
                                    name: "plus"; size: 10; color: root.blue
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                LucideIcon {
                                    name: "lightbulb"; size: 11; color: root.blue
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: "Point"
                                    color: root.blue
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            HoverHandler { id: addPointHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: lightsModel.addLight(0) }
                        }
                        Rectangle {
                            width: addSpotRow.implicitWidth + 16; height: 20; radius: 10
                            color: addSpotHover.hovered ? "#20304e" : "#1c2438"
                            border.color: "#2c3f63"
                            border.width: 1
                            Row {
                                id: addSpotRow
                                anchors.centerIn: parent
                                spacing: 3
                                LucideIcon {
                                    name: "plus"; size: 10; color: root.blue
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                LucideIcon {
                                    name: "cone"; size: 11; color: root.blue
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: "Spot"
                                    color: root.blue
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            HoverHandler { id: addSpotHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: lightsModel.addLight(1) }
                        }
                    }
                ]

                // estado vazio
                Text {
                    visible: lightsModel.count === 0
                    width: parent.width
                    text: "Nenhuma luz na cena.\nAdicione uma com os botões acima — ela nasce na frente da câmera."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }

                Column {
                    width: parent.width
                    spacing: 2

                    Repeater {
                        model: lightsModel.count

                        delegate: Rectangle {
                            id: lightRow
                            required property int index
                            // depende de `revision`: qualquer mudanca re-le o lightAt()
                            property var info: lightsModel.revision >= 0
                                               ? lightsModel.lightAt(index) : ({})
                            readonly property bool selected: lightsModel.selectedIndex === index

                            width: parent.width
                            height: 30
                            radius: 6
                            color: selected ? "#1d2743"
                                 : rowHover.hovered ? "#20211a" : "transparent"
                            border.color: selected ? "#33477a" : "transparent"
                            border.width: 1

                            // dot da cor da luz
                            Rectangle {
                                x: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 9; height: 9; radius: 4.5
                                color: lightRow.info.color !== undefined ? lightRow.info.color : "#888"
                                border.color: Qt.darker(color, 1.6)
                                border.width: 1
                                opacity: lightRow.info.enabled ? 1.0 : 0.35
                            }
                            LucideIcon {
                                x: 24
                                anchors.verticalCenter: parent.verticalCenter
                                name: lightRow.info.type === 1 ? "cone" : "lightbulb"
                                size: 12
                                color: lightRow.info.enabled ? root.textSecondary : root.textMuted
                            }
                            Text {
                                x: 43
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 43 - 110
                                text: lightRow.info.name !== undefined ? lightRow.info.name : ""
                                color: lightRow.info.enabled
                                       ? (lightRow.selected ? root.textPrimary : root.textNormal)
                                       : root.textMuted
                                font.family: "Segoe UI"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                IconButton {
                                    visible: rowHover.hovered
                                    icon: "copy"; tip: "Duplicar"
                                    onClicked: lightsModel.duplicateLight(lightRow.index)
                                }
                                IconButton {
                                    visible: rowHover.hovered
                                    icon: "trash-2"; tip: "Remover"
                                    hoverTint: "#e07a6a"
                                    onClicked: lightsModel.removeLight(lightRow.index)
                                }
                                Toggle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: lightRow.info.enabled === true
                                    onToggled: lightsModel.toggleLightEnabled(lightRow.index)
                                }
                            }

                            HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: lightsModel.selectLight(lightRow.index) }
                        }
                    }
                }

                Text {
                    visible: lightsModel.count > 0
                    width: parent.width
                    text: "Clique no marker no viewport para selecionar; arraste com o gizmo."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }
            }

            // ---- Propriedades da selecionada ----
            Card {
                title: lightsModel.lightType === 1 ? "Propriedades — Spot" : "Propriedades — Point"
                iconName: lightsModel.lightType === 1 ? "cone" : "lightbulb"
                visible: root.hasSelection
                headerItem: [
                    Toggle {
                        checked: lightsModel.lightEnabled
                        onToggled: lightsModel.lightEnabled = !checked
                    }
                ]

                // nome
                Rectangle {
                    width: parent.width
                    height: 28
                    radius: 5
                    color: "#111209"
                    border.color: nameInput.activeFocus ? root.blue : "#2a2b24"
                    border.width: 1
                    TextInput {
                        id: nameInput
                        anchors.fill: parent
                        anchors.leftMargin: 9; anchors.rightMargin: 9
                        verticalAlignment: TextInput.AlignVCenter
                        color: root.textPrimary
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                        selectByMouse: true
                        selectionColor: root.blue
                        text: lightsModel.name
                        onEditingFinished: { lightsModel.name = text; focus = false }
                        Binding {
                            target: nameInput; property: "text"
                            value: lightsModel.name
                            when: !nameInput.activeFocus
                            restoreMode: Binding.RestoreBinding
                        }
                    }
                }

                // ---- Cor (picker HSV: quadrado SV + barra de matiz + presets) ----
                Item {
                    id: picker
                    width: parent.width
                    height: 148

                    property real hue: 0
                    property real sat: 0
                    property real val: 1
                    property bool syncing: false

                    function syncFromModel() {
                        var c = lightsModel.color
                        syncing = true
                        hue = c.hsvHue >= 0 ? c.hsvHue : hue // -1 = acromatico: preserva o matiz
                        sat = c.hsvSaturation
                        val = c.hsvValue
                        syncing = false
                    }
                    function push() {
                        if (!syncing) lightsModel.color = Qt.hsva(hue, sat, val, 1.0)
                    }
                    Component.onCompleted: syncFromModel()
                    Connections {
                        target: lightsModel
                        function onSelectionChanged() { picker.syncFromModel() }
                        function onLightChanged() {
                            if (!svDrag.active && !hueDrag.active) picker.syncFromModel()
                        }
                    }

                    // quadrado SV
                    Rectangle {
                        id: svBox
                        x: 0; y: 0
                        width: parent.width - 34
                        height: 110
                        radius: 6
                        clip: true
                        border.color: root.borderColor
                        border.width: 1
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#ffffff" }
                            GradientStop { position: 1.0; color: Qt.hsva(picker.hue, 1, 1, 1) }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
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
                            border.color: picker.val > 0.55 && picker.sat < 0.5 ? "#00000090"
                                                                                : "#ffffffd0"
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
                        height: 110
                        radius: 6
                        clip: true
                        border.color: root.borderColor
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
                        y: 120
                        spacing: 6
                        Rectangle {
                            width: 40; height: 20; radius: 5
                            color: Qt.hsva(picker.hue, picker.sat, picker.val, 1)
                            border.color: root.borderColor
                            border.width: 1
                        }
                        Repeater {
                            model: ["#ffffff", "#ffd9a0", "#ffb066", "#ff6a4d",
                                    "#cfe0ff", "#8fd0ff", "#8dff9c", "#d08bff"]
                            delegate: Rectangle {
                                required property string modelData
                                width: 20; height: 20; radius: 5
                                color: modelData
                                border.color: Qt.darker(color, 1.5)
                                border.width: 1
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        lightsModel.color = modelData
                                        picker.syncFromModel()
                                    }
                                }
                            }
                        }
                    }
                }

                SliderRow {
                    label: "Intensidade"
                    valueText: root.fmt(lightsModel.intensity)
                    from: 0; to: 150
                    boundValue: lightsModel.intensity
                    onMoved: v => lightsModel.intensity = v
                }
                SliderRow {
                    label: "Raio de atenuação"
                    valueText: root.fmt(lightsModel.radius) + " m"
                    from: 0.5; to: 60
                    boundValue: lightsModel.radius
                    onMoved: v => lightsModel.radius = v
                }
                SliderRow {
                    label: "Bulbo (raio da fonte)"
                    valueText: root.fmt(lightsModel.sourceRadius, 2) + " m"
                    from: 0.01; to: 0.5
                    boundValue: lightsModel.sourceRadius
                    onMoved: v => lightsModel.sourceRadius = v
                }

                // ---- Sombras (spot: atlas 2D; point: cubemap) ----
                Item {
                    width: parent.width
                    height: 24
                    Text {
                        y: 4
                        text: "Projeta sombras"
                        color: root.textNormal
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    Text {
                        y: 5
                        x: 96
                        text: lightsModel.lightType === 1 ? "· 8 spots/quadro"
                                                          : "· 4 points/quadro"
                        color: root.textMuted
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                    }
                    Toggle {
                        anchors.right: parent.right
                        y: 2
                        checked: lightsModel.castShadows
                        onToggled: lightsModel.castShadows = !checked
                    }
                }
                SliderRow {
                    visible: lightsModel.lightType === 1
                    label: "Cone interno"
                    valueText: Math.round(lightsModel.innerConeDeg) + "°"
                    from: 0; to: 89
                    boundValue: lightsModel.innerConeDeg
                    onMoved: v => lightsModel.innerConeDeg = v
                }
                SliderRow {
                    visible: lightsModel.lightType === 1
                    label: "Cone externo"
                    valueText: Math.round(lightsModel.outerConeDeg) + "°"
                    from: 1; to: 89
                    boundValue: lightsModel.outerConeDeg
                    onMoved: v => lightsModel.outerConeDeg = v
                }
                SliderRow {
                    visible: lightsModel.lightType === 1
                    label: "Direção — azimute"
                    valueText: Math.round(lightsModel.spotAzimuthDeg) + "°"
                    from: 0; to: 360
                    boundValue: lightsModel.spotAzimuthDeg
                    onMoved: v => lightsModel.spotAzimuthDeg = v
                }
                SliderRow {
                    visible: lightsModel.lightType === 1
                    label: "Direção — inclinação"
                    valueText: Math.round(lightsModel.spotElevationDeg) + "°"
                    from: -90; to: 90
                    boundValue: lightsModel.spotElevationDeg
                    onMoved: v => lightsModel.spotElevationDeg = v
                }

                // ---- Posicao ----
                Item {
                    width: parent.width
                    height: 46

                    Text {
                        x: 0; y: 0
                        text: "Posição"
                        color: root.textNormal
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                    Row {
                        y: 17
                        width: parent.width
                        spacing: 6
                        NumberField {
                            label: "X"
                            width: (parent.width - 12 - 30) / 3
                            value: lightsModel.posX
                            onCommitted: v => lightsModel.posX = v
                        }
                        NumberField {
                            label: "Y"
                            width: (parent.width - 12 - 30) / 3
                            value: lightsModel.posY
                            onCommitted: v => lightsModel.posY = v
                        }
                        NumberField {
                            label: "Z"
                            width: (parent.width - 12 - 30) / 3
                            value: lightsModel.posZ
                            onCommitted: v => lightsModel.posZ = v
                        }
                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            icon: "video"
                            tip: "Reposicionar na frente da câmera"
                            tint: root.textSecondary
                            onClicked: lightsModel.placeAtCamera()
                        }
                    }
                }
            }

            // dica quando nada selecionado
            Card {
                title: "Propriedades"
                iconName: "info"
                visible: !root.hasSelection

                Text {
                    width: parent.width
                    text: "Selecione uma luz na lista ou clique no marker dela no viewport."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
