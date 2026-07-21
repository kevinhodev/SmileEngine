import QtQuick
import QtQuick.Controls
import "components" as C

// Scene Outliner (dock lateral, substitui o antigo painel de Luzes). Liga em tres bridges:
//  - `outlinerModel` (SceneOutlinerBridge): arvore flat da cena (Ambiente/Luzes/Meshes por
//    asset), busca, filtro, expand/colapso e selecao de luz/mesh sincronizada com o picking.
//  - `lightsModel` (LightsBridge): acoes e propriedades de luz (add/duplicar/remover/toggle,
//    cor HSV, sliders, sombras, posicao) — mesmo contrato do painel antigo.
//  - `viewportModel` (ViewportWidget): toggle das nuvens (olho da linha de ambiente).
// Estilo do TimeOfDayPanel (dark, cards, accent azul).
Rectangle {
    id: root
    color: "#141511"
    focus: true

    // Atalhos (valem quando o foco NAO esta num campo de texto do painel):
    // Del remove a luz, Ctrl+D duplica, setas navegam a arvore, F enquadra a camera.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Delete && root.hasLightSelection) {
            lightsModel.removeLight(lightsModel.selectedIndex)
            event.accepted = true
        } else if (event.key === Qt.Key_D && (event.modifiers & Qt.ControlModifier)
                   && root.hasLightSelection) {
            lightsModel.duplicateLight(lightsModel.selectedIndex)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            outlinerModel.selectStep(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            outlinerModel.selectStep(1)
            event.accepted = true
        } else if (event.key === Qt.Key_F && event.modifiers === Qt.NoModifier) {
            var r = outlinerModel.selectedRowIndex()
            if (r >= 0) outlinerModel.focusRow(r)
            event.accepted = true
        }
    }

    readonly property color cardBg: C.Theme.cardBg
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color bulbColor: "#e8c565"

    readonly property bool hasLightSelection: lightsModel.selectedIndex >= 0
    readonly property bool hasMeshSelection: outlinerModel.meshSelected

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }

    // ---- Componentes locais (mesma linguagem do TimeOfDayPanel/SettingsWindow) ----
    component Toggle: C.Toggle {}
    component Card: C.Card { contentSpacing: 10 }
    component SliderRow: C.SliderRow {}

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

    // Olho de visibilidade das linhas da arvore (aberto/cortado).
    component EyeButton: Item {
        id: eye
        property bool on: true
        property string tip
        signal clicked()
        width: 22; height: 22
        Rectangle {
            anchors.fill: parent; radius: 5
            color: eyeHover.hovered ? "#23241d" : "transparent"
        }
        LucideIcon {
            anchors.centerIn: parent
            name: eye.on ? "eye" : "eye-off"
            size: 13
            color: eye.on ? (eyeHover.hovered ? root.textPrimary : root.textSecondary)
                          : root.textMuted
        }
        HoverHandler { id: eyeHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: eye.clicked() }
        ToolTip.visible: eye.tip !== "" && eyeHover.hovered
        ToolTip.delay: 600
        ToolTip.text: eye.tip
    }

    // Chip de filtro (Tudo/Luzes/Meshes/Ambiente).
    component FilterChip: Rectangle {
        id: chip
        property string label
        property int value: 0
        readonly property bool active: outlinerModel.filter === value
        width: chipText.implicitWidth + 20
        height: 20
        radius: 10
        color: active ? "#1c2438" : (chipHover.hovered ? "#20211a" : root.cardBg)
        border.color: active ? "#2c3f63" : root.borderColor
        border.width: 1
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.active ? root.blue : root.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.DemiBold
        }
        HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: outlinerModel.filter = chip.value }
    }

    // Linha rotulo/valor do card de propriedades da mesh.
    component PropRow: Item {
        property string label
        property string value
        width: parent.width
        height: 18
        Text {
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            text: parent.label
            color: root.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 90
            horizontalAlignment: Text.AlignRight
            text: parent.value
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
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
            name: "list-tree"; size: 15
            color: outlinerModel.totalCount > 0 ? root.blue : root.textMuted
        }
        Text {
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            text: "Cena"
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        // badge com a contagem total de objetos
        Rectangle {
            x: 70
            anchors.verticalCenter: parent.verticalCenter
            visible: outlinerModel.totalCount > 0
            width: countLabel.implicitWidth + 12
            height: 16
            radius: 8
            color: "#23241d"
            Text {
                id: countLabel
                anchors.centerIn: parent
                text: outlinerModel.totalCount.toLocaleString(Qt.locale("pt-BR"), 'f', 0)
                color: root.textSecondary
                font.family: "Segoe UI"
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
        }

        // salvar luzes (ponto ambar = mudancas nao salvas)
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
                visible: lightsModel.dirty || outlinerModel.dirty
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
            TapHandler {
                enabled: lightsModel.hasSceneFile
                onTapped: {
                    lightsModel.saveLights()
                    outlinerModel.saveVisibility()
                }
            }
            ToolTip.visible: saveHover.hovered
            ToolTip.delay: 600
            ToolTip.text: lightsModel.hasSceneFile
                          ? "Salvar luzes e visibilidade (<cena>.lights/.visibility.json)"
                          : "Carregue uma cena para salvar"
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
            TapHandler { onTapped: outlinerModel.closePanel() }
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1; color: root.divider
        }
    }

    // ---- Busca ----
    Rectangle {
        id: searchBox
        anchors.top: header.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 12
        anchors.topMargin: 10
        height: 28
        radius: 6
        color: "#111209"
        border.color: searchInput.activeFocus ? root.blue : root.borderColor
        border.width: 1

        LucideIcon {
            x: 9; anchors.verticalCenter: parent.verticalCenter
            name: "search"; size: 13
            color: root.textMuted
        }
        TextInput {
            id: searchInput
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: clearSearch.visible ? 28 : 9
            verticalAlignment: TextInput.AlignVCenter
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
            selectByMouse: true
            selectionColor: root.blue
            onTextEdited: outlinerModel.search = text
            Text {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: searchInput.text === "" && !searchInput.activeFocus
                text: "Buscar na cena…"
                color: root.textMuted
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
        }
        IconButton {
            id: clearSearch
            anchors.right: parent.right
            anchors.rightMargin: 3
            anchors.verticalCenter: parent.verticalCenter
            visible: searchInput.text !== ""
            icon: "x"
            onClicked: { searchInput.text = ""; outlinerModel.search = "" }
        }
    }

    // ---- Chips de filtro ----
    Row {
        id: chipsRow
        anchors.top: searchBox.bottom
        anchors.left: parent.left
        anchors.topMargin: 8
        anchors.leftMargin: 12
        spacing: 6
        // valores casam com SceneOutlinerBridge: 0 = Tudo, senao EGroup+1
        FilterChip { label: "Tudo";     value: 0 }
        FilterChip { label: "Luzes";    value: 2 }
        FilterChip { label: "Meshes";   value: 3 }
        FilterChip { label: "Ambiente"; value: 1 }
    }

    // ---- Arvore da cena ----
    Rectangle {
        id: treeCard
        anchors.top: chipsRow.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: propsArea.top
        anchors.margins: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1
        opacity: outlinerModel.available ? 1.0 : 0.45

        // estado vazio
        Text {
            anchors.centerIn: parent
            width: parent.width - 48
            visible: tree.count === 0
            text: outlinerModel.search !== ""
                  ? "Nada na cena casa com a busca."
                  : "Carregue uma cena (Ctrl+O) para povoar o outliner."
            color: root.textMuted
            font.family: "Segoe UI"
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.25
        }

        ListView {
            id: tree
            anchors.fill: parent
            anchors.margins: 6
            anchors.bottomMargin: footer.height + 6
            clip: true
            model: outlinerModel
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            ScrollBar.vertical: ThinScrollBar {}

            // selecao veio do viewport (picking) ou das setas: garante a linha na area
            // visivel. Conexao IMPERATIVA pelo nome exato: os sinais das bridges comecam
            // com maiuscula (ScrollToRequested) e o Connections procura a versao
            // minuscula (scrollToRequested) — o handler declarativo nunca dispara.
            // Qt.callLater: o pedido pode chegar logo apos um reset do modelo
            // (RevealSelection -> Rebuild) e o positionViewAtIndex sincrono posiciona
            // com o layout velho.
            function scrollToRow(row) {
                Qt.callLater(function() {
                    tree.positionViewAtIndex(row, ListView.Contain)
                })
            }
            Component.onCompleted: outlinerModel.ScrollToRequested.connect(tree.scrollToRow)

            delegate: Rectangle {
                id: rowItem
                required property int index
                required property int kind
                required property int depth
                required property string name
                required property string icon
                required property int count
                required property bool expanded
                required property bool rowEnabled
                required property bool hasEye
                required property color dotColor
                required property bool hasDot
                required property bool selected
                required property int sceneIdx

                readonly property bool isGroup: kind === 0
                readonly property bool isEnv: kind === 1
                readonly property bool isLight: kind === 2
                readonly property bool isAsset: kind === 3
                readonly property bool isMesh: kind === 4
                readonly property bool isBranch: isGroup || isAsset
                // olho: ambiente liga nos toggles reais (bindings vivos); luz/mesh/pasta
                // usam o estado da linha (Enabled da luz / Visible do renderable)
                readonly property bool eyeOn: isEnv ? (sceneIdx === 1 ? viewportModel.cloudsEnabled
                                                     : sceneIdx === 2 ? outlinerModel.oceanVisible
                                                     : sceneIdx === 3 ? outlinerModel.terrainVisible
                                                     : true)
                                            : rowEnabled
                readonly property bool dimmed: hasEye && !eyeOn

                width: ListView.view.width
                height: isGroup ? 30 : 26
                radius: 6
                color: selected ? "#1d2743"
                     : rowHover.hovered ? "#20211a" : "transparent"
                border.color: selected ? "#33477a" : "transparent"
                border.width: 1

                // chevron (grupos e pastas)
                LucideIcon {
                    id: chevron
                    visible: rowItem.isBranch
                    x: 4 + rowItem.depth * 14
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-right"
                    size: 12
                    color: root.textSecondary
                    rotation: rowItem.expanded ? 90 : 0
                    Behavior on rotation { NumberAnimation { duration: 100 } }
                }

                // dot da cor da luz
                Rectangle {
                    visible: rowItem.hasDot
                    x: 6 + rowItem.depth * 14 + 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8; height: 8; radius: 4
                    color: rowItem.dotColor
                    border.color: Qt.darker(color, 1.6)
                    border.width: 1
                    opacity: rowItem.dimmed ? 0.35 : 1.0
                }

                LucideIcon {
                    id: rowIcon
                    x: 6 + rowItem.depth * 14 + (rowItem.isBranch ? 16 : 0)
                       + (rowItem.hasDot ? 13 : 0)
                    anchors.verticalCenter: parent.verticalCenter
                    name: rowItem.icon
                    size: rowItem.isGroup ? 13 : 12
                    color: rowItem.dimmed ? root.textMuted
                         : rowItem.isGroup && rowItem.sceneIdx === 1 ? root.bulbColor
                         : rowItem.isGroup ? root.textNormal
                         : rowItem.isLight ? root.bulbColor
                         : rowItem.isAsset ? "#c9a86a"
                         : rowItem.icon === "leaf" ? "#7fc98f"
                         : rowItem.isEnv ? root.textNormal
                         : root.textSecondary
                }

                Text {
                    id: rowLabel
                    x: rowIcon.x + rowIcon.width + 7
                    anchors.verticalCenter: parent.verticalCenter
                    width: rightActions.x - x - 4
                    text: rowItem.name
                    color: rowItem.dimmed ? root.textMuted
                         : rowItem.selected ? root.textPrimary
                         : rowItem.isGroup ? root.textPrimary
                         : root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                    font.weight: rowItem.isGroup ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                }

                // badge de contagem (grupos/pastas)
                Rectangle {
                    visible: rowItem.count >= 0 && rowItem.isBranch
                    x: Math.min(rowLabel.x + rowLabel.implicitWidth + 8,
                                rightActions.x - width - 4)
                    anchors.verticalCenter: parent.verticalCenter
                    width: badgeText.implicitWidth + 10
                    height: 14
                    radius: 7
                    color: "#23241d"
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: rowItem.count.toLocaleString(Qt.locale("pt-BR"), 'f', 0)
                        color: root.textSecondary
                        font.family: "Segoe UI"
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                    }
                }

                Row {
                    id: rightActions
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1

                    // + Point / + Spot no header do grupo Luzes
                    Rectangle {
                        visible: rowItem.isGroup && rowItem.sceneIdx === 1
                        width: addPointRow.implicitWidth + 14; height: 18; radius: 9
                        anchors.verticalCenter: parent.verticalCenter
                        color: addPointHover.hovered ? "#20304e" : "#1c2438"
                        border.color: "#2c3f63"
                        border.width: 1
                        Row {
                            id: addPointRow
                            anchors.centerIn: parent
                            spacing: 2
                            LucideIcon {
                                name: "plus"; size: 9; color: root.blue
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "Point"
                                color: root.blue
                                font.family: "Segoe UI"
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        HoverHandler { id: addPointHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: lightsModel.addLight(0) }
                    }
                    Rectangle {
                        visible: rowItem.isGroup && rowItem.sceneIdx === 1
                        width: addSpotRow.implicitWidth + 14; height: 18; radius: 9
                        anchors.verticalCenter: parent.verticalCenter
                        color: addSpotHover.hovered ? "#20304e" : "#1c2438"
                        border.color: "#2c3f63"
                        border.width: 1
                        Row {
                            id: addSpotRow
                            anchors.centerIn: parent
                            spacing: 2
                            LucideIcon {
                                name: "plus"; size: 9; color: root.blue
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "Spot"
                                color: root.blue
                                font.family: "Segoe UI"
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        HoverHandler { id: addSpotHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: lightsModel.addLight(1) }
                    }

                    // acoes de luz no hover
                    IconButton {
                        visible: rowItem.isLight && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "copy"; tip: "Duplicar"
                        onClicked: lightsModel.duplicateLight(rowItem.sceneIdx)
                    }
                    IconButton {
                        visible: rowItem.isLight && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "trash-2"; tip: "Remover"
                        hoverTint: "#e07a6a"
                        onClicked: lightsModel.removeLight(rowItem.sceneIdx)
                    }

                    // olho de visibilidade
                    EyeButton {
                        visible: rowItem.hasEye
                        anchors.verticalCenter: parent.verticalCenter
                        on: rowItem.eyeOn
                        tip: rowItem.isLight ? "Ligar/desligar a luz"
                           : rowItem.isMesh ? "Mostrar/ocultar a mesh (raster + GI)"
                           : rowItem.isAsset ? "Mostrar/ocultar todas as meshes do asset"
                           : rowItem.sceneIdx === 1 ? "Ligar/desligar as nuvens"
                           : rowItem.sceneIdx === 2 ? "Ligar/desligar o oceano"
                           : "Mostrar/ocultar o terreno"
                        onClicked: {
                            if (rowItem.isLight)
                                lightsModel.toggleLightEnabled(rowItem.sceneIdx)
                            else if (rowItem.isMesh || rowItem.isAsset)
                                outlinerModel.toggleEye(rowItem.index)
                            else if (rowItem.sceneIdx === 1)
                                viewportModel.SetCloudsEnabled(!viewportModel.cloudsEnabled)
                            else if (rowItem.sceneIdx === 2)
                                outlinerModel.oceanVisible = !outlinerModel.oceanVisible
                            else if (rowItem.sceneIdx === 3)
                                outlinerModel.terrainVisible = !outlinerModel.terrainVisible
                        }
                    }
                }

                HoverHandler {
                    id: rowHover
                    cursorShape: rowItem.isEnv && rowItem.sceneIdx !== 1 && rowItem.sceneIdx !== 2
                                 ? Qt.ArrowCursor : Qt.PointingHandCursor
                }
                TapHandler {
                    // os handlers dos botoes (olho/duplicar/lixeira) NAO tomam grab exclusivo,
                    // entao o tap da linha tambem dispara — ignora a faixa das acoes.
                    onTapped: (ep) => {
                        if (ep.position.x >= rightActions.x) return
                        root.forceActiveFocus() // atalhos de teclado voltam pro painel
                        if (rowItem.isBranch)          outlinerModel.toggleExpand(rowItem.index)
                        else if (rowItem.isLight)      lightsModel.selectLight(rowItem.sceneIdx)
                        else if (rowItem.isMesh)       outlinerModel.selectRow(rowItem.index)
                    }
                    // duplo-clique enquadra a camera no objeto (estilo tecla F da UE)
                    onDoubleTapped: (ep) => {
                        if (ep.position.x >= rightActions.x) return
                        if (rowItem.isLight || rowItem.isMesh)
                            outlinerModel.focusRow(rowItem.index)
                    }
                }
            }
        }

        // rodape com contagens
        Item {
            id: footer
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 24
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top
                height: 1; color: root.divider
            }
            Text {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    var t = outlinerModel.totalCount.toLocaleString(Qt.locale("pt-BR"), 'f', 0)
                            + " objetos"
                    if (outlinerModel.selectedCount > 0)
                        t += "  ·  " + outlinerModel.selectedCount + " selecionado"
                    if (outlinerModel.hiddenCount > 0)
                        t += "  ·  " + outlinerModel.hiddenCount + " ocultos"
                    return t
                }
                color: root.textMuted
                font.family: "Segoe UI"
                font.pixelSize: 9
            }
        }
    }

    // ---- Propriedades da selecao ----
    Flickable {
        id: propsArea
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        anchors.topMargin: 0
        height: Math.min(propsCol.implicitHeight, root.height * 0.52)
        contentHeight: propsCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: propsCol
            width: parent.width
            spacing: 10
            opacity: outlinerModel.available ? 1.0 : 0.45

            // ---- Luz selecionada (mesmo contrato do painel antigo) ----
            Card {
                title: lightsModel.lightType === 1 ? "Propriedades — Spot" : "Propriedades — Point"
                iconName: lightsModel.lightType === 1 ? "cone" : "lightbulb"
                visible: root.hasLightSelection
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
                    // Conexoes imperativas: mesmo racional do scrollToRow — os sinais
                    // SelectionChanged/LightChanged (maiuscula) nao casam com handlers
                    // declarativos de Connections (bug latente herdado do LightsPanel:
                    // o picker nao re-sincronizava ao trocar de luz).
                    Component.onCompleted: {
                        syncFromModel()
                        lightsModel.SelectionChanged.connect(picker.syncFromModel)
                        lightsModel.LightChanged.connect(function() {
                            if (!svDrag.active && !hueDrag.active) picker.syncFromModel()
                        })
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
                    label: "Raio da fonte (área)"
                    valueText: root.fmt(lightsModel.sourceRadius, 2) + " m"
                    from: 0.01; to: 1.5
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

            // ---- Mesh selecionada ----
            Card {
                title: "Propriedades — Mesh"
                iconName: "box"
                visible: !root.hasLightSelection && root.hasMeshSelection

                Text {
                    width: parent.width
                    text: outlinerModel.meshName
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                PropRow {
                    label: "Material"
                    value: outlinerModel.meshMaterial !== "" ? outlinerModel.meshMaterial : "—"
                }
                PropRow {
                    label: "Triângulos"
                    value: outlinerModel.meshTris.toLocaleString(Qt.locale("pt-BR"), 'f', 0)
                }
                PropRow {
                    label: "Vértices"
                    value: outlinerModel.meshVerts.toLocaleString(Qt.locale("pt-BR"), 'f', 0)
                }
                PropRow {
                    label: "Geometria"
                    value: outlinerModel.meshVramText
                }

                // flags do material (folhagem/two-sided/masked/translucido)
                Flow {
                    width: parent.width
                    spacing: 5
                    visible: outlinerModel.meshFlags.length > 0
                    Repeater {
                        model: outlinerModel.meshFlags
                        delegate: Rectangle {
                            required property string modelData
                            width: flagText.implicitWidth + 14
                            height: 18
                            radius: 9
                            color: "#23241d"
                            border.color: "#33342c"
                            border.width: 1
                            Text {
                                id: flagText
                                anchors.centerIn: parent
                                text: modelData
                                color: root.textSecondary
                                font.family: "Segoe UI"
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: "Duplo-clique na árvore (ou F) enquadra a câmera."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }
            }

            // dica quando nada selecionado
            Card {
                title: "Propriedades"
                iconName: "info"
                visible: !root.hasLightSelection && !root.hasMeshSelection

                Text {
                    width: parent.width
                    text: "Selecione um objeto na árvore ou clique nele no viewport."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
