pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Janela de configuracoes do editor. Renderizacao, iluminacao global e sombras/ceu usam
// RenderSettingsBridge; as demais categorias preservam a estrutura para conexao posterior.
Rectangle {
    id: root
    required property var viewportModel
    required property var renderModel
    required property var restirModel
    required property var cameraBookmarks
    required property var capture
    required property var settingsWindow

    color: C.Theme.shell
    border.color: C.Theme.hairlineStrong
    border.width: 1
    implicitWidth: 960
    implicitHeight: 640

    property int selectedPage: 0
    property string searchText: ""

    readonly property color bg: C.Theme.bg
    readonly property color sidebarBg: C.Theme.shell
    readonly property color cardBg: C.Theme.cardBg
    readonly property color hoverBg: C.Theme.surfaceRaised
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color blueBg: C.Theme.blueBg
    readonly property color blueBorder: C.Theme.blueBorder
    readonly property color accent: C.Theme.accent
    readonly property color accentBg: C.Theme.accentBg
    readonly property color accentBorder: C.Theme.accentBorder
    readonly property color green: C.Theme.green
    // Cores de acento dos vendors, usadas so nas marcas tipograficas dos upscalers.
    readonly property color amdRed: "#e8483c"
    readonly property color nvidiaGreen: "#8cc63f"

    function pageTitle() {
        const titles = [
            "Renderização", "Iluminação global", "Path tracer", "Reflexos e denoise",
            "Pós-processo", "Sombras e céu", "Interface", "Atalhos", "Projeto"
        ]
        return titles[selectedPage]
    }

    function pageSubtitle() {
        if (selectedPage === 0)
            return "Upscaling, Anti-Aliasing e Resolução Interna do Viewport"
        if (selectedPage === 1)
            return "Estimador de iluminação indireta, volume DDGI e ferramentas avançadas de validação"
        if (selectedPage === 5)
            return "Sombras do sol (CSM), sun shafts e volumetric fog: cascatas, cache, bias e debug"
        return "Esta categoria será conectada aos controles do engine em uma próxima etapa"
    }

    function matchesSearch(label) {
        return searchText.length === 0 ||
               label.toLocaleLowerCase().indexOf(searchText.toLocaleLowerCase()) >= 0
    }

    // ---- Componentes compartilhados (Editor/Qml/components) ----
    component Toggle: C.Toggle {}
    // Segmentado por chips. Escolhido em vez de um ShadowSlider com rótulo por posição, que é o
    // padrão desta página para os outros enums: primário e fallback NÃO são escala de qualidade —
    // são estimadores diferentes, e trocar entre eles invalida acumulador. Um slider desenharia
    // uma ordem que não existe, e "arrastar até o meio" leria como meio-termo entre dois.
    component Chip: C.Chip {}

    component ShadowSlider: Item {
        id: srow
        property string label
        property real from: 0
        property real to: 1
        property real step: 0.01
        property real value: 0
        property string valueText: ""
        signal committed(real v)
        // Emitido ao soltar o mouse ou a cada passo de teclado. Quem escreve em estado caro
        // (que invalida historicos ou reconstroi o model da propria lista) deve usar este,
        // nao o committed continuo durante um drag.
        signal released(real v)
        height: 46

        Text {
            x: 0; y: 0
            text: srow.label
            color: root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
        }
        Rectangle {
            anchors.right: parent.right
            y: -2
            width: 62; height: 20; radius: 4
            color: C.Theme.surfaceRaised
            border.color: root.borderColor
            Text {
                anchors.centerIn: parent
                text: srow.valueText
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }
        Slider {
            id: sctl
            x: 0; y: 20
            width: parent.width - 70
            height: 18
            from: srow.from
            to: srow.to
            stepSize: srow.step
            onMoved: {
                srow.committed(value)
                // Slider::moved tambem cobre setas/PageUp/PageDown. Nao existe um ciclo
                // pressed/released nesse caminho, entao faz o commit caro aqui uma vez por passo.
                if (!pressed)
                    srow.released(value)
            }
            onPressedChanged: if (!pressed) srow.released(value)
            background: Rectangle {
                x: sctl.leftPadding
                y: sctl.topPadding + sctl.availableHeight / 2 - height / 2
                width: sctl.availableWidth
                height: 4
                radius: 2
                color: C.Theme.surfaceRaised
                Rectangle {
                    width: sctl.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: root.blue
                }
            }
            handle: Rectangle {
                x: sctl.leftPadding + sctl.visualPosition * (sctl.availableWidth - width)
                y: sctl.topPadding + sctl.availableHeight / 2 - height / 2
                width: 14; height: 14; radius: 7
                color: "#f2efe6"
                border.color: root.blue
                border.width: 1.5
            }
        }
        // O binding declarativo quebraria no primeiro drag; religa quando solta (padrao do painel TOD).
        Binding {
            target: sctl
            property: "value"
            value: srow.value
            when: !sctl.pressed
            restoreMode: Binding.RestoreBindingOrValue
        }
    }

    component NavItem: Rectangle {
        id: nav
        property int page
        property string glyph
        property string label
        property bool beta: false

        width: 216
        height: 30
        radius: C.Theme.radiusControl
        visible: root.matchesSearch(label)
        color: root.selectedPage === page ? root.accentBg
                                          : (navHover.hovered ? C.Theme.surface : "transparent")
        border.color: root.selectedPage === page ? root.accentBorder : "transparent"
        border.width: 1

        Rectangle {
            visible: root.selectedPage === nav.page
            x: 0; y: 4
            width: 3; height: 22; radius: 1.5
            color: root.accent
        }
        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: nav.glyph
            color: root.selectedPage === nav.page ? root.accent : root.textMuted
            font.family: "Segoe UI Symbol"
            font.pixelSize: 14
        }
        Text {
            id: navLabel
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            text: nav.label
            color: root.selectedPage === nav.page ? root.textPrimary : root.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
        }
        Rectangle {
            visible: nav.beta
            anchors.left: navLabel.right
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 34; height: 14; radius: 7
            color: "#33260f"
            border.color: "#57431a"
            Text {
                anchors.centerIn: parent
                text: "beta"
                color: "#d8a03a"
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
            }
        }
        HoverHandler { id: navHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.selectedPage = nav.page }
    }

    component ActionButton: Rectangle {
        id: abtn
        property string label
        signal tapped()
        width: abtnText.implicitWidth + 26
        height: 26
        radius: 6
        color: abtnHover.hovered ? C.Theme.surfaceRaised : "transparent"
        border.color: C.Theme.hairlineStrong
        border.width: 1
        Text {
            id: abtnText
            anchors.centerIn: parent
            text: abtn.label
            color: root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        HoverHandler { id: abtnHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: abtn.tapped() }
    }

    component UpscalerOption: Rectangle {
        id: option
        property int mode: 0
        property string label
        property string detail
        property string badge
        property bool selected: false
        property bool available: true
        signal chosen()

        height: 48
        radius: C.Theme.radiusCard
        color: option.selected ? root.accentBg
                               : (optionHover.hovered && option.available ? root.hoverBg : "transparent")
        border.color: option.selected ? root.accentBorder : "transparent"
        border.width: 1
        opacity: option.available ? 1.0 : 0.46

        Rectangle {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30; height: 30; radius: C.Theme.radiusControl
            color: option.selected ? C.Theme.accentBgStrong : C.Theme.surfaceRaised
            border.color: option.selected ? C.Theme.accentBorder : C.Theme.hairlineStrong
            clip: true
            Text {
                anchors.centerIn: parent
                visible: option.mode === 1
                text: "FSR"
                color: root.amdRed
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
                font.weight: Font.Bold
            }
            Text {
                anchors.centerIn: parent
                visible: option.mode === 2
                text: "DLSS"
                color: root.nvidiaGreen
                font.family: C.Theme.fontFamily
                font.pixelSize: 8
                font.weight: Font.Bold
            }
            Text {
                anchors.centerIn: parent
                visible: option.mode === 0
                text: "1:1"
                color: option.selected ? root.accent : root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
        Text {
            x: 48; y: 8
            text: option.label
            color: option.selected ? root.textPrimary : root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Text {
            x: 48; y: 26
            width: parent.width - 154
            text: option.detail
            color: root.textMuted
            elide: Text.ElideRight
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }
        Rectangle {
            visible: option.badge.length > 0
            anchors.right: selectedMark.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: optionBadge.implicitWidth + 18
            height: 20
            radius: 10
            color: option.badge === "RECOMENDADO" ? "#1d3424" : "#25271f"
            border.color: option.badge === "RECOMENDADO" ? "#36563d" : "#3a3c33"
            Text {
                id: optionBadge
                anchors.centerIn: parent
                text: option.badge
                color: option.badge === "RECOMENDADO" ? root.green : root.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }
        Text {
            id: selectedMark
            anchors.right: parent.right
            anchors.rightMargin: 13
            anchors.verticalCenter: parent.verticalCenter
            text: option.selected ? "✓" : ""
            color: root.accent
            font.family: "Segoe UI Symbol"
            font.pixelSize: 13
        }
        HoverHandler {
            id: optionHover
            enabled: option.available
            cursorShape: option.available ? Qt.PointingHandCursor : Qt.ForbiddenCursor
        }
        TapHandler {
            enabled: option.available
            onTapped: option.chosen()
        }
    }

    // Linha do seletor de denoiser (Nenhum / NRD RELAX / DLSS Ray Reconstruction). Espelha o
    // UpscalerOption, mas com a marca tipografica do denoiser (OFF/NRD/RR).
    component DenoiserOption: Rectangle {
        id: dopt
        property int mode: 0
        property string label
        property string detail
        property bool selected: false
        property bool available: true
        signal chosen()

        height: 48
        radius: C.Theme.radiusCard
        color: dopt.selected ? root.accentBg
                             : (doptHover.hovered && dopt.available ? root.hoverBg : "transparent")
        border.color: dopt.selected ? root.accentBorder : "transparent"
        border.width: 1
        opacity: dopt.available ? 1.0 : 0.46

        Rectangle {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30; height: 30; radius: C.Theme.radiusControl
            color: dopt.selected ? C.Theme.accentBgStrong : C.Theme.surfaceRaised
            border.color: dopt.selected ? C.Theme.accentBorder : C.Theme.hairlineStrong
            clip: true
            Text {
                anchors.centerIn: parent
                text: dopt.mode === 2 ? "RR" : (dopt.mode === 1 ? "NRD" : "OFF")
                color: dopt.mode === 2 ? root.nvidiaGreen
                                       : (dopt.mode === 1 ? root.textNormal : root.textMuted)
                font.family: C.Theme.fontFamily
                font.pixelSize: dopt.mode === 1 ? 8 : 9
                font.weight: Font.Bold
            }
        }
        Text {
            x: 48; y: 8
            text: dopt.label
            color: dopt.selected ? root.textPrimary : root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Text {
            x: 48; y: 26
            width: parent.width - 80
            text: dopt.detail
            color: root.textMuted
            elide: Text.ElideRight
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 13
            anchors.verticalCenter: parent.verticalCenter
            text: dopt.selected ? "✓" : ""
            color: root.accent
            font.family: "Segoe UI Symbol"
            font.pixelSize: 13
        }
        HoverHandler {
            id: doptHover
            enabled: dopt.available
            cursorShape: dopt.available ? Qt.PointingHandCursor : Qt.ForbiddenCursor
        }
        TapHandler {
            enabled: dopt.available
            onTapped: dopt.chosen()
        }
    }

    component Card: Rectangle {
        id: card
        property string title
        readonly property int headerHeight: 40
        readonly property int contentPadding: 16
        radius: C.Theme.radiusCard
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1

        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: card.headerHeight

            Text {
                x: 20
                anchors.verticalCenter: parent.verticalCenter
                text: card.title
                color: root.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: card.headerHeight
            height: 1
            color: root.divider
        }
    }

    component StatusRow: Item {
        id: statusRow
        property string label
        property string value
        height: 20
        // O valor manda: ele dimensiona primeiro e o label ocupa o que sobrar, com elide. Sem
        // isso os dois cresciam um contra o outro e o valor longo cobria o label
        // (ex.: "Draws visiveis" sumindo sob "644 / 1592 · ocl 198").
        Text {
            id: statusValue
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: statusRow.value
            color: root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Text {
            anchors.left: parent.left
            anchors.right: statusValue.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: statusRow.label
            elide: Text.ElideRight
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
    }

    C.FramelessTitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40
        windowBridge: root.settingsWindow
        title: "Configurações — SmileEngine"
        iconSource: "image://smilelogo/32"
    }

    // Sidebar.
    Rectangle {
        id: sidebar
        anchors.left: parent.left
        anchors.top: titleBar.bottom
        anchors.bottom: footer.top
        width: 240
        color: root.sidebarBg

        Rectangle {
            x: 12; y: 16
            width: 216; height: 30; radius: 6
            color: C.Theme.surface
            border.color: root.borderColor
            border.width: 1
            Text {
                x: 11
                anchors.verticalCenter: parent.verticalCenter
                text: "⌕"
                color: root.textMuted
                font.family: "Segoe UI Symbol"
                font.pixelSize: 16
            }
            TextField {
                id: searchField
                x: 30; y: 1
                width: parent.width - 36
                height: 28
                padding: 0
                placeholderText: "Buscar configuração…"
                placeholderTextColor: root.textMuted
                color: root.textNormal
                selectionColor: root.blue
                selectedTextColor: C.Theme.shell
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
                background: null
                onTextChanged: root.searchText = text
            }
        }

        Text {
            x: 20; y: 70
            text: "Gráficos"
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Column {
            x: 12; y: 84
            spacing: 4
            NavItem { page: 0; glyph: "▣"; label: "Renderização" }
            NavItem { page: 1; glyph: "☼"; label: "Iluminação global" }
            NavItem { page: 2; glyph: "⌁"; label: "Path tracer"; beta: true }
            NavItem { page: 3; glyph: "◇"; label: "Reflexos e denoise" }
            NavItem { page: 4; glyph: "☷"; label: "Pós-processo" }
            NavItem { page: 5; glyph: "☾"; label: "Sombras e céu" }
        }

        Text {
            x: 20; y: 304
            text: "Editor"
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Column {
            x: 12; y: 318
            spacing: 4
            NavItem { page: 6; glyph: "▤"; label: "Interface" }
            NavItem { page: 7; glyph: "⌨"; label: "Atalhos" }
            NavItem { page: 8; glyph: "▰"; label: "Projeto" }
        }

        Rectangle {
            x: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            width: 216; height: 52; radius: 8
            color: C.Theme.surface
            border.color: root.borderColor
            border.width: 1
            Text {
                x: 14; y: 9
                text: "Preset ativo"
                color: root.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
            Text {
                x: 14; y: 27
                text: "Ultra — RT completo"
                color: root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 13
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                color: root.textMuted
                font.family: "Segoe UI Symbol"
                font.pixelSize: 18
            }
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: root.divider
        }
    }

    // Conteudo da pagina.
    Item {
        id: pageArea
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: footer.top

        Text {
            x: 24; y: 22
            text: root.pageTitle()
            color: root.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 20
            font.weight: Font.Medium
        }
        Text {
            x: 24; y: 49
            text: root.pageSubtitle()
            color: root.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 24
            y: 28
            width: Math.min(220, gpuLabel.implicitWidth + 28)
            height: 22
            radius: 11
            color: C.Theme.surface
            border.color: root.borderColor
            border.width: 1
            Text {
                id: gpuLabel
                anchors.centerIn: parent
                width: parent.width - 20
                text: root.viewportModel.gpuName + " · DX12"
                color: root.textSecondary
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
        }

        // Duas colunas empilhadas por Column (mesmo padrao dos paineis graficos, adaptado
        // p/ 2 colunas): nenhum card sabe seu proprio y, entao mudar/ocultar conteudo reflui tudo
        // sozinho. O contentHeight segue a coluna mais alta, entao o scroll sempre alcanca o fim.
        Loader {
            id: renderingLoader
            anchors.fill: parent
            anchors.topMargin: 84
            active: root.selectedPage === 0
            sourceComponent: Component {
                Flickable {
                    id: renderingPage
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: Math.max(renderingLeftCol.height, renderingRightCol.height) + 40
                    clip: true
                    ScrollBar.vertical: ThinScrollBar { revealed: renderingPageHover.hovered }
                    HoverHandler { id: renderingPageHover }

            readonly property int rightW: 180
            readonly property int gap: 16
            readonly property int leftW: width - 48 - rightW - gap

            Column {
                id: renderingLeftCol
                x: 24
                width: renderingPage.leftW
                spacing: 16

            Card {
                id: upscalingCard
                width: parent.width
                title: "Upscaling e Anti-Aliasing"

                // Ritmo vertical do card, encadeado por binding (nada de y hardcoded): o respiro
                // no topo e na base e o mesmo (contentPadding), rotulo->controle usa gapLabel e a
                // troca de bloco usa gapSection. Mexer num espaco reflui o resto sozinho.
                readonly property int gapLabel: 8
                readonly property int gapSection: 14
                // fsrControls e nativeControls ocupam o mesmo slot (so um fica visivel por vez),
                // entao qualquer um serve de referencia p/ o fim do conteudo.
                readonly property int contentBottom: recommendedRow.visible
                    ? recommendedRow.y + recommendedRow.height
                    : fsrControls.y + fsrControls.height
                height: contentBottom + contentPadding

                Text {
                    id: techLabel
                    x: 20; y: upscalingCard.headerHeight + upscalingCard.contentPadding
                    text: "Tecnologia de Reconstrução"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Rectangle {
                    id: upscalerField
                    x: 20
                    y: techLabel.y + techLabel.height + upscalingCard.gapLabel
                    width: parent.width - 40
                    height: 44
                    radius: 7
                    color: C.Theme.surfaceRaised
                    border.color: upscalerPopup.opened || upscalerHover.hovered
                                  ? root.blueBorder : root.borderColor
                    border.width: 1

                    Rectangle {
                        x: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30; height: 28; radius: 7
                        color: "#292c25"
                        border.color: "#383a31"
                        clip: true
                        // Marca tipografica em vez de logo bitmap: nitida em qualquer DPI e nunca
                        // corta (o banner do FSR era 300 KB recortado por sourceClipRect).
                        Text {
                            anchors.centerIn: parent
                            visible: root.renderModel.upscalerMode === 1
                            text: "FSR"
                            color: root.amdRed
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: root.renderModel.upscalerMode === 2
                            text: "DLSS"
                            color: root.nvidiaGreen
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: root.renderModel.upscalerMode === 0
                            text: "1:1"
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: root.renderModel.upscalerMode === 2
                              ? "NVIDIA DLSS"
                              : root.renderModel.upscalerMode === 1
                                ? "AMD FidelityFX Super Resolution"
                                : "Sem upscaling"
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    Text {
                        x: 49; y: 24
                        width: parent.width - 174
                        text: root.renderModel.upscalerMode === 2
                              ? "DLSS · Reconstrução por IA (Super Resolution)"
                              : root.renderModel.upscalerMode === 1
                                ? "FSR 3.1 · Reconstrução Temporal"
                                : "TAA · Escala Manual de Renderização"
                        color: root.textMuted
                        elide: Text.ElideRight
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Rectangle {
                        visible: root.renderModel.upscalerMode === root.renderModel.recommendedUpscalerMode
                        anchors.right: dropdownArrow.left
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: recommendedText.implicitWidth + 16
                        height: 20; radius: 10
                        color: "#1d3424"
                        border.color: "#36563d"
                        Text {
                            id: recommendedText
                            anchors.centerIn: parent
                            text: "RECOMENDADO"
                            color: root.green
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                    }
                    Canvas {
                        id: dropdownArrow
                        anchors.right: parent.right
                        anchors.rightMargin: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 8
                        property bool expanded: upscalerPopup.opened
                        onExpandedChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = root.textSecondary
                            ctx.lineWidth = 1.4
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            if (expanded) {
                                ctx.moveTo(1, 6.5)
                                ctx.lineTo(6, 1.5)
                                ctx.lineTo(11, 6.5)
                            } else {
                                ctx.moveTo(1, 1.5)
                                ctx.lineTo(6, 6.5)
                                ctx.lineTo(11, 1.5)
                            }
                            ctx.stroke()
                        }
                    }
                    HoverHandler { id: upscalerHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: upscalerPopup.opened ? upscalerPopup.close() : upscalerPopup.open()
                    }
                }

                Text {
                    id: techHelper
                    x: 20
                    y: upscalerField.y + upscalerField.height + upscalingCard.gapLabel
                    text: root.renderModel.fsrAvailable
                          ? "O recomendado é escolhido conforme o hardware e os backends disponíveis."
                          : "FSR 3.1 indisponível; usando o caminho nativo com TAA."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }

                Item {
                    id: fsrControls
                    visible: root.renderModel.upscalerMode === 1 || root.renderModel.upscalerMode === 2
                    x: 20
                    y: techHelper.y + techHelper.height + upscalingCard.gapSection
                    width: parent.width - 40
                    height: 111

                    Text {
                        x: 0; y: 0
                        text: "Preset de qualidade"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Text {
                        anchors.right: parent.right
                        y: 1
                        // O backend selecionado e quem dita a escala interna — nao e sempre o FSR.
                        text: (root.renderModel.upscalerMode === 2 ? "DLSS" : "FSR") +
                              " controla a escala interna"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Rectangle {
                        id: qualitySelector
                        x: 0; y: 18
                        width: parent.width
                        height: 36
                        radius: 6
                        color: C.Theme.surfaceRaised
                        border.color: root.borderColor
                        border.width: 1

                        Row {
                            anchors.fill: parent
                            Repeater {
                                // Rotulo + escala de render de cada preset. "Ultra" e Ultra
                                // PERFORMANCE (a menor resolucao) — sem o sufixo ele le como
                                // "o melhor de todos", que e o oposto do que faz.
                                model: [
                                    { name: "100%",        scale: "nativo" },
                                    { name: "Qualidade",   scale: "67%"    },
                                    { name: "Balanceado",  scale: "59%"    },
                                    { name: "Performance", scale: "50%"    },
                                    { name: "Ultra Perf.", scale: "33%"    }
                                ]
                                delegate: Rectangle {
                                    id: qualityOption
                                    required property var modelData
                                    required property int index
                                    readonly property bool isCurrent: root.renderModel.upscalerQuality === index
                                    width: qualitySelector.width / 5
                                    height: qualitySelector.height
                                    radius: 6
                                    color: isCurrent ? root.accentBg
                                                     : (qualityHover.hovered
                                                        ? C.Theme.surfaceRaised : "transparent")
                                    border.color: isCurrent ? root.accentBorder : "transparent"
                                    border.width: 1
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 1
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qualityOption.modelData.name
                                            color: qualityOption.isCurrent ? root.accent : root.textSecondary
                                            font.family: C.Theme.fontFamily
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qualityOption.modelData.scale
                                            color: qualityOption.isCurrent ? C.Theme.accentText : root.textMuted
                                            opacity: qualityOption.isCurrent ? 0.85 : 1.0
                                            font.family: C.Theme.fontFamily
                                            font.pixelSize: 9
                                        }
                                    }
                                    HoverHandler {
                                        id: qualityHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: root.renderModel.SetUpscalerQuality(qualityOption.index)
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        x: 0; y: 65
                        width: parent.width
                        height: 46; radius: 7
                        color: "#151a16"
                        border.color: "#29352d"
                        Rectangle {
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 7; height: 7; radius: 3.5
                            color: root.green
                        }
                        Text {
                            x: 29; y: 8
                            text: "Reconstrução Ativa"
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 11
                        }
                        Text {
                            x: 29; y: 25
                            text: root.viewportModel.internalResolution + "  →  " + root.viewportModel.outputResolution
                            color: root.textPrimary
                            font.family: C.Theme.fontMono
                            font.pixelSize: 10
                        }
                        Rectangle {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: reductionText.implicitWidth + 16
                            height: 21; radius: 10.5
                            color: "#1b2b21"
                            border.color: "#34483a"
                            Text {
                                id: reductionText
                                anchors.centerIn: parent
                                text: root.renderModel.renderScale < 0.999
                                      ? "−" + Math.round((1.0 - root.renderModel.renderScale *
                                                         root.renderModel.renderScale) * 100) + "% Pixels"
                                      : "Resolução Integral"
                                color: root.green
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 9
                            }
                        }
                    }
                }

                Item {
                    id: nativeControls
                    visible: root.renderModel.upscalerMode === 0
                    x: 20
                    y: techHelper.y + techHelper.height + upscalingCard.gapSection
                    width: parent.width - 40
                    height: 111

                    Text {
                        x: 0; y: 2
                        text: "Anti-aliasing temporal (TAA)"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                    }
                    Toggle {
                        anchors.right: parent.right
                        y: -3
                        checked: root.renderModel.taaEnabled
                        onToggled: root.renderModel.SetTAAEnabled(!checked)
                    }
                    Rectangle { x: 0; y: 29; width: parent.width; height: 1; color: root.divider }
                    Text {
                        x: 0; y: 39
                        text: "Escala de renderização (SSAA)"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                    }
                    Rectangle {
                        anchors.right: parent.right
                        y: 33
                        width: 54; height: 22; radius: 4
                        color: C.Theme.surfaceRaised
                        border.color: root.borderColor
                        Text {
                            anchors.centerIn: parent
                            text: root.renderModel.renderScale.toFixed(2).replace(".", ",") + "×"
                            color: root.textPrimary
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }
                    Slider {
                        id: renderScaleSlider
                        x: 0; y: 58
                        width: parent.width
                        height: 18
                        from: 1.0
                        to: 2.0
                        stepSize: 0.05
                        value: root.renderModel.renderScale
                        onPressedChanged: {
                            if (!pressed) root.renderModel.SetRenderScale(value)
                        }
                        background: Rectangle {
                            x: renderScaleSlider.leftPadding
                            y: renderScaleSlider.topPadding + renderScaleSlider.availableHeight / 2 - height / 2
                            width: renderScaleSlider.availableWidth
                            height: 4; radius: 2
                            color: C.Theme.surfaceRaised
                            Rectangle {
                                width: renderScaleSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 2
                                color: root.blue
                            }
                        }
                        handle: Rectangle {
                            x: renderScaleSlider.leftPadding +
                               renderScaleSlider.visualPosition * (renderScaleSlider.availableWidth - width)
                            y: renderScaleSlider.topPadding + renderScaleSlider.availableHeight / 2 - height / 2
                            width: 14; height: 14; radius: 7
                            color: "#f2efe6"
                            border.color: root.blue
                            border.width: 1.5
                        }
                    }
                    Text {
                        x: 0; y: 84
                        text: "1,0×"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                    }
                    Text {
                        anchors.right: parent.right
                        y: 84
                        text: "2,0×"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 9
                    }
                }

                // Só aparece quando NÃO se está no backend recomendado — no recomendado o badge
                // do dropdown já comunica isso. Clicável: informar sem dar saída é beco sem saída.
                Row {
                    id: recommendedRow
                    x: 20
                    y: fsrControls.y + fsrControls.height + upscalingCard.gapLabel
                    spacing: 5
                    visible: root.renderModel.upscalerMode !== root.renderModel.recommendedUpscalerMode
                    Text {
                        text: "Recomendado para esta GPU:"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Text {
                        id: useRecommendedLink
                        text: "Usar " + root.renderModel.recommendedUpscalerName
                        color: root.blue
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                        font.underline: useRecommendedHover.hovered
                        HoverHandler {
                            id: useRecommendedHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: root.renderModel.SetUpscalerMode(root.renderModel.recommendedUpscalerMode)
                        }
                    }
                }

                Popup {
                    id: upscalerPopup
                    popupType: Popup.Item
                    x: 20; y: 119
                    width: parent.width - 40
                    height: 164
                    padding: 6
                    z: 100
                    // O campo pertence ao parent do popup: clicar nele deve chegar ao TapHandler
                    // e alternar open/close, sem o CloseOnPressOutside fechar e reabrir no mesmo tap.
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                    background: Rectangle {
                        color: "#1e201a"
                        border.color: "#3b3d34"
                        border.width: 1
                        radius: 8
                    }
                    contentItem: Column {
                        spacing: 4
                        UpscalerOption {
                            width: upscalerPopup.availableWidth
                            mode: 0
                            label: "Sem upscaling"
                            detail: "TAA · Escala Manual de 100% a 200% (SSAA)"
                            selected: root.renderModel.upscalerMode === 0
                            badge: root.renderModel.recommendedUpscalerMode === 0 ? "RECOMENDADO" : ""
                            onChosen: {
                                root.renderModel.SetUpscalerMode(0)
                                upscalerPopup.close()
                            }
                        }
                        UpscalerOption {
                            width: upscalerPopup.availableWidth
                            mode: 1
                            label: "AMD FidelityFX Super Resolution"
                            detail: available
                                    ? "FSR 3.1 · Temporal · Compatível com esta GPU"
                                    : "FSR 3.1 não foi Inicializado Neste Dispositivo"
                            selected: root.renderModel.upscalerMode === 1
                            available: root.renderModel.fsrAvailable
                            badge: root.renderModel.recommendedUpscalerMode === 1 ? "RECOMENDADO" : ""
                            onChosen: {
                                root.renderModel.SetUpscalerMode(1)
                                upscalerPopup.close()
                            }
                        }
                        UpscalerOption {
                            width: upscalerPopup.availableWidth
                            mode: 2
                            label: "NVIDIA DLSS"
                            detail: available
                                    ? "DLSS · Reconstrução por IA · Requer GPU NVIDIA RTX"
                                    : "DLSS Indisponível (requer GPU NVIDIA RTX)"
                            selected: root.renderModel.upscalerMode === 2
                            available: root.renderModel.dlssAvailable
                            badge: root.renderModel.recommendedUpscalerMode === 2 ? "RECOMENDADO" : ""
                            onChosen: {
                                root.renderModel.SetUpscalerMode(2)
                                upscalerPopup.close()
                            }
                        }
                    }
                }
            }

            // Denoiser do GI/reflexao (eixo separado do upscaler). RR acopla denoise+upscale, entao
            // fica ao lado do card de Upscaling; a ativacao saiu da viewport (era toggle NRD).
            Card {
                id: denoiserCard
                width: parent.width
                title: "Denoiser (GI e Reflexão)"

                readonly property int gapLabel: 8
                readonly property int contentBottom: denoiserHelper.y + denoiserHelper.height
                height: contentBottom + contentPadding

                Text {
                    id: denoiserLabel
                    x: 20; y: denoiserCard.headerHeight + denoiserCard.contentPadding
                    text: "Redução de Ruído do Ray Tracing"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Rectangle {
                    id: denoiserField
                    x: 20
                    y: denoiserLabel.y + denoiserLabel.height + denoiserCard.gapLabel
                    width: parent.width - 40
                    height: 44
                    radius: 7
                    color: C.Theme.surfaceRaised
                    border.color: denoiserPopup.opened || denoiserHover.hovered
                                  ? root.blueBorder : root.borderColor
                    border.width: 1

                    Rectangle {
                        x: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30; height: 28; radius: 7
                        color: "#292c25"
                        border.color: "#383a31"
                        clip: true
                        Text {
                            anchors.centerIn: parent
                            text: root.renderModel.denoiserMode === 2 ? "RR"
                                  : (root.renderModel.denoiserMode === 1 ? "NRD" : "OFF")
                            color: root.renderModel.denoiserMode === 2 ? root.nvidiaGreen
                                   : (root.renderModel.denoiserMode === 1 ? root.textNormal : root.textMuted)
                            font.family: C.Theme.fontFamily
                            font.pixelSize: root.renderModel.denoiserMode === 1 ? 8 : 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: root.renderModel.denoiserMode === 2 ? "DLSS Ray Reconstruction"
                              : (root.renderModel.denoiserMode === 1 ? "NRD RELAX" : "Nenhum")
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    Text {
                        x: 49; y: 24
                        width: parent.width - 80
                        text: root.renderModel.denoiserMode === 2
                              ? "Denoiser neural (NVIDIA) · substitui NRD + upscaler"
                              : (root.renderModel.denoiserMode === 1
                                 ? "RELAX · difuso (GI) + especular (reflexão)"
                                 : "GI e reflexão sem denoise (ruidoso)")
                        color: root.textMuted
                        elide: Text.ElideRight
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Canvas {
                        id: denoiserArrow
                        anchors.right: parent.right
                        anchors.rightMargin: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12; height: 8
                        property bool expanded: denoiserPopup.opened
                        onExpandedChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = root.textSecondary
                            ctx.lineWidth = 1.4
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            if (expanded) {
                                ctx.moveTo(1, 6.5); ctx.lineTo(6, 1.5); ctx.lineTo(11, 6.5)
                            } else {
                                ctx.moveTo(1, 1.5); ctx.lineTo(6, 6.5); ctx.lineTo(11, 1.5)
                            }
                            ctx.stroke()
                        }
                    }
                    HoverHandler { id: denoiserHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: denoiserPopup.opened ? denoiserPopup.close() : denoiserPopup.open()
                    }
                }

                Text {
                    id: denoiserHelper
                    x: 20
                    y: denoiserField.y + denoiserField.height + denoiserCard.gapLabel
                    width: parent.width - 40
                    wrapMode: Text.WordWrap
                    text: root.renderModel.rrAvailable
                          ? "Ray Reconstruction faz denoise e upscale num passo só — ao selecioná-lo, o upscaling fica travado em NVIDIA DLSS."
                          : "DLSS Ray Reconstruction requer GPU NVIDIA RTX; nesta máquina só NRD está disponível."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                    lineHeight: 1.25
                }

                Popup {
                    id: denoiserPopup
                    popupType: Popup.Item
                    x: 20
                    y: denoiserField.y + denoiserField.height + 4
                    width: parent.width - 40
                    height: 164
                    padding: 6
                    z: 100
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    background: Rectangle {
                        color: "#1e201a"; border.color: "#3b3d34"; border.width: 1; radius: 8
                    }
                    contentItem: Column {
                        spacing: 4
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 0
                            label: "Nenhum"
                            detail: "GI e reflexão ruidosos (sem denoise)"
                            selected: root.renderModel.denoiserMode === 0
                            onChosen: { root.renderModel.SetDenoiserMode(0); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 1
                            label: "NRD RELAX"
                            detail: "A-trous · difuso (GI) + especular (reflexão)"
                            selected: root.renderModel.denoiserMode === 1
                            onChosen: { root.renderModel.SetDenoiserMode(1); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 2
                            label: "DLSS Ray Reconstruction"
                            detail: available ? "IA · substitui NRD + upscaler (trava DLSS)"
                                              : "Indisponível (requer GPU NVIDIA RTX)"
                            selected: root.renderModel.denoiserMode === 2
                            available: root.renderModel.rrAvailable
                            onChosen: { root.renderModel.SetDenoiserMode(2); denoiserPopup.close() }
                        }
                    }
                }
            }

            // Preferencia real de renderizacao: trade-off que depende da cena. Nomeado pelos
            // passes (nao por "geometria") — o prepass muda a ordem dos passes, nao a geometria.
            Card {
                width: parent.width
                height: 88
                title: "Passes de Renderização"

                Text {
                    x: 20; y: 56
                    text: "Depth Prepass"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    // Ancorada ate o toggle (que e 36 de largura + 20 de margem): sem isso o
                    // texto cresce por baixo dele. elide garante degradacao limpa se nao couber.
                    y: 57
                    anchors.left: parent.left;              anchors.leftMargin: 158
                    anchors.right: prepassToggle.left;      anchors.rightMargin: 12
                    text: "troca um passe extra por menos overdraw"
                    elide: Text.ElideRight
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    id: prepassToggle
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 50
                    checked: root.renderModel.depthPrepassEnabled
                    onToggled: root.renderModel.SetDepthPrepassEnabled(!checked)
                }
            }

            // Chaves de investigacao, nao preferencias: desligar so faz sentido p/ isolar bug
            // de culling (geometria sumindo/piscando). Separadas do card acima justamente p/
            // nao parecerem ajuste de qualidade.
            Card {
                width: parent.width
                height: 148
                title: "Diagnóstico"

                Text {
                    x: 20; y: 50
                    width: parent.width - 40
                    text: "Ligados por padrão. Desligue apenas para investigar geometria sumindo " +
                          "ou piscando — com eles off a cena fica bem mais lenta."
                    wrapMode: Text.WordWrap
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }

                Text {
                    x: 20; y: 92
                    text: "Frustum Culling"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    y: 93
                    anchors.left: parent.left;          anchors.leftMargin: 158
                    anchors.right: frustumToggle.left;  anchors.rightMargin: 12
                    text: "descarta o que está fora da câmera"
                    elide: Text.ElideRight
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    id: frustumToggle
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 86
                    checked: root.renderModel.frustumCullingEnabled
                    onToggled: root.renderModel.SetFrustumCullingEnabled(!checked)
                }

                Text {
                    x: 20; y: 120
                    text: "Occlusion Culling"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    y: 121
                    anchors.left: parent.left;            anchors.leftMargin: 158
                    anchors.right: occlusionToggle.left;  anchors.rightMargin: 12
                    text: "HZB: descarta o que está atrás de parede"
                    elide: Text.ElideRight
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    id: occlusionToggle
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 114
                    checked: root.renderModel.occlusionCullingEnabled
                    onToggled: root.renderModel.SetOcclusionCullingEnabled(!checked)
                }
            }

            // Fase 3 do GPU-DRIVEN-PLAN. Card PROPRIO, e nao uma linha no de "Diagnostico":
            // aquele diz "ligados por padrao" e este nasce desligado. Aqui o toggle e o A/B do
            // plano — mesmas listas de CPU, mesmas draws, so o caminho pelo qual o pixel shader
            // alcanca o material muda.
            Card {
                width: parent.width
                height: 130
                title: "GPU-driven"

                Text {
                    x: 20; y: 50
                    width: parent.width - 40
                    text: root.renderModel.bindlessMaterialsAvailable
                          ? "Experimental. O material vai para o shader por uma tabela global em " +
                            "vez de um bind por objeto. A imagem deve ficar idêntica."
                          : "Indisponível: esta GPU não tem Resource Binding Tier 3."
                    wrapMode: Text.WordWrap
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }

                Text {
                    x: 20; y: 96
                    text: "Materiais bindless"
                    color: root.renderModel.bindlessMaterialsAvailable ? root.textNormal
                                                                       : root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Toggle {
                    id: bindlessMaterialsToggle
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 90
                    interactive: root.renderModel.bindlessMaterialsAvailable
                    checked: root.renderModel.bindlessMaterialsEnabled
                    onToggled: root.renderModel.SetBindlessMaterialsEnabled(!checked)
                }
            }

            } // renderingLeftCol

            Column {
                id: renderingRightCol
                x: 24 + renderingPage.leftW + renderingPage.gap
                width: renderingPage.rightW
                spacing: 16

            Card {
                width: parent.width
                height: 150
                title: "Desempenho"

                Text {
                    x: 16; y: 54
                    text: Math.round(root.viewportModel.fps)
                    color: root.blue
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 28
                    font.weight: Font.Medium
                }
                Text {
                    x: 66; y: 70
                    text: "FPS"
                    color: root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 16; y: 90
                    text: root.viewportModel.frameTimeMs.toFixed(1).replace(".", ",") + " ms por frame"
                    color: root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Rectangle { x: 0; y: 112; width: parent.width; height: 1; color: root.divider }
                // Mantem aqui apenas a telemetria que reage aos controles desta pagina.
                StatusRow {
                    x: 16; y: 121; width: parent.width - 32
                    label: "Res. interna"
                    value: root.viewportModel.internalResolution
                }
            }

            } // renderingRightCol
                }
            }
        }

        Loader {
            id: shadowsLoader
            anchors.fill: parent
            anchors.topMargin: 84
            active: root.selectedPage === 5
            sourceComponent: Component {
                Flickable {
                    id: shadowsPage
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: shadowsCol.height + 40
                    clip: true
                    ScrollBar.vertical: ThinScrollBar { revealed: shadowsPageHover.hovered }
                    HoverHandler { id: shadowsPageHover }

            // Coluna unica: contentHeight segue a Column.
            // e o scroll sempre alcança o último card
            Column {
                id: shadowsCol
                x: 24
                width: shadowsPage.width - 48
                spacing: 16

            Card {
                width: parent.width
                height: 364
                title: "Sombras do sol (CSM)"

                Text {
                    x: 20; y: 55
                    text: "Sombras dinâmicas"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "4 cascatas · 2048² · PCF Poisson 16 taps"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: root.renderModel.sunShadowsEnabled
                    onToggled: root.renderModel.SetSunShadowsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Distância máxima"
                    from: 100; to: 3000; step: 50
                    value: root.renderModel.shadowMaxDistance
                    valueText: Math.round(root.renderModel.shadowMaxDistance) + " m"
                    onCommitted: (v) => root.renderModel.SetShadowMaxDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Bias de profundidade"
                    from: 0; to: 8; step: 0.1
                    value: root.renderModel.shadowDepthBias
                    valueText: root.renderModel.shadowDepthBias.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => root.renderModel.SetShadowDepthBias(v)
                }
                // Em texels, como o bias — e portanto também 4× maior em metros a cada
                // cascata. Junto com o bias é o que apaga sombra de contato nas cascatas
                // distantes: 2,5 tx valem 5,9 cm na cascata 0 e 23,2 cm na 1.
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Normal offset"
                    from: 0; to: 6; step: 0.1
                    value: root.renderModel.shadowNormalOffset
                    valueText: root.renderModel.shadowNormalOffset.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => root.renderModel.SetShadowNormalOffset(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Tamanho angular do sol (PCSS)"
                    from: 0; to: 2.0; step: 0.01
                    value: root.renderModel.shadowSunAngle
                    valueText: root.renderModel.shadowSunAngle < 0.005
                               ? "off"
                               : root.renderModel.shadowSunAngle.toFixed(2).replace(".", ",") + "°"
                    onCommitted: (v) => root.renderModel.SetShadowSunAngle(v)
                }

                Text {
                    x: 20; y: 324
                    text: "Debug de cascatas"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 324
                    text: "tinge a cena pela cascata usada"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 318
                    checked: root.renderModel.shadowDebugCascades
                    onToggled: root.renderModel.SetShadowDebugCascades(!checked)
                }
            }

            Card {
                width: parent.width
                height: 500
                title: "Sun shafts"

                Text {
                    x: 20; y: 55
                    text: "Raios volumétricos (raymarch no CSM)"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raio de verdade: janela, fresta, copa — via height fog"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: root.renderModel.sunShaftsEnabled
                    onToggled: root.renderModel.SetSunShaftsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Intensidade"
                    from: 0; to: 5.0; step: 0.1
                    value: root.renderModel.sunShaftsIntensity
                    valueText: root.renderModel.sunShaftsIntensity.toFixed(1).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetSunShaftsIntensity(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Poeira — densidade do feixe"
                    from: 1; to: 64; step: 1
                    value: root.renderModel.sunShaftsDust
                    valueText: "×" + Math.round(root.renderModel.sunShaftsDust)
                    onCommitted: (v) => root.renderModel.SetSunShaftsDust(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.95; step: 0.01
                    value: root.renderModel.sunShaftsPhaseG
                    valueText: root.renderModel.sunShaftsPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetSunShaftsPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Passos do raymarch"
                    from: 8; to: 64; step: 4
                    value: root.renderModel.sunShaftsSteps
                    valueText: root.renderModel.sunShaftsSteps + ""
                    onCommitted: (v) => root.renderModel.SetSunShaftsSteps(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Alcance da marcha (m)"
                    from: 32; to: 400; step: 8
                    value: root.renderModel.sunShaftsRange
                    valueText: Math.round(root.renderModel.sunShaftsRange) + " m"
                    onCommitted: (v) => root.renderModel.SetSunShaftsRange(v)
                }

                Rectangle { x: 20; y: 372; width: parent.width - 40; height: 1; color: root.divider }

                Text {
                    x: 20; y: 390
                    text: "Acumulação temporal"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 409
                    text: "integra o ruído do raymarch ao longo dos frames"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 389
                    checked: root.renderModel.sunShaftsTemporal
                    onToggled: root.renderModel.SetSunShaftsTemporal(!checked)
                }
            }

            Card {
                width: parent.width
                height: 722
                title: "Volumetric fog"

                Text {
                    x: 20; y: 55
                    text: "Fog froxel (grid 3D estilo UE)"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "fog perto com sombra do sol e GI; além do alcance segue o analítico"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: root.renderModel.volFogEnabled
                    onToggled: root.renderModel.SetVolFogEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Alcance do volume (m)"
                    from: 30; to: 300; step: 10
                    value: root.renderModel.volFogDistance
                    valueText: Math.round(root.renderModel.volFogDistance) + " m"
                    onCommitted: (v) => root.renderModel.SetVolFogDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Densidade do volume"
                    from: 0.25; to: 8.0; step: 0.25
                    value: root.renderModel.volFogDensity
                    valueText: "×" + root.renderModel.volFogDensity.toFixed(2).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetVolFogDensity(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.9; step: 0.05
                    value: root.renderModel.volFogPhaseG
                    valueText: root.renderModel.volFogPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetVolFogPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Ambiente (DDGI/céu)"
                    from: 0; to: 4.0; step: 0.1
                    value: root.renderModel.volFogAmbient
                    valueText: "×" + root.renderModel.volFogAmbient.toFixed(1).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetVolFogAmbient(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Luzes no ar (halo de point/spot)"
                    from: 0; to: 4.0; step: 0.1
                    value: root.renderModel.volFogLights
                    valueText: "×" + root.renderModel.volFogLights.toFixed(1).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetVolFogLights(v)
                }

                Rectangle { x: 20; y: 380; width: parent.width - 40; height: 1; color: root.divider }

                Text {
                    x: 20; y: 398
                    text: "Acumulação temporal"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 417
                    text: "jitter Halton + reprojeção — desligar volta o banding em fatias"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 397
                    checked: root.renderModel.volFogTemporal
                    onToggled: root.renderModel.SetVolFogTemporal(!checked)
                }

                Text {
                    x: 20; y: 448
                    text: "Conservative depth"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 467
                    text: "pula froxel atrás de parede — só custo, não muda o visual"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 447
                    checked: root.renderModel.volFogConsDepth
                    onToggled: root.renderModel.SetVolFogConsDepth(!checked)
                }

                Rectangle { x: 20; y: 498; width: parent.width - 40; height: 1; color: root.divider }

                Text {
                    x: 20; y: 516
                    text: "Height fog analítico — céu na cor do inscatter"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                ShadowSlider {
                    x: 20; y: 546
                    width: parent.width - 40
                    label: "0 = cor chapada (antigo) · 1 = cor do céu naquela direção"
                    from: 0; to: 1.0; step: 0.05
                    value: root.renderModel.heightFogSkyContribution
                    valueText: root.renderModel.heightFogSkyContribution.toFixed(2).replace(".", ",")
                    onCommitted: (v) => root.renderModel.SetHeightFogSkyContribution(v)
                }

                Text {
                    x: 20; y: 606
                    text: "Transmitância do sol por pixel"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 625
                    text: "sol atenuado pela altitude da superfície — topo dourado, vale azul"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 605
                    checked: root.renderModel.perPixelAtmoTransmittance
                    onToggled: root.renderModel.SetPerPixelAtmoTransmittance(!checked)
                }

                Text {
                    x: 20; y: 656
                    text: "Ambiente do céu direcional (SH-L1)"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 675
                    text: "desligado = 2 cores chapadas, ambiente só varia com o Y da normal"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 655
                    checked: root.renderModel.skyAmbientSH
                    onToggled: root.renderModel.SetSkyAmbientSH(!checked)
                }
            }

            Card {
                width: parent.width
                height: 420
                title: "Cascatas — custo e bias"

                Text {
                    x: 20; y: 55
                    text: "Cache round-robin"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "cascatas 2/3 re-renderizam a cada 2/4 frames"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: root.renderModel.shadowCacheEnabled
                    onToggled: root.renderModel.SetShadowCacheEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Caster mínimo (texels da cascata)"
                    from: 0; to: 8; step: 0.5
                    value: root.renderModel.shadowMinCasterTexels
                    valueText: root.renderModel.shadowMinCasterTexels.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => root.renderModel.SetShadowMinCasterTexels(v)
                }

                Text {
                    x: 20; y: 164
                    text: "Bias por cascata (multiplicador)"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Column {
                    x: 20; y: 188
                    width: parent.width - 40
                    spacing: 8
                    Repeater {
                        model: ["Cascata 0 — perto", "Cascata 1", "Cascata 2", "Cascata 3 — longe"]
                        delegate: ShadowSlider {
                            required property string modelData
                            required property int index
                            width: parent.width
                            label: modelData
                            from: 0.25; to: 4.0; step: 0.05
                            value: Number(root.renderModel.shadowCascadeBias[index])
                            valueText: "×" + Number(root.renderModel.shadowCascadeBias[index]).toFixed(2).replace(".", ",")
                            onCommitted: (v) => root.renderModel.SetShadowCascadeBiasScale(index, v)
                        }
                    }
                }
            }
                }
            }
        }
        }

        // ---- Pagina 1: Iluminacao global ----------------------------------------------------
        // A visão normal mostra só a política operacional. Calibração, diagnóstico e protocolo
        // de captura ficam no laboratório recolhível; continuam na mesma Column para que abrir ou
        // fechar o grupo reflua a página sem coordenadas manuais.
        Loader {
            id: giLoader
            anchors.fill: parent
            anchors.topMargin: 84
            active: root.selectedPage === 1
            sourceComponent: Component {
                Flickable {
                    id: giPage
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: giCol.height + 40
                    clip: true
                    ScrollBar.vertical: ThinScrollBar { revealed: giPageHover.hovered }
                    HoverHandler { id: giPageHover }

            // SNAPSHOT do model, nao binding direto em renderModel.rayEpsilons. Escrever um
            // epsilon emite GISettingsChanged, o que re-le a propriedade e trocaria a lista do
            // Repeater — reconstruindo os delegates DEBAIXO do mouse durante o arrasto. Recarrega
            // so nos momentos discretos: abrir a pagina e restaurar padroes.
            property var epsModel: []
            // O uso cotidiano precisa de três decisões; o restante desta página existe para
            // bring-up, calibração e A/B. Mantê-lo recolhido evita transformar instrumentos de
            // laboratório em opções que parecem necessárias para configurar uma cena.
            property bool labVisible: false
            function reloadEps() {
                epsModel = root.renderModel.rayEpsilons
                biasMaxSlider.uiValue = root.renderModel.giSurfaceBiasMax
                fadeSlider.uiValue    = root.renderModel.giVolumeFadeProbes
            }
            Component.onCompleted: reloadEps()

            Column {
                id: giCol
                x: 24
                width: parent.width - 48
                spacing: 16

                Card {
                    id: giOverviewCard
                    width: parent.width
                    title: "Pipeline de iluminação indireta"
                    height: giLabButton.y + giLabButton.height + contentPadding

                    Text {
                        id: giPolicyText
                        x: 20
                        y: giOverviewCard.headerHeight + giOverviewCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: root.renderModel.indirectPolicySummary
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                    }

                    Text {
                        id: giPrimaryLabel
                        x: 20
                        y: giPolicyText.y + giPolicyText.height + 14
                        text: "Estimador primário"
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                    }
                    Row {
                        id: giPrimaryRow
                        x: 20
                        y: giPrimaryLabel.y + giPrimaryLabel.height + 6
                        spacing: 6
                        Chip {
                            label: "ReSTIR + SHaRC"
                            active: root.renderModel.indirectPrimary === 0
                            onTapped: root.renderModel.SetIndirectPrimary(0)
                        }
                        Chip {
                            label: "DDGI"
                            active: root.renderModel.indirectPrimary === 1
                            onTapped: root.renderModel.SetIndirectPrimary(1)
                        }
                        Chip {
                            label: "Off"
                            active: root.renderModel.indirectPrimary === 2
                            onTapped: root.renderModel.SetIndirectPrimary(2)
                        }
                    }

                    Text {
                        id: giDdgiLabel
                        x: 20
                        y: giPrimaryRow.y + giPrimaryRow.height + 18
                        text: "Volume DDGI"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Text {
                        id: giDdgiHint
                        x: 20
                        y: giDdgiLabel.y + giDdgiLabel.height + 2
                        text: "fallback, névoa e modo DDGI"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Toggle {
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        y: giDdgiLabel.y - 3
                        checked: root.renderModel.ddgiEnabled
                        onToggled: root.renderModel.ToggleDDGI()
                    }

                    Text {
                        id: giRestirLabel
                        x: 20
                        y: giDdgiHint.y + giDdgiHint.height + 16
                        text: "ReSTIR GI"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Text {
                        id: giRestirHint
                        x: 20
                        y: giRestirLabel.y + giRestirLabel.height + 2
                        text: "produtor do modo ReSTIR + SHaRC"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Toggle {
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        y: giRestirLabel.y - 3
                        checked: root.renderModel.restirGIEnabled
                        onToggled: root.renderModel.ToggleReSTIRGI()
                    }

                    Text {
                        id: giRestirHalfLabel
                        x: 20
                        y: giRestirHint.y + giRestirHint.height + 16
                        text: "ReSTIR GI meia-res"
                        color: root.renderModel.restirGIEnabled ? root.textNormal : root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Text {
                        id: giRestirHalfHint
                        x: 20
                        y: giRestirHalfLabel.y + giRestirHalfLabel.height + 2
                        text: "¼ dos pixels + reconstrução temporal"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Toggle {
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        y: giRestirHalfLabel.y - 3
                        enabled: root.renderModel.restirGIEnabled
                        checked: root.renderModel.restirGIHalfRes
                        onToggled: root.renderModel.ToggleReSTIRGIHalfRes()
                    }

                    StatusRow {
                        id: giCacheStatus
                        x: 20
                        y: giRestirHalfHint.y + giRestirHalfHint.height + 16
                        width: parent.width - 40
                        label: "World radiance cache"
                        value: !root.renderModel.radianceCacheEnabled ? "desligado"
                               : (root.renderModel.radianceCacheQuery &&
                                  root.renderModel.radianceCacheAutoWarmup ? "automático"
                                                                     : "configuração manual")
                    }

                    ActionButton {
                        id: giLabButton
                        x: 20
                        y: giCacheStatus.y + giCacheStatus.height + 16
                        label: giPage.labVisible ? "Ocultar experimentos DDGI"
                                                 : "Experimentos DDGI"
                        onTapped: giPage.labVisible = !giPage.labVisible
                    }
                    ActionButton {
                        x: giLabButton.x + giLabButton.width + 8
                        y: giLabButton.y
                        label: "Abrir ReSTIR Lab"
                        onTapped: root.restirModel.OpenWindow()
                    }
                }


                // Card proprio, e nao mais um knob da tabela de epsilons: isto nao e um valor
                // continuo p/ varrer, e sim uma troca de REGIME da TLAS (reconstroi a estrutura).
                Card {
                    id: rtCullCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Culling nos reflexos"
                    height: cullLabel.y + cullLabel.height + contentPadding + 8

                    Text {
                        id: cullHelper
                        x: 20
                        y: rtCullCard.headerHeight + rtCullCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "A cena de ray tracing marca como two-sided só o que realmente é " +
                              "— folhagem, cutouts e vidro, o mesmo critério do rasterizador. " +
                              "Cada passe decide se descarta o verso das outras superfícies.\n\n" +
                              "Esta chave vale só para os raios de reflexo. O gather da " +
                              "iluminação indireta nunca descarta o verso: é assim que ele evita " +
                              "que o raio atravesse a casca de um prédio e traga a luz da rua " +
                              "para dentro. O DDGI também não descarta, porque detecta sonda " +
                              "enterrada justamente enxergando o verso.\n\n" +
                              "Ligar aproxima do comportamento da Unreal e tende a limpar ruído " +
                              "de acertos espúrios. Onde olhar: reflexo de contato e reflexo em " +
                              "parede."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: cullLabel
                        x: 20
                        y: cullHelper.y + cullHelper.height + 18
                        text: "Descartar verso nos reflexos"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        id: cullToggle
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: cullLabel.y - 6
                        checked: root.renderModel.reflectionsCullBackface
                        onToggled: root.renderModel.ToggleReflectionsCullBackface()
                    }
                }




                Card {
                    id: cameraBookmarksCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Câmeras de referência — protocolo de captura"
                    height: camSlotsColumn.y + camSlotsColumn.height + contentPadding + 8

                    Text {
                        id: camHelper
                        x: 20
                        y: cameraBookmarksCard.headerHeight + cameraBookmarksCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Quatro poses persistentes por cena, gravadas em <cena>.cameras.json " +
                              "ao lado da cena — viajam com ela e entram no repositório.\n\n" +
                              "Existem para o A/B do estimador. O que se compara a partir daqui é " +
                              "ruído, convergência temporal e ocupação do cache, e os três são " +
                              "função da posição EXATA da câmera: duas fotos tiradas à mão não " +
                              "distinguem \"o estimador mudou\" de \"a câmera está meio metro à " +
                              "esquerda\".\n\n" +
                              "Restaurar aqui é NAVEGAÇÃO — teleporta e corta a câmera. Não é " +
                              "captura: uma captura determinística precisa também zerar os caches " +
                              "de mundo (DDGI, radiance cache), que o corte de câmera preserva de " +
                              "propósito, e esperar o aquecimento."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    // Aviso de sidecar não entendido. Sem isto, gravar por cima de um arquivo
                    // corrompido ou de outra versão seria silencioso — e é justamente o caso em
                    // que o usuário precisa saber que existe um .bak antes de confiar no resultado.
                    Text {
                        id: camWarning
                        x: 20
                        y: camHelper.y + camHelper.height + 12
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        visible: root.cameraBookmarks.sidecarWarning !== ""
                        height: visible ? implicitHeight : 0
                        text: "⚠ " + root.cameraBookmarks.sidecarWarning
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    Column {
                        id: camSlotsColumn
                        x: 20
                        y: camWarning.y + camWarning.height + (camWarning.visible ? 14 : 6)
                        width: parent.width - 40
                        spacing: 8

                        Repeater {
                            model: 4
                             delegate: Item {
                                 id: camSlotRow
                                 required property int index
                                 width: camSlotsColumn.width
                                height: 26
                                readonly property bool filled:
                                     root.cameraBookmarks.slotsFilled.length > camSlotRow.index
                                     && root.cameraBookmarks.slotsFilled[camSlotRow.index]

                                Text {
                                    id: camSlotName
                                    anchors.verticalCenter: parent.verticalCenter
                                     text: "Câmera " + (camSlotRow.index + 1)
                                    color: parent.filled ? root.textNormal : root.textSecondary
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 13
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 90
                                    width: parent.width - 90 - 150
                                    elide: Text.ElideRight
                                     // A propriedade notifica quando um slot e regravado; o
                                     // invocavel SlotLabel() nao criaria essa dependencia.
                                     text: root.cameraBookmarks.slotLabels.length > camSlotRow.index
                                           && root.cameraBookmarks.slotLabels[camSlotRow.index] !== ""
                                           ? root.cameraBookmarks.slotLabels[camSlotRow.index] : "vazia"
                                    color: root.textSecondary
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 11
                                }
                                Row {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 6
                                    ActionButton {
                                        label: "Gravar"
                                         onTapped: root.cameraBookmarks.Save(camSlotRow.index)
                                    }
                                    ActionButton {
                                        label: "Ir"
                                         onTapped: root.cameraBookmarks.Restore(camSlotRow.index)
                                    }
                                     // A captura deve partir da pose restaurada no mesmo handler.
                                    ActionButton {
                                        label: "Capturar"
                                        opacity: camSlotRow.filled && !root.capture.busy ? 1.0 : 0.45
                                        onTapped: {
                                            if (root.capture.busy) return
                                             if (!root.cameraBookmarks.Restore(camSlotRow.index)) return
                                             root.capture.Shoot(camSlotRow.index, captureCard.warmupFrames,
                                                          captureCard.scientific,
                                                          captureCard.pinnedHours)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Card {
                    id: captureCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Captura determinística — PNG + manifesto"
                    height: captureColumn.y + captureColumn.height + contentPadding + 8

                    // O N é FIXO para todo o A/B, escolhido uma vez por calibração. Fica aqui, e
                    // não por captura, justamente para não variar entre configurações: parada
                    // adaptativa daria mais tempo de acumulação à configuração que converge mais
                    // devagar, que é precisamente a variável em teste.
                    //
                    // 128 é calibrado (sweep 32/64/128/256 em Docs/CAPTURE-PROTOCOL.md): a
                    // repetibilidade da imagem estabiliza aí, e 256 custa o dobro para ganhar
                    // 0,03 dB. O slider fica para exercitar outro ponto, não para o uso normal.
                    property int  warmupFrames: 128
                    property bool scientific: true
                    // Hora fixada durante a sessão. Não pode ser "a hora atual no clique": com o
                    // Time of Day correndo, duas capturas disparadas com minutos de diferença
                    // sairiam com sóis diferentes, e nenhum reset conserta isso depois. Aqui ela é
                    // um valor DECLARADO, que não muda entre uma captura e outra a menos que o
                    // operador mude — que é o que torna o A/B válido por construção.
                    //
                    // Negativa = ainda não inicializada; o Timer abaixo puxa a hora do mundo na
                    // primeira vez que o card aparece.
                    property real pinnedHours: -1
                    function formatHours(h) {
                        if (h < 0) return "—"
                        var hh = Math.floor(h)
                        var mm = Math.floor((h - hh) * 60)
                        return (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
                    }

                    Text {
                        id: captureHelper
                        x: 20
                        y: captureCard.headerHeight + captureCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Zera TODO acumulador — inclusive os caches de mundo (DDGI, ReGIR, " +
                              "radiance cache, FFT do oceano), que o corte de câmera preserva de " +
                              "propósito — reinicia a semente de amostragem, aquece N frames " +
                              "RENDERIZADOS e grava o frame seguinte em PNG, com um manifesto do " +
                              "estado real da engine ao lado.\n\n" +
                              "Durante a sessão a câmera fica TRAVADA e o mundo para: sol, nuvens, " +
                              "água e vento não avançam. Sem isso, duas capturas do mesmo bookmark " +
                              "sairiam de estados diferentes conforme quantos segundos couberam " +
                              "entre elas — e o A/B mediria o relógio, não o estimador.\n\n" +
                              "O aquecimento é de centenas de frames, não de dezenas: a histerese " +
                              "do DDGI é 0,99 e o radiance cache tem teto de 64 amostras por " +
                              "célula."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    Column {
                        id: captureColumn
                        x: 20
                        y: captureHelper.y + captureHelper.height + 12
                        width: parent.width - 40
                        spacing: 6

                        C.ToggleRow {
                            label: "Preset científico"
                            hint: "nativo, sem upscaler e sem TAA"
                            checked: captureCard.scientific
                            interactive: !root.capture.busy
                            onToggled: captureCard.scientific = !captureCard.scientific
                        }

                        C.SliderRow {
                            label: "Frames de aquecimento (N)"
                            valueText: captureCard.warmupFrames.toFixed(0)
                            from: 0
                            to: 512
                            stepSize: 8
                            boundValue: captureCard.warmupFrames
                            interactive: !root.capture.busy
                            onMoved: (v) => captureCard.warmupFrames = Math.round(v)
                        }

                        Item {
                            width: parent.width
                            height: 26
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Hora do dia fixada"
                                color: root.capture.busy ? root.textMuted : root.textNormal
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: captureCard.formatHours(captureCard.pinnedHours)
                                    color: root.textSecondary
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 11
                                }
                                ActionButton {
                                    label: "Usar hora atual"
                                    opacity: root.capture.busy ? 0.45 : 1.0
                                    onTapped: {
                                        if (root.capture.busy) return
                                        var h = root.capture.CurrentTimeOfDayHours()
                                        if (h >= 0) captureCard.pinnedHours = h
                                    }
                                }
                            }
                        }

                        Item {
                            width: parent.width
                            height: 28

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 130
                                elide: Text.ElideMiddle
                                text: root.capture.busy
                                      ? "aquecendo — faltam " + root.capture.warmupRemaining + " frames"
                                      : (root.capture.lastResult !== "" ? root.capture.lastResult
                                                                   : "nenhuma captura nesta sessão")
                                color: root.capture.busy ? root.blue : root.textSecondary
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 11
                            }
                            ActionButton {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                label: "Capturar aqui"
                                opacity: root.capture.busy ? 0.45 : 1.0
                                // Sem restaurar pose: captura da câmera livre, para inspecionar um
                                // ponto que ainda não virou bookmark. O manifesto grava slot -1.
                                onTapped: {
                                    if (!root.capture.busy)
                                        root.capture.Shoot(-1, captureCard.warmupFrames,
                                                      captureCard.scientific,
                                                      captureCard.pinnedHours)
                                }
                            }
                        }
                    }

                    // A sessão vive na render thread; a GUI pergunta. Só enquanto o card está
                    // visível ou uma captura está em curso — o mesmo critério do card de stats do
                    // radiance cache logo acima.
                    Timer {
                        interval: 200
                        repeat: true
                        running: captureCard.visible || root.capture.busy
                        onTriggered: {
                            root.capture.Poll()
                            // Semeia a hora fixada uma única vez, com o relógio do mundo. Depois
                            // disso ela é do operador: puxá-la do mundo a cada tique faria
                            // exatamente o que a fixação existe para impedir.
                            if (captureCard.pinnedHours < 0) {
                                var h = root.capture.CurrentTimeOfDayHours()
                                if (h >= 0) captureCard.pinnedHours = h
                            }
                        }
                    }
                }


                Card {
                    id: ddgiSampleCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Amostragem do DDGI — bias de auto-sombra"
                    height: biasMaxSlider.y + biasMaxSlider.height + contentPadding + 8

                    Text {
                        id: ddgiSampleHelper
                        x: 20
                        y: ddgiSampleCard.headerHeight + ddgiSampleCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Antes de ler a irradiância, o ponto sombreado é deslocado na normal " +
                              "e na direção da câmera para não se auto-ocluir. O deslocamento " +
                              "escala com o espaçamento do grid, e o grid aqui é dimensionado pela " +
                              "caixa da cena inteira — no Bistro isso dá 8 m entre sondas, ou seja " +
                              "1,20 m de deslocamento: o ponto atravessa a parede e lê a célula do " +
                              "outro lado.\n\n" +
                              "O teto limita esse deslocamento em metros; 0 volta ao " +
                              "comportamento histórico, sem teto. Padrão 0,40 m.\n\n" +
                              "O teste de backface das sondas usa sempre o ponto sem " +
                              "deslocamento, e só a distância do Chebyshev usa o deslocado — é a " +
                              "separação que o Flax faz, e não é ajustável.\n\n" +
                              "O bias também vale no segundo quique, dentro das próprias sondas, " +
                              "então mexer aqui reinicia o atlas e os históricos que se apoiam " +
                              "nele — reflexos, ReSTIR, denoiser e névoa. Espere convergir antes " +
                              "de comparar: logo após a troca a imagem passa pelo ruído de um " +
                              "único quadro de sondas."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    ShadowSlider {
                        id: biasMaxSlider
                        x: 20
                        y: ddgiSampleHelper.y + ddgiSampleHelper.height + 16
                        width: parent.width - 40
                        label: "Teto do bias"
                        from: 0.0
                        to: 2.0
                        step: 0.01
                        // Igual aos epsilons: o rótulo acompanha o arrasto, a engine só é tocada
                        // ao soltar — cada pixel de arrasto reiniciaria o atlas do DDGI e todos
                        // os históricos que se apoiam nele. Sem binding direto em viewportModel
                        // (escrever emite GISettingsChanged e o valor saltaria de volta no meio
                        // do arrasto); recarrega em reloadEps().
                        property real uiValue: 0.0
                        value: uiValue
                        valueText: uiValue === 0.0
                                       ? "sem teto"
                                       : uiValue.toFixed(2).replace(".", ",") + " m"
                        onCommitted: (v) => biasMaxSlider.uiValue = v
                        onReleased: (v) => {
                            biasMaxSlider.uiValue = v
                            root.renderModel.SetGISurfaceBiasMax(v)
                        }
                    }

                }

                Card {
                    id: ddgiAdaptiveCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Resposta do atlas — histerese adaptativa"
                    height: adaptiveHystLabel.y + adaptiveHystLabel.height + contentPadding + 8

                    Text {
                        id: adaptiveHystHelper
                        x: 20
                        y: ddgiAdaptiveCard.headerHeight + ddgiAdaptiveCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Cada sonda guarda 98% do valor anterior por atualização, então uma " +
                              "mudança na cena leva uns 150 quadros para ser absorvida. Quem " +
                              "encurta esse tempo é a invalidação por evento: quando alguém " +
                              "avisa que a região mudou, as sondas de lá passam a misturar " +
                              "rápido.\n\n" +
                              "O problema é que avisar é uma lista, e lista se esquece — mover " +
                              "objeto, esconder no olho do outliner e editar cor ou intensidade " +
                              "de luz não avisavam ninguém, e a luz velha ficava presa no atlas.\n\n" +
                              "Ligado, o próprio passe de atualização compara a estimativa nova " +
                              "com a guardada e derruba a histerese da sonda por conta própria: " +
                              "cerca de três vezes mais luz cai para 0,90, uma ordem de " +
                              "grandeza cai para 0,50. A medida é da sonda inteira, não de um " +
                              "texel, para um firefly isolado não disparar, e fica limitada a " +
                              "0,90 enquanto a sonda está enterrada dentro de geometria.\n\n" +
                              "Vale só para a irradiância. O atlas de distância reamostra menos " +
                              "de um raio por texel a cada quadro, então lá um detector " +
                              "dispararia com o próprio ruído em vez de com mudança de cena.\n\n" +
                              "Desligado por padrão. Na primeira calibração ele acendia com o " +
                              "ruído do próprio estimador — a estimativa de um quadro vem de " +
                              "uns 32 raios e varia sozinha mais do que o limiar de então —, e " +
                              "isso prendia sondas de alta variância em histerese baixa, ou " +
                              "seja, flicker espalhado. Os limiares subiram para fora do ruído, " +
                              "mas essa versão ainda não passou por um A/B próprio.\n\n" +
                              "E o papel dele encolheu: mover, esconder e editar luz agora " +
                              "avisam sozinhos, então o que sobra para a rede pegar são " +
                              "mudanças que ninguém ligou. O que procurar ao testar: sonda perto " +
                              "de emissivo pequeno continua sendo o falso positivo provável. " +
                              "Trocar reinicia o atlas e os históricos que se apoiam nele; " +
                              "espere convergir antes de comparar."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: adaptiveHystLabel
                        x: 20
                        y: adaptiveHystHelper.y + adaptiveHystHelper.height + 18
                        text: "Histerese adaptativa"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: adaptiveHystLabel.y - 6
                        checked: root.renderModel.giAdaptiveHysteresis
                        onToggled: root.renderModel.ToggleGIAdaptiveHysteresis()
                    }
                }

                Card {
                    id: ddgiCascadesCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Cascatas de sondas"
                    height: cascadeRow.y + cascadeRow.height + contentPadding + 8

                    Text {
                        id: cascadeHelper
                        x: 20
                        y: ddgiCascadesCard.headerHeight + ddgiCascadesCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "O volume de sondas cobre a cena inteira com um espaçamento só, e " +
                              "esse espaçamento sai do tamanho da cena: no Bistro dá 8 metros. " +
                              "O paper pede 1 a 2 metros para escala humana, e é dessa diferença " +
                              "que vêm o vazamento de luz através de parede fina, a separação " +
                              "ruim entre cômodos e o bias de amostragem que precisou de um " +
                              "teto em metros para não atravessar geometria.\n\n" +
                              "Com duas cascatas, a grossa continua sendo exatamente esse volume " +
                              "— cobrindo tudo, convergindo em todo lugar — e uma segunda, quatro " +
                              "vezes mais fina, entra por dentro dela. Cada ponto é servido pela " +
                              "mais fina que o contém, e as duas se misturam nas últimas células " +
                              "da borda para não haver costura. O que sai da fina cai na grossa; " +
                              "só o que sai da grossa cai no ambiente do céu.\n\n" +
                              "A fina SEGUE A CÂMERA, e a grade dela rola por baixo em células " +
                              "inteiras: o que já foi calculado continua valendo para o mesmo " +
                              "ponto do mundo, e só a fatia que entrou pela borda é recalculada. " +
                              "Sem isso, cada dois metros andados jogariam fora tudo o que ela " +
                              "tinha guardado.\n\n" +
                              "Custo, medido no Bistro (3060 Ti, 1573×804): cada cascata é um " +
                              "grid inteiro de sondas, então duas dobram os raios por quadro — " +
                              "8.832 sondas contra 4.416. O traçado vai de 2,64 para 5,14 ms, " +
                              "linear na contagem, e o QUADRO de 7,65 para 8,48 ms: 131 para " +
                              "118 FPS, cerca de 11%. Amostrar as duas cascatas, essa parte é de " +
                              "graça — iluminação, reflexos e névoa não se mexeram.\n\n" +
                              "O custo não aparece na espera pelo traçado (0,02 → 0,10 ms), e sim " +
                              "nos passes que dividem a GPU com ele enquanto roda em paralelo: o " +
                              "Z-prepass e o G-buffer ficam mais caros sem ler nada do GI. Quem " +
                              "decide o orçamento é o quadro, não a linha de espera.\n\n" +
                              "Para remedir: espere a relocação convergir (nos primeiros ~180 " +
                              "updates as sondas ainda estão se acomodando e todas traçam com 64 " +
                              "raios) e mantenha a mesma câmera nos dois lados.\n\n" +
                              "Trocar aqui RECRIA o volume: os atlas, o buffer de raios e as " +
                              "sondas são realocados, e a iluminação indireta reconverge do zero."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Row {
                        id: cascadeRow
                        x: 20
                        y: cascadeHelper.y + cascadeHelper.height + 18
                        spacing: 8
                        Text {
                            text: "Cascatas"
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Repeater {
                             model: [1, 2, 3]
                             delegate: Rectangle {
                                 id: cascadeOption
                                 required property int modelData
                                 width: 44; height: 26; radius: 4
                                 color: root.renderModel.giCascadeCount === cascadeOption.modelData
                                       ? C.Theme.accent : "transparent"
                                border.color: root.textSecondary
                                border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                     text: cascadeOption.modelData
                                     color: root.renderModel.giCascadeCount === cascadeOption.modelData
                                           ? "#ffffff" : root.textNormal
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 13
                                }
                                MouseArea {
                                    anchors.fill: parent
                                     onClicked: root.renderModel.SetGICascadeCount(cascadeOption.modelData)
                                }
                            }
                        }
                    }
                }

                Card {
                    id: ddgiAdaptiveRaysCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Orçamento de raios por sonda"
                    height: adaptiveRaysLabel.y + adaptiveRaysLabel.height + contentPadding + 8

                    Text {
                        id: adaptiveRaysHelper
                        x: 20
                        y: ddgiAdaptiveRaysCard.headerHeight + ddgiAdaptiveRaysCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Hoje toda sonda traça 64 raios por quadro, esteja ela encostada " +
                              "numa parede ou boiando no meio da rua. Como o custo do DDGI é " +
                              "raios vezes número de sondas, é esse produto que decide quantas " +
                              "sondas cabem no quadro — e é a resposta a essa pergunta que " +
                              "libera, ou não, uma grade mais fina.\n\n" +
                              "Ligado, cada sonda recebe uma contagem proporcional à distância " +
                              "até a geometria mais próxima: quem está a menos de meio espaçamento " +
                              "de uma superfície fica no teto, quem está a mais de oito cai para " +
                              "o piso. A ideia é que a sonda em espaço aberto vê quase só céu e " +
                              "geometria distante, então poucos raios já descrevem bem o que ela " +
                              "recebe — enquanto a encostada numa quina precisa de todos.\n\n" +
                              "A classificação sai do mesmo passe que reloca as sondas, e é por " +
                              "isso que este botão não fazia nada antes: com a relocação " +
                              "desligada o passe saía cedo e a contagem congelava. Agora ele " +
                              "classifica dos dois jeitos.\n\n" +
                              "Nos quadros em que a classificação roda, o traçado volta aos 64 " +
                              "raios de propósito: ela mede a proximidade pelo acerto mais " +
                              "próximo, e medir isso com a sonda já decimada faria a estimativa " +
                              "vir viesada para longe — a sonda cairia mais um degrau, e outro. " +
                              "Como consequência, a economia só aparece depois que a relocação " +
                              "converge, cerca de 180 quadros após carregar a cena.\n\n" +
                              "O que procurar: a sonda decimada tem menos amostras por quadro, " +
                              "logo mais variância — se aparecer cintilação, ela vem do céu " +
                              "aberto e não dos cantos. Trocar reinicia o atlas e os históricos " +
                              "que se apoiam nele; espere convergir antes de comparar, e olhe o " +
                              "bloco do DDGI no profiler, porque o ganho de tempo é metade da " +
                              "resposta."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: adaptiveRaysLabel
                        x: 20
                        y: adaptiveRaysHelper.y + adaptiveRaysHelper.height + 18
                        text: "Raios adaptativos"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: adaptiveRaysLabel.y - 6
                        checked: root.renderModel.giAdaptiveRays
                        onToggled: root.renderModel.ToggleGIAdaptiveRays()
                    }
                }


                Card {
                    id: volumeFadeCard
                    visible: giPage.labVisible
                    width: parent.width
                    title: "Fora do volume de sondas"
                    height: fadeSlider.y + fadeSlider.height + contentPadding + 8

                    Text {
                        id: fadeHelper
                        x: 20
                        y: volumeFadeCard.headerHeight + volumeFadeCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "O volume cobre a caixa da cena, e o terreno fica de fora de " +
                              "propósito — um terreno de quilômetros esticaria a grade inteira. " +
                              "Só que o gather não falha fora do volume: ele grampeia as " +
                              "coordenadas e estende a última fileira de sondas ao infinito, " +
                              "enquanto o ambiente difuso comum fica desligado porque o GI está " +
                              "ligado. Na prática o terreno inteiro herda a irradiância da borda " +
                              "da grade.\n\n" +
                              "Este controle desvanece para o ambiente hemisférico da atmosfera " +
                              "— o mesmo que a cena usaria sem GI — ao longo da largura " +
                              "escolhida, medida em células da grade e contada para FORA da " +
                              "borda. Dentro do volume nada muda: o volume é justo, então o chão " +
                              "nasce a meia célula da face inferior e um desvanecimento para " +
                              "dentro lavaria o piso inteiro. 0 volta ao comportamento " +
                              "histórico.\n\n" +
                              "Vale também nos raios das sondas, onde o indireto desvanece para " +
                              "o escuro em vez do ambiente — fora do volume a luz da borda seria " +
                              "reinjetada no próprio atlas. Por isso mexer aqui reinicia o atlas " +
                              "e os históricos; espere convergir antes de comparar."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    ShadowSlider {
                        id: fadeSlider
                        x: 20
                        y: fadeHelper.y + fadeHelper.height + 16
                        width: parent.width - 40
                        label: "Largura do fade"
                        from: 0.0
                        to: 3.0
                        step: 0.25
                        property real uiValue: 0.0
                        value: uiValue
                        valueText: uiValue === 0.0
                                       ? "desligado"
                                       : uiValue.toFixed(2).replace(".", ",") + " células"
                        onCommitted: (v) => fadeSlider.uiValue = v
                        onReleased: (v) => {
                            fadeSlider.uiValue = v
                            root.renderModel.SetGIVolumeFadeProbes(v)
                        }
                    }
                }
            }
                }
            }
        }

        Item {
            visible: root.selectedPage !== 0 && root.selectedPage !== 1
                     && root.selectedPage !== 5 && root.selectedPage !== 6
            anchors.fill: parent
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(430, parent.width - 80)
                height: 150
                radius: 10
                color: root.cardBg
                border.color: root.borderColor
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 30
                    text: root.pageTitle()
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 18
                }
                Text {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 18
                    width: parent.width - 60
                    text: "A estrutura desta página já está reservada. Os controles serão adicionados durante a refatoração."
                    color: root.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
            }
        }
    }

    // Rodape.
    Rectangle {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 48
        color: root.bg

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: root.divider
        }
        Text {
            x: 264
            anchors.verticalCenter: parent.verticalCenter
            text: "As alterações são aplicadas em tempo real no viewport"
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Rectangle {
            id: resetButton
            anchors.right: applyButton.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 140; height: 28; radius: 6
            color: resetHover.hovered ? C.Theme.surfaceRaised : "transparent"
            border.color: C.Theme.hairlineStrong
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: "Restaurar padrões"
                color: root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
            }
            HoverHandler { id: resetHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.renderModel.ResetRenderSettings() }
        }
        Rectangle {
            id: applyButton
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 78; height: 28; radius: 6
            color: applyHover.hovered ? "#70adff" : root.blue
            Text {
                anchors.centerIn: parent
                text: "Aplicar"
                color: C.Theme.shell
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            HoverHandler { id: applyHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.settingsWindow.close() }
        }
    }
}
