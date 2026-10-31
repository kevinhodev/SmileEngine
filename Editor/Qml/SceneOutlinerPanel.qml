pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Scene Outliner — dock ESQUERDO no conceito Cartao 18%. O painel e so hierarquia,
// busca e visibilidade; as propriedades da selecao vivem no InspectorPanel (dock direito),
// no split classico de engine outliner|viewport|inspector.
//
// Dependencias explicitas da raiz (chegam por setInitialProperties):
//  - `outlinerModel` (SceneOutlinerBridge): arvore flat da cena (Ambiente/Luzes/Meshes por
//    asset), busca, filtro, expand/colapso e selecao de luz/mesh sincronizada com o picking.
//  - `lightsModel` (LightsBridge): acoes de luz da arvore (add/duplicar/remover/toggle) e o
//    salvamento do .lights.json.
//  - `cloudsModel` (CloudsBridge): o olho da linha de nuvens le/escreve o estado real.
//  - `sceneDoc` (SceneDocument): camada autorada (.smap) — precisa estar declarada aqui,
//    passar a propriedade so do lado C++ deixa `sceneDoc` undefined e o sceneDoc.save() vira
//    ReferenceError, que aborta o handler em silencio.
Rectangle {
    id: root
    required property var outlinerModel
    required property var lightsModel
    required property var cloudsModel
    required property var sceneDoc

    color: C.Theme.shell
    focus: true

    // Atalhos (valem quando o foco NAO esta num campo de texto do painel):
    // Del remove, Ctrl+D duplica, setas navegam a arvore, F enquadra a camera.
    // Luz e mesh sao selecoes mutuamente exclusivas, entao cada tecla so pode cair num
    // dos dois ramos — quem decide e qual das duas esta ativa.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Delete && root.hasLightSelection) {
            lightsModel.removeLight(lightsModel.selectedIndex)
            event.accepted = true
        } else if (event.key === Qt.Key_Delete && root.hasMeshSelection) {
            outlinerModel.deleteSelectedMesh()
            event.accepted = true
        } else if (event.key === Qt.Key_D && (event.modifiers & Qt.ControlModifier)
                   && root.hasLightSelection) {
            lightsModel.duplicateLight(lightsModel.selectedIndex)
            event.accepted = true
        } else if (event.key === Qt.Key_D && (event.modifiers & Qt.ControlModifier)
                   && root.hasMeshSelection) {
            outlinerModel.duplicateSelectedMesh()
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

    readonly property bool hasLightSelection: lightsModel.selectedIndex >= 0
    readonly property bool hasMeshSelection: outlinerModel.meshSelected

    // Contagens sao DADO: separador de milhar no formato do usuario (digitos tabulares da UI).
    function fmtCount(n) { return n.toLocaleString(Qt.locale("pt-BR"), 'f', 0) }

    // Cor do icone pelo TIPO do objeto, nao pelo estado: ambar = luz, verde-agua = ambiente,
    // azul = mesh. A selecao nao recolore o icone — quem marca a selecao e a faixa da linha.
    function iconTint(name) {
        if (name === "lightbulb" || name === "cone") return C.Theme.amber
        if (name === "box") return C.Theme.blue
        if (name === "leaf") return C.Theme.green
        if (name === "folder" || name === "list-tree") return C.Theme.textMuted
        return C.Theme.teal
    }

    // ---- Componentes locais ----

    // Botaozinho de icone com hover (linhas da lista, header).
    component IconButton: Item {
        id: ib
        property string icon
        property color tint: C.Theme.textMuted
        property color hoverTint: C.Theme.textPrimary
        property string tip
        signal clicked()
        width: 22; height: 22
        Rectangle {
            anchors.fill: parent; radius: C.Theme.radiusControl
            color: ibHover.hovered ? C.Theme.raisedHover : "transparent"
        }
        LucideIcon {
            anchors.centerIn: parent
            name: ib.icon; size: 13
            color: ibHover.hovered ? ib.hoverTint : ib.tint
        }
        HoverHandler { id: ibHover; cursorShape: Qt.PointingHandCursor }
        ToolTip.visible: ib.tip !== "" && ibHover.hovered
        ToolTip.delay: 600
        ToolTip.text: ib.tip
        TapHandler { onTapped: ib.clicked() }
    }

    // Olho de visibilidade das linhas da arvore (aberto/cortado).
    component EyeButton: Item {
        id: eye
        property bool on: true
        property string tip
        signal clicked()
        width: 22; height: 22
        Rectangle {
            anchors.fill: parent; radius: C.Theme.radiusControl
            color: eyeHover.hovered ? C.Theme.raisedHover : "transparent"
        }
        LucideIcon {
            anchors.centerIn: parent
            name: eye.on ? "eye" : "eye-off"
            size: 14
            color: eye.on ? (eyeHover.hovered ? C.Theme.textPrimary : C.Theme.textMuted)
                          : C.Theme.textDisabled
        }
        HoverHandler { id: eyeHover; cursorShape: Qt.PointingHandCursor }
        ToolTip.visible: eye.tip !== "" && eyeHover.hovered
        ToolTip.delay: 600
        ToolTip.text: eye.tip
        TapHandler { onTapped: eye.clicked() }
    }

    // Segmento do filtro: os quatro dividem um trilho rebaixado; o ativo sobe em cinza claro.
    component FilterChip: Rectangle {
        id: chip
        property string label
        property int value: 0
        readonly property bool active: root.outlinerModel.filter === value
        height: 22
        radius: C.Theme.radiusChip
        color: active ? C.Theme.raisedOn
                      : (chipHover.hovered ? C.Theme.surfaceHover : "transparent")
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.active ? C.Theme.textPrimary : C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textBody
            font.weight: Font.Medium
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.outlinerModel.filter = chip.value }
    }

    // Botao "+ Point" / "+ Spot" do header do grupo Luzes: botao em relevo, compacto.
    component AddLightChip: Rectangle {
        id: addChip
        property string label
        signal clicked()
        width: addChipText.implicitWidth + 14
        height: 20
        radius: C.Theme.radiusControl
        color: addChipHover.hovered ? C.Theme.raisedHover : C.Theme.surfaceRaised
        Text {
            id: addChipText
            anchors.centerIn: parent
            text: addChip.label
            color: addChipHover.hovered ? C.Theme.textPrimary : C.Theme.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textMeta
            font.weight: Font.Medium
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        HoverHandler { id: addChipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: addChip.clicked() }
    }

    // Fio escuro da divisa com o viewport (continua o separador do QMainWindow).
    Rectangle {
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 1
        color: C.Theme.hairlineStrong
        z: 1
    }

    C.DockPanelHeader {
        id: header
        anchors.left: parent.left; anchors.right: parent.right
        title: "Cena"
        focused: root.activeFocus || searchInput.activeFocus
        detail: root.outlinerModel.totalCount > 0 ? root.fmtCount(root.outlinerModel.totalCount) : ""
        actions: Component {
            Row {
                spacing: 2

                Item {
                    id: saveBtn
                    width: 24; height: 24
                    opacity: root.lightsModel.hasSceneFile ? 1.0 : 0.35
                    Rectangle {
                        anchors.fill: parent; radius: C.Theme.radiusControl
                        color: saveHover.hovered && root.lightsModel.hasSceneFile
                               ? C.Theme.raisedHover : "transparent"
                    }
                    LucideIcon {
                        anchors.centerIn: parent
                        name: "save"; size: 14
                        color: saveHover.hovered && root.lightsModel.hasSceneFile
                               ? C.Theme.textPrimary : C.Theme.textSecondary
                    }
                    Rectangle {
                        visible: root.sceneDoc.dirty || root.lightsModel.dirty || root.outlinerModel.dirty
                        x: 15; y: 2
                        width: 7; height: 7; radius: 3.5
                        color: C.Theme.accent
                        border.color: C.Theme.chrome
                        border.width: 1
                    }
                    HoverHandler {
                        id: saveHover
                        cursorShape: root.lightsModel.hasSceneFile ? Qt.PointingHandCursor
                                                              : Qt.ArrowCursor
                    }
                    TapHandler {
                        enabled: root.lightsModel.hasSceneFile
                        onTapped: {
                            root.lightsModel.saveLights()
                            root.outlinerModel.saveVisibility()
                            root.sceneDoc.save()
                        }
                    }
                    ToolTip.visible: saveHover.hovered
                    ToolTip.delay: 600
                    ToolTip.text: root.lightsModel.hasSceneFile
                                  ? "Salvar cena (<cena>.smap) + luzes"
                                  : "Carregue uma cena para salvar"
                }
                IconButton {
                    icon: "x"
                    tip: "Fechar painel"
                    onClicked: root.outlinerModel.closePanel()
                }
            }
        }
    }

    // ---- Busca ----
    Rectangle {
        id: searchBox
        anchors.top: header.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 10; anchors.rightMargin: 10
        anchors.topMargin: 10
        height: 26
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        border.color: searchInput.activeFocus ? C.Theme.accent : "transparent"
        border.width: 1

        LucideIcon {
            x: 10; anchors.verticalCenter: parent.verticalCenter
            name: "search"; size: 14
            color: C.Theme.textMuted
        }
        TextInput {
            id: searchInput
            anchors.fill: parent
            anchors.leftMargin: 32
            anchors.rightMargin: clearSearch.visible ? 30 : 58
            verticalAlignment: TextInput.AlignVCenter
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textBody
            selectByMouse: true
            selectionColor: C.Theme.accent
            selectedTextColor: C.Theme.accentInk
            onTextEdited: root.outlinerModel.search = text
            Text {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: searchInput.text === "" && !searchInput.activeFocus
                text: "Buscar na cena"
                color: C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: C.Theme.textBody
            }
        }
        // Atalho impresso no campo, como tecla: mostra a estrutura em vez de esconde-la.
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            visible: searchInput.text === "" && !searchInput.activeFocus
            width: keyCap.implicitWidth + 10
            height: 16
            radius: C.Theme.radiusChip
            color: C.Theme.surfaceRaised
            Text {
                id: keyCap
                anchors.centerIn: parent
                text: "Ctrl K"
                color: C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: C.Theme.textMeta - 1
                font.weight: Font.Medium
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
        }
        IconButton {
            id: clearSearch
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            visible: searchInput.text !== ""
            icon: "x"
            onClicked: { searchInput.text = ""; root.outlinerModel.search = "" }
        }
    }

    Shortcut {
        sequences: [StandardKey.Find, "Ctrl+K"]
        onActivated: { searchInput.forceActiveFocus(); searchInput.selectAll() }
    }

    // ---- Filtro (controle segmentado) ----
    Rectangle {
        id: chipsRow
        anchors.top: searchBox.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.topMargin: 8
        anchors.leftMargin: 10; anchors.rightMargin: 10
        height: 26
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        Row {
            id: segments
            anchors.fill: parent
            anchors.margins: 2
            readonly property real segmentWidth: width / 4
            // valores casam com SceneOutlinerBridge: 0 = Tudo, senao EGroup+1
            FilterChip { width: segments.segmentWidth; label: "Tudo";     value: 0 }
            FilterChip { width: segments.segmentWidth; label: "Luzes";    value: 2 }
            FilterChip { width: segments.segmentWidth; label: "Meshes";   value: 3 }
            FilterChip { width: segments.segmentWidth; label: "Ambiente"; value: 1 }
        }
    }

    // ---- Arvore da cena ----
    // Sem card: no conceito a arvore encosta nas bordas do dock e a unica moldura e a
    // hairline do rodape. Isso e o que devolve a largura util que o card comia.
    Item {
        id: treeArea
        anchors.top: chipsRow.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.topMargin: 10
        opacity: root.outlinerModel.available ? 1.0 : 0.45

        // estado vazio
        Text {
            anchors.centerIn: parent
            width: parent.width - 48
            visible: tree.count === 0
            text: root.outlinerModel.search !== ""
                  ? "Nada na cena casa com a busca."
                  : "Carregue uma cena (Ctrl+O) para povoar o outliner."
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textBody
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            lineHeight: 1.3
        }

        ListView {
            id: tree
            anchors.fill: parent
            clip: true
            model: root.outlinerModel
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
            Component.onCompleted: root.outlinerModel.ScrollToRequested.connect(tree.scrollToRow)

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
                readonly property bool isMore: kind === 5
                readonly property bool isBranch: isGroup || isAsset
                // O grupo Luzes troca a contagem pelos chips + POINT/+ SPOT: os dois na mesma
                // margem direita se sobrepunham.
                readonly property bool isSelectedLightsGroup: isGroup && sceneIdx === 1
                // olho: ambiente liga nos toggles reais (bindings vivos); luz/mesh/pasta
                // usam o estado da linha (Enabled da luz / Visible do renderable)
                readonly property bool eyeOn: isEnv ? (sceneIdx === 1 ? root.cloudsModel.enabled
                                                     : sceneIdx === 2 ? root.outlinerModel.oceanVisible
                                                     : sceneIdx === 3 ? root.outlinerModel.terrainVisible
                                                     : true)
                                            : rowEnabled
                readonly property bool dimmed: hasEye && !eyeOn

                width: ListView.view.width
                height: C.Theme.rowHeight
                // Selecao: faixa accent de baixa opacidade + barra de 2 px na borda. Sem raio
                // e sem borda em volta — a linha vai de ponta a ponta.
                color: selected ? C.Theme.accentBg
                     : rowHover.hovered ? C.Theme.rowHover : "transparent"

                // Guias de indentacao: um fio por nivel, na coluna do chevron do pai.
                Repeater {
                    model: rowItem.depth
                    delegate: Rectangle {
                        required property int index
                        x: 17 + index * 16
                        width: 1
                        height: rowItem.height
                        color: C.Theme.treeGuide
                    }
                }

                Rectangle {
                    visible: rowItem.selected
                    anchors.left: parent.left
                    width: 2; height: parent.height
                    color: C.Theme.accent
                }

                // chevron (grupos e pastas)
                LucideIcon {
                    id: chevron
                    visible: rowItem.isBranch
                    x: 11 + rowItem.depth * 16
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-right"
                    size: 12
                    color: C.Theme.textMuted
                    rotation: rowItem.expanded ? 90 : 0
                    Behavior on rotation { NumberAnimation { duration: 100 } }
                }

                // dot da cor da luz
                Rectangle {
                    visible: rowItem.hasDot
                    x: 14 + rowItem.depth * 16 + 15
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8; height: 8; radius: 4
                    color: rowItem.dotColor
                    border.color: Qt.darker(color, 1.6)
                    border.width: 1
                    opacity: rowItem.dimmed ? 0.35 : 1.0
                }

                LucideIcon {
                    id: rowIcon
                    visible: !rowItem.isMore
                    x: 11 + rowItem.depth * 16 + 16 + (rowItem.hasDot ? 13 : 0)
                    anchors.verticalCenter: parent.verticalCenter
                    name: rowItem.icon
                    size: 15
                    color: rowItem.dimmed ? C.Theme.textDisabled : root.iconTint(rowItem.icon)
                }

                Text {
                    id: rowLabel
                    x: rowItem.isMore ? 27 + rowItem.depth * 16
                                      : rowIcon.x + rowIcon.width + 7
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, rightActions.x - x - 4)
                    text: rowItem.isMore
                          ? "+ " + root.fmtCount(rowItem.count)
                            + (rowItem.count === 1 ? " objeto…" : " objetos…")
                          : rowItem.name
                    color: rowItem.isMore
                         ? (rowHover.hovered ? C.Theme.textPrimary : C.Theme.textMuted)
                         : rowItem.dimmed ? C.Theme.textDisabled
                         : rowItem.selected || rowItem.isGroup ? C.Theme.textPrimary
                         : C.Theme.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: C.Theme.textBody
                    font.weight: rowItem.isGroup ? Font.DemiBold
                               : rowItem.selected ? Font.Medium
                               : Font.Normal
                    elide: Text.ElideRight
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.HighRenderTypeQuality
                    // Nome de mesh cozida estoura a largura do dock com folga; o tooltip e o
                    // unico jeito de ler o nome inteiro sem alargar o painel.
                    ToolTip.visible: rowHover.hovered && rowLabel.truncated
                    ToolTip.delay: 700
                    ToolTip.text: rowItem.name
                }

                Row {
                    id: rightActions
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    // Contagem de grupos/pastas: numero cru em texto meta, sem badge (o badge
                    // duplicava a moldura que a propria linha ja da). Fica DENTRO da faixa de
                    // acoes para o olho nao cair por cima dela e para o rotulo poder medir a
                    // largura livre por rightActions.x.
                    Text {
                        visible: rowItem.count >= 0 && rowItem.isBranch
                                 && !rowItem.isSelectedLightsGroup
                        anchors.verticalCenter: parent.verticalCenter
                        rightPadding: rowItem.hasEye ? 2 : 6
                        text: root.fmtCount(rowItem.count)
                        color: C.Theme.textMuted
                        font.family: C.Theme.fontNum
                        font.pixelSize: C.Theme.textMeta
                        renderType: Text.QtRendering
                        renderTypeQuality: Text.HighRenderTypeQuality
                    }

                    // + POINT / + SPOT no header do grupo Luzes
                    AddLightChip {
                        visible: rowItem.isGroup && rowItem.sceneIdx === 1
                        anchors.verticalCenter: parent.verticalCenter
                        label: "+ Point"
                        onClicked: root.lightsModel.addLight(0)
                    }
                    AddLightChip {
                        visible: rowItem.isGroup && rowItem.sceneIdx === 1
                        anchors.verticalCenter: parent.verticalCenter
                        label: "+ Spot"
                        onClicked: root.lightsModel.addLight(1)
                    }

                    // acoes de luz no hover
                    IconButton {
                        visible: rowItem.isLight && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "copy"; tip: "Duplicar"
                        onClicked: root.lightsModel.duplicateLight(rowItem.sceneIdx)
                    }
                    IconButton {
                        visible: rowItem.isLight && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "trash-2"; tip: "Remover"
                        hoverTint: C.Theme.error
                        onClicked: root.lightsModel.removeLight(rowItem.sceneIdx)
                    }

                    // acoes de mesh no hover. Selecionam a linha antes de agir: as duas acoes
                    // do outliner operam sobre a SELECAO (o Id que o renderer tem ancorado), e
                    // clicar no botao de uma linha ja significa escolher aquela linha.
                    IconButton {
                        visible: rowItem.isMesh && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "copy"; tip: "Duplicar"
                        onClicked: {
                            root.outlinerModel.selectRow(rowItem.index)
                            root.outlinerModel.duplicateSelectedMesh()
                        }
                    }
                    IconButton {
                        visible: rowItem.isMesh && rowHover.hovered
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "trash-2"; tip: "Remover"
                        hoverTint: C.Theme.error
                        onClicked: {
                            root.outlinerModel.selectRow(rowItem.index)
                            root.outlinerModel.deleteSelectedMesh()
                        }
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
                                root.lightsModel.toggleLightEnabled(rowItem.sceneIdx)
                            else if (rowItem.isMesh || rowItem.isAsset)
                                root.outlinerModel.toggleEye(rowItem.index)
                            else if (rowItem.sceneIdx === 1)
                                root.cloudsModel.SetEnabled(!root.cloudsModel.enabled)
                            else if (rowItem.sceneIdx === 2)
                                root.outlinerModel.oceanVisible = !root.outlinerModel.oceanVisible
                            else if (rowItem.sceneIdx === 3)
                                root.outlinerModel.terrainVisible = !root.outlinerModel.terrainVisible
                        }
                    }
                }

                HoverHandler {
                    id: rowHover
                    cursorShape: rowItem.isMore ? Qt.PointingHandCursor
                               : rowItem.isEnv && rowItem.sceneIdx !== 1
                                 && rowItem.sceneIdx !== 2 && rowItem.sceneIdx !== 4
                                  ? Qt.ArrowCursor : Qt.PointingHandCursor
                }
                TapHandler {
                    // os handlers dos botoes (olho/duplicar/lixeira) NAO tomam grab exclusivo,
                    // entao o tap da linha tambem dispara — ignora a faixa das acoes.
                    onTapped: (ep) => {
                        if (ep.position.x >= rightActions.x) return
                        root.forceActiveFocus() // atalhos de teclado voltam pro painel
                        if (rowItem.isMore)            root.outlinerModel.showMore(rowItem.index)
                        else if (rowItem.isBranch)     root.outlinerModel.toggleExpand(rowItem.index)
                        else if (rowItem.isEnv &&
                                 (rowItem.sceneIdx === 1 || rowItem.sceneIdx === 2
                                  || rowItem.sceneIdx === 4))
                                                         root.outlinerModel.selectRow(rowItem.index)
                        else if (rowItem.isLight)      root.lightsModel.selectLight(rowItem.sceneIdx)
                        else if (rowItem.isMesh)       root.outlinerModel.selectRow(rowItem.index)
                    }
                    // duplo-clique enquadra a camera no objeto (estilo tecla F da UE)
                    onDoubleTapped: (ep) => {
                        if (ep.position.x >= rightActions.x) return
                        if (rowItem.isLight || rowItem.isMesh)
                            root.outlinerModel.focusRow(rowItem.index)
                    }
                }
            }
        }
    }

    // ---- Rodape com contagens ----
    Item {
        id: footer
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top
            height: 1; color: C.Theme.hairline
        }
        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: {
                var t = root.fmtCount(root.outlinerModel.totalCount) + " objetos"
                if (root.outlinerModel.selectedCount > 0)
                    t += " · " + root.outlinerModel.selectedCount + " selecionado"
                if (root.outlinerModel.hiddenCount > 0)
                    t += " · " + root.outlinerModel.hiddenCount + " ocultos"
                return t
            }
            color: C.Theme.textMuted
            font.family: C.Theme.fontNum
            font.pixelSize: C.Theme.textMeta
            elide: Text.ElideRight
            width: parent.width - 24
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }
}
