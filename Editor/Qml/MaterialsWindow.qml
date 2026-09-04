pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Editor de Materiais (menu Janela > Materiais). Liga em `materialsModel` (MaterialsBridge)
// e `materialsWindow` (WindowBridge da QDialog frameless). Browser a esquerda (busca, chips,
// badges, VRAM por material), propriedades em cards a direita — edicao AO VIVO na cena
// (estilo Material Instance da Flax); clicar numa mesh do viewport seleciona o material dela
// pelo picking GPU existente.
// Mesmo chrome/linguagem do TimeOfDayWindow/StatsWindow.
Rectangle {
    id: root
    required property var materialsModel
    required property var materialsWindow

    color: C.Theme.shell
    border.color: C.Theme.hairlineStrong
    border.width: 1
    implicitWidth: 1460
    implicitHeight: 800
    focus: true

    readonly property color cardBg: C.Theme.cardBg
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color blueBg: C.Theme.blueBg
    readonly property color blueBorder: C.Theme.blueBorder
    readonly property color amber: C.Theme.amber

    function fmt(v, dec) { return Number(v).toFixed(dec === undefined ? 2 : dec).replace(".", ",") }

    // Busca de parametro (UE-style, Ctrl+F): filtra linhas e cards pelos labels.
    property string paramSearch: ""
    function matchParam(s) {
        return paramSearch === "" || s.toLowerCase().indexOf(paramSearch.toLowerCase()) >= 0
    }
    function badgeColor(b) {
        if (b === "FOL")   return "#8fbf5f"
        if (b === "POM")   return root.amber
        if (b === "BLEND") return "#7fb8d8"
        if (b === "MASK")  return "#d8a875"
        if (b === "EMIS")  return "#e8a865"
        return root.textSecondary // 2S
    }

    Shortcut {
        sequence: "Ctrl+S"
        context: Qt.WindowShortcut
        onActivated: root.materialsModel.saveMaterials()
    }
    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.WindowShortcut
        onActivated: paramSearchInput.forceActiveFocus()
    }

    // ---- Componentes compartilhados (Editor/Qml/components) + regras locais de busca ----
    component Toggle: C.Toggle {}
    component ToggleRow: C.ToggleRow { visible: root.matchParam(label) }
    component Card: C.Card {
        property string keywords: ""  // labels do card p/ busca de parametro (Ctrl+F)
        visible: root.paramSearch === "" || root.matchParam(keywords)
    }
    component SliderRow: C.SliderRow { visible: root.matchParam(label) }
    component Chip: C.Chip {}
    component ColorRow: C.ColorRow { visible: root.matchParam(label) }

    C.FramelessTitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: 40
        windowBridge: root.materialsWindow
        title: "Materiais"
        subtitle: root.materialsModel.available
            ? root.materialsModel.totalCount + " materiais · edição ao vivo na cena"
            : "aguardando o renderer…"
        iconName: "palette"
        iconColor: root.blue
        showMaximize: false
    }

    // ---- Toolbar (Salvar / Reverter) ----
    Item {
        id: toolbar
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 13
        anchors.rightMargin: 13
        height: 42

        Rectangle {
            id: saveBtn
            y: 7
            width: saveRow.implicitWidth + 24
            height: 28
            radius: C.Theme.radiusControl
            color: root.materialsModel.dirty ? C.Theme.accentBgStrong
                                        : (saveHover.hovered ? C.Theme.surfaceRaised
                                                             : C.Theme.surface)
            border.color: root.materialsModel.dirty ? C.Theme.accentBorder : C.Theme.hairlineStrong
            border.width: 1
            Row {
                id: saveRow
                anchors.centerIn: parent
                spacing: 6
                LucideIcon {
                    name: "save"; size: 13
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.materialsModel.dirty ? C.Theme.accent : root.textSecondary
                }
                Text {
                    text: "Salvar"
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.materialsModel.dirty ? C.Theme.accentText : root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: root.materialsModel.dirty ? Font.DemiBold : Font.Normal
                }
            }
            HoverHandler { id: saveHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.materialsModel.saveMaterials() }
        }

        Rectangle {
            id: revertBtn
            anchors.left: saveBtn.right
            anchors.leftMargin: 8
            y: 7
            width: revertRow.implicitWidth + 24
            height: 28
            radius: C.Theme.radiusControl
            visible: root.materialsModel.hasSelection
            opacity: root.materialsModel.selectedModified ? 1.0 : 0.45
            color: revHover.hovered && root.materialsModel.selectedModified
                ? C.Theme.surfaceRaised : C.Theme.surface
            border.color: C.Theme.hairlineStrong
            border.width: 1
            Row {
                id: revertRow
                anchors.centerIn: parent
                spacing: 6
                LucideIcon {
                    name: "rotate-ccw"; size: 13
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.textSecondary
                }
                Text {
                    text: "Reverter material"
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
            }
            HoverHandler { id: revHover; cursorShape: root.materialsModel.selectedModified ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { enabled: root.materialsModel.selectedModified; onTapped: root.materialsModel.resetSelected() }
        }

        // Busca de parametro (Ctrl+F): filtra os cards/linhas de propriedade da direita.
        Rectangle {
            id: paramSearchBox
            anchors.right: dirtyRow.visible ? dirtyRow.left : parent.right
            anchors.rightMargin: dirtyRow.visible ? 14 : 0
            y: 7
            width: 210
            height: 28
            radius: 6
            color: C.Theme.surface
            border.color: paramSearchInput.activeFocus ? C.Theme.accentBorder
                                                        : C.Theme.hairlineStrong
            border.width: 1
            LucideIcon {
                x: 9
                anchors.verticalCenter: parent.verticalCenter
                name: "search"; size: 12
                color: root.textMuted
            }
            TextInput {
                id: paramSearchInput
                anchors.fill: parent
                anchors.leftMargin: 28
                anchors.rightMargin: 24
                verticalAlignment: TextInput.AlignVCenter
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
                clip: true
                selectByMouse: true
                selectionColor: root.blue
                onTextEdited: root.paramSearch = text
                Keys.onEscapePressed: { text = ""; root.paramSearch = ""; focus = false }
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: paramSearchInput.text.length === 0 && !paramSearchInput.activeFocus
                    text: "Buscar parâmetro…  Ctrl+F"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: paramSearchInput.text.length > 0
                text: "×"
                color: root.textSecondary
                font.pixelSize: 13
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { paramSearchInput.text = ""; root.paramSearch = "" } }
            }
        }

        Row {
            id: dirtyRow
            anchors.right: parent.right
            anchors.verticalCenter: saveBtn.verticalCenter
            spacing: 6
            visible: root.materialsModel.dirty
            Rectangle {
                width: 7; height: 7; radius: 3.5
                anchors.verticalCenter: parent.verticalCenter
                color: root.amber
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "alterações não salvas · Ctrl+S"
                color: root.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }
    }

    // ---- Browser (esquerda) ----
    Rectangle {
        id: browser
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 13
        anchors.bottomMargin: 13
        width: 300
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1

        // busca
        Rectangle {
            id: searchBox
            x: 12; y: 12
            width: parent.width - 24
            height: 28
            radius: 6
            color: C.Theme.shell
            border.color: searchInput.activeFocus ? root.blueBorder : root.borderColor
            border.width: 1
            LucideIcon {
                x: 9
                anchors.verticalCenter: parent.verticalCenter
                name: "search"; size: 12
                color: root.textMuted
            }
            TextInput {
                id: searchInput
                anchors.fill: parent
                anchors.leftMargin: 28
                anchors.rightMargin: 24
                verticalAlignment: TextInput.AlignVCenter
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
                clip: true
                selectByMouse: true
                selectionColor: root.blue
                onTextEdited: root.materialsModel.search = text
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: searchInput.text.length === 0 && !searchInput.activeFocus
                    text: "Buscar material…"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: searchInput.text.length > 0
                text: "×"
                color: root.textSecondary
                font.pixelSize: 13
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { searchInput.text = ""; root.materialsModel.search = "" } }
            }
        }

        // chips de filtro
        Row {
            id: filterChips
            x: 12; y: 48
            spacing: 6
            Chip { label: "Todos";   active: root.materialsModel.filter === 0; onTapped: root.materialsModel.filter = 0 }
            Chip { label: "Opacos";  active: root.materialsModel.filter === 1; onTapped: root.materialsModel.filter = 1 }
            Chip { label: "Blend";   active: root.materialsModel.filter === 2; onTapped: root.materialsModel.filter = 2 }
            Chip { label: "Foliage"; active: root.materialsModel.filter === 3; onTapped: root.materialsModel.filter = 3 }
            Chip { label: "POM";     active: root.materialsModel.filter === 4; onTapped: root.materialsModel.filter = 4 }
        }

        ListView {
            id: matList
            anchors.top: filterChips.bottom
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footer.top
            anchors.leftMargin: 6
            anchors.rightMargin: 4
            clip: true
            model: root.materialsModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            Component.onCompleted: {
                // Sinal maiusculo da bridge: handler declarativo de Connections nao dispara
                // (gotcha conhecido) — conexao imperativa.
                root.materialsModel.ScrollToRequested.connect(function(row) {
                    matList.positionViewAtIndex(row, ListView.Contain)
                })
            }

            delegate: Rectangle {
                id: matRow
                required property int index
                required property string name
                required property int meshCount
                required property string vramText
                required property var badges
                required property color swatch
                required property bool selected
                required property bool modified
                required property string thumbUrl

                width: matList.width
                height: 50
                radius: C.Theme.radiusCard
                color: matRow.selected ? C.Theme.accentBg
                                       : (rowHover.hovered ? C.Theme.surface : "transparent")

                Rectangle {
                    visible: matRow.selected
                    x: 0; y: 5
                    width: 3; height: parent.height - 10
                    radius: 1.5
                    color: C.Theme.accent
                }
                // thumbnail real (esfera renderizada com o material + HDRI); fallback =
                // swatch com "luz" fake enquanto a fila nao chega nesse material.
                Rectangle {
                    id: ball
                    x: 12; y: 9
                    width: 32; height: 32; radius: 16
                    color: matRow.thumbUrl !== "" ? "transparent" : matRow.swatch
                    border.color: matRow.thumbUrl !== "" ? "transparent" : Qt.darker(matRow.swatch, 1.6)
                    border.width: 1
                    Rectangle {
                        visible: matRow.thumbUrl === ""
                        x: 6; y: 5
                        width: 10; height: 10; radius: 5
                        color: "#ffffff"
                        opacity: 0.35
                    }
                    Image {
                        visible: matRow.thumbUrl !== ""
                        anchors.fill: parent
                        anchors.margins: 1
                        source: matRow.thumbUrl
                        sourceSize: Qt.size(96, 96)
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                    }
                }
                Text {
                    x: 54; y: 8
                    width: badgeRow.x - x - 6
                    text: matRow.name
                    color: matRow.selected ? root.textPrimary : root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: matRow.selected ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                }
                Text {
                    x: 54; y: 26
                    text: matRow.meshCount + (matRow.meshCount === 1 ? " mesh · " : " meshes · ") + matRow.vramText
                          + (matRow.modified ? "  ●" : "")
                    color: matRow.modified ? root.amber : root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }
                Row {
                    id: badgeRow
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    y: 9
                    spacing: 4
                    Repeater {
                        model: matRow.badges
                        delegate: Rectangle {
                            required property string modelData
                            width: bLabel.implicitWidth + 10
                            height: 15
                            radius: 7
                            color: C.Theme.shell
                            border.color: root.borderColor
                            border.width: 1
                            Text {
                                id: bLabel
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: root.badgeColor(parent.modelData)
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.materialsModel.selectRow(matRow.index) }
            }
        }

        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: footer.top
            anchors.leftMargin: 1; anchors.rightMargin: 1
            height: 1
            color: root.divider
        }
        Item {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 30
            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                text: matList.count === root.materialsModel.totalCount
                      ? root.materialsModel.totalCount + " materiais na cena"
                      : matList.count + " de " + root.materialsModel.totalCount + " materiais"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }
    }

    // ---- Preview (centro): primitiva + material + HDRI proprio ----
    Rectangle {
        id: previewPane
        anchors.top: toolbar.bottom
        anchors.left: browser.right
        anchors.right: propsPane.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.bottomMargin: 13
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1

        // toolbar do preview: primitivas + HDRI
        Row {
            id: previewChips
            x: 12; y: 10
            spacing: 6
            Chip { label: "Esfera";   active: root.materialsModel.previewPrimitive === 0; onTapped: root.materialsModel.previewPrimitive = 0 }
            Chip { label: "Cubo";     active: root.materialsModel.previewPrimitive === 1; onTapped: root.materialsModel.previewPrimitive = 1 }
            Chip { label: "Plano";    active: root.materialsModel.previewPrimitive === 2; onTapped: root.materialsModel.previewPrimitive = 2 }
            Chip { label: "Cilindro"; active: root.materialsModel.previewPrimitive === 3; onTapped: root.materialsModel.previewPrimitive = 3 }
            Chip { label: "Mesh da cena"; active: root.materialsModel.previewPrimitive === 4; onTapped: root.materialsModel.previewPrimitive = 4 }
        }
        Chip {
            anchors.right: parent.right
            anchors.rightMargin: 12
            y: 10
            label: "HDRI…"
            onTapped: root.materialsModel.browsePreviewHdri()
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 1; anchors.rightMargin: 1
            y: 44; height: 1
            color: root.divider
        }

        // area da imagem (quadrada, centrada)
        Rectangle {
            id: previewArea
            anchors.top: parent.top
            anchors.topMargin: 45
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: previewHint.top
            anchors.margins: 12
            radius: 6
            color: C.Theme.shell
            clip: true

            Image {
                id: previewImage
                anchors.fill: parent
                visible: root.materialsModel.hasSelection && root.materialsModel.previewSeq > 0
                source: "image://materialpreview/p" + root.materialsModel.previewSeq
                cache: false
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            Column {
                anchors.centerIn: parent
                spacing: 8
                visible: !previewImage.visible
                LucideIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "globe"; size: 26
                    color: root.textMuted
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: !root.materialsModel.hasSelection ? "selecione um material"
                        : "preparando o preview (HDRI + IBL)…"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                property real lastX: 0
                property real lastY: 0
                onPressed: (e) => { lastX = e.x; lastY = e.y }
                onPositionChanged: (e) => {
                    if (!pressed) return
                    var dx = e.x - lastX
                    var dy = e.y - lastY
                    lastX = e.x; lastY = e.y
                    if (e.modifiers & Qt.ShiftModifier)
                        root.materialsModel.rotatePreviewEnv(dx)
                    else
                        root.materialsModel.orbitPreview(dx, dy)
                }
                onWheel: (w) => root.materialsModel.zoomPreview(w.angleDelta.y / 120.0)
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            }
        }

        Text {
            id: previewHint
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 10
            anchors.horizontalCenter: parent.horizontalCenter
            text: "arraste orbita · scroll aproxima · Shift+arraste gira o HDRI"
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }
    }

    // ---- Propriedades (direita) ----
    Item {
        id: propsPane
        anchors.top: toolbar.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 13
        anchors.bottomMargin: 13
        width: 400

        // estado vazio
        Column {
            anchors.centerIn: parent
            spacing: 8
            visible: !root.materialsModel.hasSelection
            LucideIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "palette"; size: 28
                color: root.textMuted
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Selecione um material na lista"
                color: root.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "ou clique numa mesh no viewport — o material dela abre aqui"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }

        Flickable {
            anchors.fill: parent
            visible: root.materialsModel.hasSelection
            contentHeight: propsCol.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            Column {
                id: propsCol
                width: parent.width
                spacing: 10

                // Cabecalho do material
                Rectangle {
                    width: parent.width
                    height: 56
                    radius: 8
                    color: root.cardBg
                    border.color: root.borderColor
                    border.width: 1
                    Rectangle {
                        x: 14; y: 13
                        width: 30; height: 30; radius: 15
                        color: root.materialsModel.baseColor
                        border.color: Qt.darker(root.materialsModel.baseColor, 1.6)
                        border.width: 1
                        Rectangle { x: 6; y: 5; width: 9; height: 9; radius: 4.5; color: "#ffffff"; opacity: 0.35 }
                    }
                    Text {
                        x: 56; y: 10
                        width: parent.width - 56 - 220 // deixa espaco pros chips de acao
                        text: root.materialsModel.name + (root.materialsModel.selectedModified ? "  ●" : "")
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        x: 56; y: 30
                        width: parent.width - 56 - 220
                        text: "usado por " + root.materialsModel.meshCount +
                              (root.materialsModel.meshCount === 1 ? " mesh · " : " meshes · ") +
                              root.materialsModel.vramText + " em texturas"
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    // Acoes de cena (F2): picking reverso + isolar as meshes do material.
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Chip {
                            label: "Selecionar na cena"
                            onTapped: root.materialsModel.selectInScene()
                        }
                        Chip {
                            label: root.materialsModel.isolating ? "Isolando ●" : "Isolar"
                            active: root.materialsModel.isolating
                            onTapped: root.materialsModel.setIsolate(!root.materialsModel.isolating)
                        }
                    }
                }

                // ---- Base ----
                Card {
                    title: "Base"
                    iconName: "sun"
                    keywords: "cor base metálico rugosidade força do ao emissivo"
                    ColorRow {
                        label: "Cor base"
                        value: root.materialsModel.baseColor
                        onPushed: (c) => root.materialsModel.baseColor = c
                    }
                    SliderRow {
                        label: "Metálico"
                        from: 0; to: 1
                        boundValue: root.materialsModel.metallic
                        valueText: root.fmt(root.materialsModel.metallic)
                        onMoved: (v) => root.materialsModel.metallic = v
                    }
                    SliderRow {
                        label: "Rugosidade"
                        from: 0; to: 1
                        boundValue: root.materialsModel.roughness
                        valueText: root.fmt(root.materialsModel.roughness)
                        onMoved: (v) => root.materialsModel.roughness = v
                    }
                    // Sem "Força do AO": o SceneLoader força AOStrength = 0 em todo material
                    // cozido de propósito, porque somar o AO da textura por cima de GTAO + DDGI +
                    // oclusão por raio escurece a cena inteira. O slider existia e não fazia nada.
                    ColorRow {
                        label: "Emissivo"
                        value: root.materialsModel.emissiveColor
                        onPushed: (c) => root.materialsModel.emissiveColor = c
                    }
                    SliderRow {
                        label: "Força do emissivo"
                        from: 0; to: 20
                        boundValue: root.materialsModel.emissiveStrength
                        valueText: "× " + root.fmt(root.materialsModel.emissiveStrength, 1)
                        onMoved: (v) => root.materialsModel.emissiveStrength = v
                    }
                    // Separa "brilha na tela" de "ilumina o ambiente": sem ray tracing, marcar
                    // texel como emissivo era efeito local de destaque, sem consequência. Com RT
                    // esses texels passam a iluminar de verdade, e material autorado como detalhe
                    // decorativo começa a lavar a cena. Baixar aqui tira a malha do indireto sem
                    // apagar o brilho dela. Também é o par do "Peso no indireto" da luz: quando a
                    // malha e uma luz analítica representam a mesma fonte, uma das duas tem que
                    // ceder, senão a energia entra duas vezes no indireto.
                    SliderRow {
                        label: "Emissivo no indireto"
                        from: 0; to: 1
                        boundValue: root.materialsModel.rtEmissiveScale
                        valueText: root.materialsModel.rtEmissiveScale <= 0.001
                                     ? "não ilumina"
                                     : "× " + root.fmt(root.materialsModel.rtEmissiveScale, 2)
                        onMoved: (v) => root.materialsModel.rtEmissiveScale = v
                    }
                }

                // ---- Texturas (F2: clique troca o mapa; × limpa o slot) ----
                Card {
                    title: "Texturas"
                    iconName: "copy"
                    keywords: "texturas albedo normal metalrough ao emissivo height metal rough mapa"
                    Grid {
                        width: parent.width
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 6
                        Repeater {
                            model: root.materialsModel.textureSlots
                            delegate: Rectangle {
                                id: slotCell
                                required property var modelData
                                required property int index
                                width: (propsCol.width - 28 - 8) / 2
                                height: 40
                                radius: 6
                                color: slotHover.hovered ? C.Theme.surface : C.Theme.shell
                                border.color: slotHover.hovered ? root.blueBorder
                                            : modelData.has ? root.borderColor : "#20211b"
                                border.width: 1
                                opacity: modelData.has || slotHover.hovered ? 1.0 : 0.55
                                Rectangle {
                                    x: 10; y: 13
                                    width: 14; height: 14; radius: 3
                                    color: slotCell.modelData.has ? root.blueBg : "transparent"
                                    border.color: slotCell.modelData.has ? root.blueBorder
                                                                : C.Theme.hairlineStrong
                                    border.width: 1
                                    // dot ambar: slot com override do editor
                                    Rectangle {
                                        visible: slotCell.modelData.overridden
                                        x: 9; y: -3
                                        width: 8; height: 8; radius: 4
                                        color: root.amber
                                        border.color: C.Theme.shell
                                        border.width: 1
                                    }
                                }
                                Text {
                                    x: 34; y: 6
                                    text: slotCell.modelData.label
                                    color: slotCell.modelData.has ? root.textNormal : root.textMuted
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 10
                                    font.weight: slotCell.modelData.has ? Font.Medium : Font.Normal
                                }
                                Text {
                                    x: 34; y: 21
                                    width: parent.width - 42
                                    text: slotCell.modelData.info
                                    color: root.textMuted
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 9
                                    elide: Text.ElideRight
                                }
                                // limpar o slot (so em cima de slot preenchido)
                                Text {
                                    id: clearBtn
                                    visible: slotCell.modelData.has && slotHover.hovered
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    y: 3
                                    text: "×"
                                    color: clearHover.hovered ? "#e0885a" : root.textSecondary
                                    font.pixelSize: 13
                                    HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: root.materialsModel.clearSlot(slotCell.index) }
                                }
                                HoverHandler { id: slotHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        // clique no × nao deve abrir o dialogo
                                        if (!clearHover.hovered)
                                            root.materialsModel.browseSlotTexture(slotCell.index)
                                    }
                                }
                            }
                        }
                    }
                    Text {
                        width: parent.width
                        text: "clique troca o mapa (png/jpg/bmp/dds) · × volta pro shader sem o mapa · ● = override"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                    }
                }

                // ---- Normal ----
                Card {
                    title: "Normal"
                    iconName: "mountain"
                    keywords: "normal força flip y directx bc5 reconstruir z"
                    dimmed: !root.materialsModel.hasNormalMap
                    SliderRow {
                        label: "Força"
                        from: 0; to: 3
                        interactive: root.materialsModel.hasNormalMap
                        boundValue: root.materialsModel.normalStrength
                        valueText: root.fmt(root.materialsModel.normalStrength)
                        onMoved: (v) => root.materialsModel.normalStrength = v
                    }
                    ToggleRow {
                        label: "Flip Y (mapa DirectX)"
                        interactive: root.materialsModel.hasNormalMap
                        checked: root.materialsModel.normalFlipY
                        onToggled: root.materialsModel.normalFlipY = !root.materialsModel.normalFlipY
                    }
                    ToggleRow {
                        label: "BC5 → reconstruir Z"
                        hint: "mapa só RG"
                        interactive: root.materialsModel.hasNormalMap
                        checked: root.materialsModel.normalReconstructZ
                        onToggled: root.materialsModel.normalReconstructZ = !root.materialsModel.normalReconstructZ
                    }
                }

                // ---- Parallax (POM) ----
                Card {
                    title: "Parallax (POM)"
                    iconName: "box"
                    keywords: "parallax pom altura passos de frente rasante auto-sombra refino binário relevo"
                    dimmed: !root.materialsModel.hasHeightMap
                    headerItem: Text {
                        visible: !root.materialsModel.hasHeightMap
                        text: "sem height map"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    SliderRow {
                        label: "Altura"
                        from: 0; to: 0.2; stepSize: 0.005
                        interactive: root.materialsModel.hasHeightMap
                        boundValue: root.materialsModel.heightScale
                        valueText: root.fmt(root.materialsModel.heightScale, 3)
                        onMoved: (v) => root.materialsModel.heightScale = v
                    }
                    SliderRow {
                        label: "Passos (de frente)"
                        from: 1; to: 32; stepSize: 1
                        interactive: root.materialsModel.hasHeightMap
                        boundValue: root.materialsModel.parallaxMinSteps
                        valueText: root.materialsModel.parallaxMinSteps.toString()
                        onMoved: (v) => root.materialsModel.parallaxMinSteps = v
                    }
                    SliderRow {
                        label: "Passos (rasante)"
                        from: 4; to: 64; stepSize: 1
                        interactive: root.materialsModel.hasHeightMap
                        boundValue: root.materialsModel.parallaxMaxSteps
                        valueText: root.materialsModel.parallaxMaxSteps.toString()
                        onMoved: (v) => root.materialsModel.parallaxMaxSteps = v
                    }
                    ToggleRow {
                        label: "Auto-sombra"
                        hint: "sombra suave do próprio relevo"
                        interactive: root.materialsModel.hasHeightMap
                        checked: root.materialsModel.parallaxSelfShadow
                        onToggled: root.materialsModel.parallaxSelfShadow = !root.materialsModel.parallaxSelfShadow
                    }
                    ToggleRow {
                        label: "Refino binário"
                        hint: "interseção mais nítida"
                        interactive: root.materialsModel.hasHeightMap
                        checked: root.materialsModel.parallaxRefine
                        onToggled: root.materialsModel.parallaxRefine = !root.materialsModel.parallaxRefine
                    }
                }

                // ---- Transparência ----
                Card {
                    title: "Transparência"
                    iconName: "eye"
                    keywords: "transparência alpha test masked corte de alpha translúcido blend"
                    ToggleRow {
                        label: "Alpha test (masked)"
                        hint: "clip pelo alpha do albedo"
                        checked: root.materialsModel.alphaTest
                        onToggled: root.materialsModel.alphaTest = !root.materialsModel.alphaTest
                    }
                    SliderRow {
                        label: "Corte de alpha"
                        from: 0; to: 1
                        interactive: root.materialsModel.alphaTest
                        boundValue: root.materialsModel.alphaCutoff
                        valueText: root.fmt(root.materialsModel.alphaCutoff)
                        onMoved: (v) => root.materialsModel.alphaCutoff = v
                        // O cutoff e uma das entradas do bake do micro-map de opacidade. O valor
                        // acompanha o arraste (raster e recorte por raio), mas o rebake — que
                        // reconstroi BLAS — so sai quando o controle e solto.
                        onCommitted: root.materialsModel.commitOmmInputs()
                    }
                    ToggleRow {
                        label: "Translúcido (blend)"
                        hint: "sai do G-buffer → passe forward"
                        checked: root.materialsModel.blend
                        onToggled: root.materialsModel.blend = !root.materialsModel.blend
                    }
                }

                // ---- Shading ----
                Card {
                    title: "Shading"
                    iconName: "leaf"
                    keywords: "shading defaultlit folhagem subsurface two-sided cor de subsurface transmissão"
                    Row {
                        spacing: 6
                        visible: root.matchParam("shading defaultlit folhagem subsurface")
                        Chip {
                            label: "DefaultLit"
                            active: root.materialsModel.shadingModel === 0
                            onTapped: root.materialsModel.shadingModel = 0
                        }
                        Chip {
                            label: "Folhagem"
                            active: root.materialsModel.shadingModel === 1
                            onTapped: root.materialsModel.shadingModel = 1
                        }
                        // Sólido translúcido (vela/cortina/cera): transmissão direta + brilho
                        // de trás no GI; mantém GTAO/ReSTIR (folhagem não).
                        Chip {
                            label: "Subsurface"
                            active: root.materialsModel.shadingModel === 2
                            onTapped: root.materialsModel.shadingModel = 2
                        }
                    }
                    ToggleRow {
                        label: "Two-sided"
                        hint: "desenha as costas (cull off)"
                        checked: root.materialsModel.twoSided
                        onToggled: root.materialsModel.twoSided = !root.materialsModel.twoSided
                    }
                    ColorRow {
                        visible: root.materialsModel.shadingModel >= 1 && root.matchParam("cor de subsurface")
                        label: "Cor de subsurface"
                        value: root.materialsModel.subsurfaceColor
                        onPushed: (c) => root.materialsModel.subsurfaceColor = c
                    }
                    SliderRow {
                        visible: root.materialsModel.shadingModel >= 1 && root.matchParam("transmissão")
                        label: "Transmissão"
                        from: 0; to: 1
                        boundValue: root.materialsModel.subsurfaceIntensity
                        valueText: root.fmt(root.materialsModel.subsurfaceIntensity)
                        onMoved: (v) => root.materialsModel.subsurfaceIntensity = v
                    }
                }

                // rodape: onde os overrides vao parar
                Text {
                    width: parent.width
                    text: "Overrides salvos em <cena>.materials.json (sidecar, junto do .visibility.json)"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }
}
