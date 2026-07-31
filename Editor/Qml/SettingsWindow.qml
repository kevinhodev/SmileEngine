import QtQuick
import QtQuick.Controls
import "components" as C

// Janela de configuracoes do editor. A pagina de Renderizacao esta funcional; as demais
// categorias preservam a estrutura da referencia e recebem conteudo nas proximas iteracoes.
Rectangle {
    id: root
    color: "#141511"
    border.color: "#2e2f28"
    border.width: 1
    implicitWidth: 960
    implicitHeight: 640

    property int selectedPage: 0
    property string searchText: ""

    readonly property color bg: C.Theme.bg
    readonly property color sidebarBg: "#10110f"
    readonly property color cardBg: C.Theme.cardBg
    readonly property color hoverBg: "#22231c"
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color blueBg: C.Theme.blueBg
    readonly property color blueBorder: C.Theme.blueBorder
    readonly property color green: C.Theme.green
    // Cores de acento dos vendors, usadas so nas marcas tipograficas dos upscalers.
    readonly property color amdRed: "#e8483c"
    readonly property color nvidiaGreen: "#8cc63f"

    function pageTitle() {
        const titles = [
            "Renderização", "Iluminação global", "Path tracer", "Reflexos e denoise",
            "Água — FFT", "Pós-processo", "Sombras e céu", "Nuvens volumétricas",
            "Clima", "Interface", "Atalhos", "Projeto"
        ]
        return titles[selectedPage]
    }

    function pageSubtitle() {
        if (selectedPage === 0)
            return "Upscaling, Anti-Aliasing e Resolução Interna do Viewport"
        if (selectedPage === 1)
            return "Geometria dos raios de GI, reflexo e sombra: origem, intervalo e frescor da amostra"
        if (selectedPage === 6)
            return "Sombras do sol (CSM), sun shafts e volumetric fog: cascatas, cache, bias e debug"
        if (selectedPage === 7)
            return "Raymarch de nuvens na atmosfera: cobertura, forma, iluminação e custo"
        if (selectedPage === 8)
            return "Chuva: wetness deferred, poças, cortina de gotas e acoplamento com o céu"
        return "Esta categoria será conectada aos controles do engine em uma próxima etapa"
    }

    function matchesSearch(label) {
        return searchText.length === 0 ||
               label.toLocaleLowerCase().indexOf(searchText.toLocaleLowerCase()) >= 0
    }

    // ---- Componentes compartilhados (Editor/Qml/components) ----
    component WindowButton: C.WindowButton {}
    component Toggle: C.Toggle {}

    component ShadowSlider: Item {
        id: srow
        property string label
        property real from: 0
        property real to: 1
        property real step: 0.01
        property real value: 0
        property string valueText: ""
        signal committed(real v)
        // Emitido so ao SOLTAR o mouse. Quem escreve em estado caro (que invalida historicos ou
        // reconstroi o model da propria lista) deve usar este, nao o committed continuo.
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
            color: "#23241d"
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
            onMoved: srow.committed(value)
            onPressedChanged: if (!pressed) srow.released(value)
            background: Rectangle {
                x: sctl.leftPadding
                y: sctl.topPadding + sctl.availableHeight / 2 - height / 2
                width: sctl.availableWidth
                height: 4
                radius: 2
                color: "#23241d"
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
        radius: 6
        visible: root.matchesSearch(label)
        color: root.selectedPage === page ? root.blueBg : (navHover.hovered ? "#1a1b15" : "transparent")
        border.color: root.selectedPage === page ? root.blueBorder : "transparent"
        border.width: 1

        Rectangle {
            visible: root.selectedPage === nav.page
            x: 0; y: 4
            width: 3; height: 22; radius: 1.5
            color: root.blue
        }
        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: nav.glyph
            color: root.selectedPage === nav.page ? root.blue : "#8f8a7d"
            font.family: "Segoe UI Symbol"
            font.pixelSize: 14
        }
        Text {
            id: navLabel
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            text: nav.label
            color: root.selectedPage === nav.page ? root.textPrimary : "#b9b5aa"
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
        color: abtnHover.hovered ? "#23241d" : "transparent"
        border.color: "#33342c"
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
        radius: 6
        color: option.selected ? root.blueBg
                               : (optionHover.hovered && option.available ? root.hoverBg : "transparent")
        border.color: option.selected ? root.blueBorder : "transparent"
        border.width: 1
        opacity: option.available ? 1.0 : 0.46

        Rectangle {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30; height: 30; radius: 7
            color: option.selected ? "#243651" : "#292b24"
            border.color: option.selected ? "#345681" : "#36382f"
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
                color: option.selected ? root.blue : root.textNormal
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
            color: root.blue
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
        radius: 6
        color: dopt.selected ? root.blueBg
                             : (doptHover.hovered && dopt.available ? root.hoverBg : "transparent")
        border.color: dopt.selected ? root.blueBorder : "transparent"
        border.width: 1
        opacity: dopt.available ? 1.0 : 0.46

        Rectangle {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 30; height: 30; radius: 7
            color: dopt.selected ? "#243651" : "#292b24"
            border.color: dopt.selected ? "#345681" : "#36382f"
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
            color: root.blue
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
        radius: 8
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

    // Barra de titulo.
    Rectangle {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40
        color: root.bg

        Image {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            source: "image://smilelogo/32"
            sourceSize: Qt.size(32, 32)
            width: 20; height: 20
            fillMode: Image.PreserveAspectFit
            smooth: true
        }
        Text {
            x: 42
            anchors.verticalCenter: parent.verticalCenter
            text: "Configurações — SmileEngine"
            color: root.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
        }

        MouseArea {
            anchors.left: parent.left
            anchors.right: windowButtons.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            property bool moving: false
            onPressed: moving = false
            onPositionChanged: {
                if (pressed && !moving) {
                    moving = true
                    settingsWindow.startSystemMove()
                }
            }
            onReleased: moving = false
            onDoubleClicked: settingsWindow.toggleMaximize()
        }

        Row {
            id: windowButtons
            anchors.right: parent.right
            anchors.top: parent.top
            WindowButton { glyph: "−"; onTapped: settingsWindow.minimize() }
            WindowButton {
                glyph: settingsWindow.maximized ? "❐" : "□"
                onTapped: settingsWindow.toggleMaximize()
            }
            WindowButton { glyph: "×"; danger: true; onTapped: settingsWindow.close() }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.divider
        }
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
            color: "#1a1b15"
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
                selectedTextColor: "#10110f"
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
            NavItem { page: 4; glyph: "≋"; label: "Água — FFT" }
            NavItem { page: 5; glyph: "☷"; label: "Pós-processo" }
            NavItem { page: 6; glyph: "☾"; label: "Sombras e céu" }
            NavItem { page: 7; glyph: "☁"; label: "Nuvens volumétricas" }
            NavItem { page: 8; glyph: "☂"; label: "Clima" }
        }

        Text {
            x: 20; y: 372
            text: "Editor"
            color: root.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }
        Column {
            x: 12; y: 386
            spacing: 4
            NavItem { page: 9; glyph: "▤"; label: "Interface" }
            NavItem { page: 10; glyph: "⌨"; label: "Atalhos" }
            NavItem { page: 11; glyph: "▰"; label: "Projeto" }
        }

        Rectangle {
            x: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            width: 216; height: 52; radius: 8
            color: "#1a1b15"
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
            color: "#1a1b15"
            border.color: root.borderColor
            border.width: 1
            Text {
                id: gpuLabel
                anchors.centerIn: parent
                width: parent.width - 20
                text: viewportModel.gpuName + " · DX12"
                color: root.textSecondary
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
            }
        }

        // Duas colunas empilhadas por Column (mesmo padrao da pagina de Sombras/Nuvens, adaptado
        // p/ 2 colunas): nenhum card sabe seu proprio y, entao mudar/ocultar conteudo reflui tudo
        // sozinho. O contentHeight segue a coluna mais alta, entao o scroll sempre alcanca o fim.
        Flickable {
            id: renderingPage
            visible: root.selectedPage === 0
            anchors.fill: parent
            anchors.topMargin: 84
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
                    color: "#23241d"
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
                            visible: viewportModel.upscalerMode === 1
                            text: "FSR"
                            color: root.amdRed
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: viewportModel.upscalerMode === 2
                            text: "DLSS"
                            color: root.nvidiaGreen
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: viewportModel.upscalerMode === 0
                            text: "1:1"
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: viewportModel.upscalerMode === 2
                              ? "NVIDIA DLSS"
                              : viewportModel.upscalerMode === 1
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
                        text: viewportModel.upscalerMode === 2
                              ? "DLSS · Reconstrução por IA (Super Resolution)"
                              : viewportModel.upscalerMode === 1
                                ? "FSR 3.1 · Reconstrução Temporal"
                                : "TAA · Escala Manual de Renderização"
                        color: root.textMuted
                        elide: Text.ElideRight
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Rectangle {
                        visible: viewportModel.upscalerMode === viewportModel.recommendedUpscalerMode
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
                    text: viewportModel.fsrAvailable
                          ? "O recomendado é escolhido conforme o hardware e os backends disponíveis."
                          : "FSR 3.1 indisponível; usando o caminho nativo com TAA."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }

                Item {
                    id: fsrControls
                    visible: viewportModel.upscalerMode === 1 || viewportModel.upscalerMode === 2
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
                        text: (viewportModel.upscalerMode === 2 ? "DLSS" : "FSR") +
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
                        color: "#23241d"
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
                                    required property var modelData
                                    required property int index
                                    readonly property bool isCurrent: viewportModel.upscalerQuality === index
                                    width: qualitySelector.width / 5
                                    height: qualitySelector.height
                                    radius: 6
                                    color: isCurrent ? root.blueBg
                                                     : (qualityHover.hovered ? "#2a2b24" : "transparent")
                                    border.color: isCurrent ? root.blueBorder : "transparent"
                                    border.width: 1
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 1
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: modelData.name
                                            color: isCurrent ? root.blue : root.textSecondary
                                            font.family: C.Theme.fontFamily
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: modelData.scale
                                            color: isCurrent ? root.blue : root.textMuted
                                            opacity: isCurrent ? 0.85 : 1.0
                                            font.family: C.Theme.fontFamily
                                            font.pixelSize: 9
                                        }
                                    }
                                    HoverHandler {
                                        id: qualityHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: viewportModel.SetUpscalerQuality(index)
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
                            text: viewportModel.internalResolution + "  →  " + viewportModel.outputResolution
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
                                text: viewportModel.renderScale < 0.999
                                      ? "−" + Math.round((1.0 - viewportModel.renderScale *
                                                         viewportModel.renderScale) * 100) + "% Pixels"
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
                    visible: viewportModel.upscalerMode === 0
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
                        checked: viewportModel.taaEnabled
                        onToggled: viewportModel.SetTAAEnabled(!checked)
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
                        color: "#23241d"
                        border.color: root.borderColor
                        Text {
                            anchors.centerIn: parent
                            text: viewportModel.renderScale.toFixed(2).replace(".", ",") + "×"
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
                        value: viewportModel.renderScale
                        onPressedChanged: {
                            if (!pressed) viewportModel.SetRenderScale(value)
                        }
                        background: Rectangle {
                            x: renderScaleSlider.leftPadding
                            y: renderScaleSlider.topPadding + renderScaleSlider.availableHeight / 2 - height / 2
                            width: renderScaleSlider.availableWidth
                            height: 4; radius: 2
                            color: "#23241d"
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
                    visible: viewportModel.upscalerMode !== viewportModel.recommendedUpscalerMode
                    Text {
                        text: "Recomendado para esta GPU:"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Text {
                        id: useRecommendedLink
                        text: "Usar " + viewportModel.recommendedUpscalerName
                        color: root.blue
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                        font.underline: useRecommendedHover.hovered
                        HoverHandler {
                            id: useRecommendedHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: viewportModel.SetUpscalerMode(viewportModel.recommendedUpscalerMode)
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
                            selected: viewportModel.upscalerMode === 0
                            badge: viewportModel.recommendedUpscalerMode === 0 ? "RECOMENDADO" : ""
                            onChosen: {
                                viewportModel.SetUpscalerMode(0)
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
                            selected: viewportModel.upscalerMode === 1
                            available: viewportModel.fsrAvailable
                            badge: viewportModel.recommendedUpscalerMode === 1 ? "RECOMENDADO" : ""
                            onChosen: {
                                viewportModel.SetUpscalerMode(1)
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
                            selected: viewportModel.upscalerMode === 2
                            available: viewportModel.dlssAvailable
                            badge: viewportModel.recommendedUpscalerMode === 2 ? "RECOMENDADO" : ""
                            onChosen: {
                                viewportModel.SetUpscalerMode(2)
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
                    color: "#23241d"
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
                            text: viewportModel.denoiserMode === 2 ? "RR"
                                  : (viewportModel.denoiserMode === 1 ? "NRD" : "OFF")
                            color: viewportModel.denoiserMode === 2 ? root.nvidiaGreen
                                   : (viewportModel.denoiserMode === 1 ? root.textNormal : root.textMuted)
                            font.family: C.Theme.fontFamily
                            font.pixelSize: viewportModel.denoiserMode === 1 ? 8 : 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: viewportModel.denoiserMode === 2 ? "DLSS Ray Reconstruction"
                              : (viewportModel.denoiserMode === 1 ? "NRD RELAX" : "Nenhum")
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    Text {
                        x: 49; y: 24
                        width: parent.width - 80
                        text: viewportModel.denoiserMode === 2
                              ? "Denoiser neural (NVIDIA) · substitui NRD + upscaler"
                              : (viewportModel.denoiserMode === 1
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
                    text: viewportModel.rrAvailable
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
                            selected: viewportModel.denoiserMode === 0
                            onChosen: { viewportModel.SetDenoiserMode(0); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 1
                            label: "NRD RELAX"
                            detail: "A-trous · difuso (GI) + especular (reflexão)"
                            selected: viewportModel.denoiserMode === 1
                            onChosen: { viewportModel.SetDenoiserMode(1); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 2
                            label: "DLSS Ray Reconstruction"
                            detail: available ? "IA · substitui NRD + upscaler (trava DLSS)"
                                              : "Indisponível (requer GPU NVIDIA RTX)"
                            selected: viewportModel.denoiserMode === 2
                            available: viewportModel.rrAvailable
                            onChosen: { viewportModel.SetDenoiserMode(2); denoiserPopup.close() }
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
                    checked: viewportModel.depthPrepassEnabled
                    onToggled: viewportModel.SetDepthPrepassEnabled(!checked)
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
                    checked: viewportModel.frustumCullingEnabled
                    onToggled: viewportModel.SetFrustumCullingEnabled(!checked)
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
                    checked: viewportModel.occlusionCullingEnabled
                    onToggled: viewportModel.SetOcclusionCullingEnabled(!checked)
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
                    text: Math.round(viewportModel.fps)
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
                    text: viewportModel.frameTimeMs.toFixed(1).replace(".", ",") + " ms por frame"
                    color: root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Rectangle { x: 0; y: 112; width: parent.width; height: 1; color: root.divider }
                // So o que reage a ESTA pagina: trocar o preset muda a resolucao interna e o
                // fps/ms na hora. Saiu a VRAM (era o total ESTATICO do adapter, alem de estar
                // sob um cabecalho de Ray tracing — a StatsWindow ja mostra a do processo) e
                // saiu a contagem de draws, que e telemetria geral e nao reage a esta pagina.
                // NOTA: draws hoje nao aparecem em lugar nenhum; o bridge ainda expoe
                // visibleDrawCount/occludedDrawCount/totalDrawCount p/ quem for reexibir
                // (a StatsWindow e o lugar natural — ela so tem VRAM e custo por passe).
                StatusRow {
                    x: 16; y: 121; width: parent.width - 32
                    label: "Res. interna"
                    value: viewportModel.internalResolution
                }
            }

            } // renderingRightCol
        }

        Flickable {
            id: shadowsPage
            visible: root.selectedPage === 6
            anchors.fill: parent
            anchors.topMargin: 84
            contentWidth: width
            contentHeight: shadowsCol.height + 40
            clip: true
            ScrollBar.vertical: ThinScrollBar { revealed: shadowsPageHover.hovered }
            HoverHandler { id: shadowsPageHover }

            // coluna única (padrão da página de nuvens): contentHeight segue a Column
            // e o scroll sempre alcança o último card
            Column {
                id: shadowsCol
                x: 24
                width: shadowsPage.width - 48
                spacing: 16

            Card {
                width: parent.width
                height: 312
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
                    checked: viewportModel.sunShadowsEnabled
                    onToggled: viewportModel.SetSunShadowsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Distância máxima"
                    from: 100; to: 3000; step: 50
                    value: viewportModel.shadowMaxDistance
                    valueText: Math.round(viewportModel.shadowMaxDistance) + " m"
                    onCommitted: (v) => viewportModel.SetShadowMaxDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Bias de profundidade"
                    from: 0; to: 0.003; step: 0.0001
                    value: viewportModel.shadowDepthBias
                    valueText: (viewportModel.shadowDepthBias * 10000).toFixed(1).replace(".", ",") + "e-4"
                    onCommitted: (v) => viewportModel.SetShadowDepthBias(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Tamanho angular do sol (PCSS)"
                    from: 0; to: 2.0; step: 0.01
                    value: viewportModel.shadowSunAngle
                    valueText: viewportModel.shadowSunAngle < 0.005
                               ? "off"
                               : viewportModel.shadowSunAngle.toFixed(2).replace(".", ",") + "°"
                    onCommitted: (v) => viewportModel.SetShadowSunAngle(v)
                }

                Text {
                    x: 20; y: 272
                    text: "Debug de cascatas"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 272
                    text: "tinge a cena pela cascata usada"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 266
                    checked: viewportModel.shadowDebugCascades
                    onToggled: viewportModel.SetShadowDebugCascades(!checked)
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
                    checked: viewportModel.sunShaftsEnabled
                    onToggled: viewportModel.SetSunShaftsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Intensidade"
                    from: 0; to: 5.0; step: 0.1
                    value: viewportModel.sunShaftsIntensity
                    valueText: viewportModel.sunShaftsIntensity.toFixed(1).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetSunShaftsIntensity(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Poeira — densidade do feixe"
                    from: 1; to: 64; step: 1
                    value: viewportModel.sunShaftsDust
                    valueText: "×" + Math.round(viewportModel.sunShaftsDust)
                    onCommitted: (v) => viewportModel.SetSunShaftsDust(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.95; step: 0.01
                    value: viewportModel.sunShaftsPhaseG
                    valueText: viewportModel.sunShaftsPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetSunShaftsPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Passos do raymarch"
                    from: 8; to: 64; step: 4
                    value: viewportModel.sunShaftsSteps
                    valueText: viewportModel.sunShaftsSteps + ""
                    onCommitted: (v) => viewportModel.SetSunShaftsSteps(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Alcance da marcha (m)"
                    from: 32; to: 400; step: 8
                    value: viewportModel.sunShaftsRange
                    valueText: Math.round(viewportModel.sunShaftsRange) + " m"
                    onCommitted: (v) => viewportModel.SetSunShaftsRange(v)
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
                    checked: viewportModel.sunShaftsTemporal
                    onToggled: viewportModel.SetSunShaftsTemporal(!checked)
                }
            }

            Card {
                width: parent.width
                height: 552
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
                    checked: viewportModel.volFogEnabled
                    onToggled: viewportModel.SetVolFogEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Alcance do volume (m)"
                    from: 30; to: 300; step: 10
                    value: viewportModel.volFogDistance
                    valueText: Math.round(viewportModel.volFogDistance) + " m"
                    onCommitted: (v) => viewportModel.SetVolFogDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Densidade do volume"
                    from: 0.25; to: 8.0; step: 0.25
                    value: viewportModel.volFogDensity
                    valueText: "×" + viewportModel.volFogDensity.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetVolFogDensity(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.9; step: 0.05
                    value: viewportModel.volFogPhaseG
                    valueText: viewportModel.volFogPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetVolFogPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Ambiente (DDGI/céu)"
                    from: 0; to: 4.0; step: 0.1
                    value: viewportModel.volFogAmbient
                    valueText: "×" + viewportModel.volFogAmbient.toFixed(1).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetVolFogAmbient(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Luzes no ar (halo de point/spot)"
                    from: 0; to: 4.0; step: 0.1
                    value: viewportModel.volFogLights
                    valueText: "×" + viewportModel.volFogLights.toFixed(1).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetVolFogLights(v)
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
                    checked: viewportModel.volFogTemporal
                    onToggled: viewportModel.SetVolFogTemporal(!checked)
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
                    checked: viewportModel.volFogConsDepth
                    onToggled: viewportModel.SetVolFogConsDepth(!checked)
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
                    checked: viewportModel.shadowCacheEnabled
                    onToggled: viewportModel.SetShadowCacheEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Caster mínimo (texels da cascata)"
                    from: 0; to: 8; step: 0.5
                    value: viewportModel.shadowMinCasterTexels
                    valueText: viewportModel.shadowMinCasterTexels.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => viewportModel.SetShadowMinCasterTexels(v)
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
                            value: Number(viewportModel.shadowCascadeBias[index])
                            valueText: "×" + Number(viewportModel.shadowCascadeBias[index]).toFixed(2).replace(".", ",")
                            onCommitted: (v) => viewportModel.SetShadowCascadeBiasScale(index, v)
                        }
                    }
                }
            }
            }
        }

        Flickable {
            id: cloudsPage
            visible: root.selectedPage === 7
            anchors.fill: parent
            anchors.topMargin: 84
            contentWidth: width
            contentHeight: cloudsCol.height + 40
            clip: true
            ScrollBar.vertical: ThinScrollBar { revealed: cloudsPageHover.hovered }
            HoverHandler { id: cloudsPageHover }

            Column {
                id: cloudsCol
                x: 24
                width: cloudsPage.width - 48
                spacing: 16

            Card {
                width: parent.width
                height: 330
                title: "Camada de nuvens"

                Text {
                    x: 20; y: 55
                    text: "Nuvens volumétricas"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raymarch acoplado à atmosfera"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: viewportModel.cloudsEnabled
                    onToggled: viewportModel.SetCloudsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Cobertura"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.cloudCoverage
                    valueText: Math.round(viewportModel.cloudCoverage * 100) + " %"
                    onCommitted: (v) => viewportModel.SetCloudCoverage(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Densidade (extinção /km)"
                    from: 0.1; to: 10.0; step: 0.1
                    value: viewportModel.cloudDensity
                    valueText: viewportModel.cloudDensity.toFixed(1).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetCloudDensity(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Altitude da base"
                    from: 0.5; to: 8.0; step: 0.1
                    value: viewportModel.cloudBottomKm
                    valueText: viewportModel.cloudBottomKm.toFixed(1).replace(".", ",") + " km"
                    onCommitted: (v) => viewportModel.SetCloudAltitude(v, viewportModel.cloudThicknessKm)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Espessura da camada"
                    from: 0.5; to: 8.0; step: 0.1
                    value: viewportModel.cloudThicknessKm
                    valueText: viewportModel.cloudThicknessKm.toFixed(1).replace(".", ",") + " km"
                    onCommitted: (v) => viewportModel.SetCloudAltitude(viewportModel.cloudBottomKm, v)
                }
            }

            Card {
                width: parent.width
                height: 336
                title: "Distribuição — weather map"

                ShadowSlider {
                    x: 20; y: 56
                    width: parent.width - 40
                    label: "Células do padrão (re-bakea na hora)"
                    from: 1; to: 8; step: 1
                    value: viewportModel.cloudWeatherCells
                    valueText: viewportModel.cloudWeatherCells + "×"
                    onCommitted: (v) => viewportModel.SetCloudWeatherCells(v)
                }
                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Semente"
                    from: 0; to: 9999; step: 1
                    value: viewportModel.cloudWeatherSeed
                    valueText: viewportModel.cloudWeatherSeed + ""
                    onCommitted: (v) => viewportModel.SetCloudWeatherSeed(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Viés de tipo (stratus ↔ cumulonimbus)"
                    from: -0.5; to: 0.5; step: 0.01
                    value: viewportModel.cloudTypeBias
                    valueText: (viewportModel.cloudTypeBias >= 0 ? "+" : "") +
                               viewportModel.cloudTypeBias.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetCloudTypeBias(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Variação de topo (peak height, canal B)"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.cloudPeakVariation
                    valueText: Math.round(viewportModel.cloudPeakVariation * 100) + " %"
                    onCommitted: (v) => viewportModel.SetCloudPeakVariation(v)
                }

                Row {
                    x: 20; y: 268
                    spacing: 10
                    ActionButton {
                        label: "Semente aleatória"
                        onTapped: viewportModel.SetCloudWeatherSeed(Math.floor(Math.random() * 10000))
                    }
                    ActionButton {
                        label: "Carregar textura…"
                        onTapped: viewportModel.LoadCloudWeatherTexture()
                    }
                    ActionButton {
                        label: "Voltar ao procedural"
                        visible: viewportModel.cloudWeatherAuthored
                        onTapped: viewportModel.ClearCloudWeatherTexture()
                    }
                }
                Text {
                    x: 20; y: 306
                    text: viewportModel.cloudWeatherAuthored
                          ? "fonte: textura autorada — R = cobertura · G = tipo · B = altura de topo"
                          : "fonte: procedural (seed " + viewportModel.cloudWeatherSeed + ")"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }
            }

            Card {
                width: parent.width
                height: 418
                title: "Forma e iluminação"

                Text {
                    x: 20; y: 316
                    text: "Sombra das nuvens no chão"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 335
                    text: "shadow map 512² projetado da camada"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 315
                    checked: viewportModel.cloudShadows
                    onToggled: viewportModel.SetCloudShadowsEnabled(!checked)
                }
                ShadowSlider {
                    x: 20; y: 362
                    width: parent.width - 40
                    label: "Intensidade da sombra"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.cloudShadowStrength
                    valueText: Math.round(viewportModel.cloudShadowStrength * 100) + " %"
                    onCommitted: (v) => viewportModel.SetCloudShadowStrength(v)
                }

                ShadowSlider {
                    x: 20; y: 56
                    width: parent.width - 40
                    label: "Erosão de detalhe"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.cloudErosion
                    valueText: Math.round(viewportModel.cloudErosion * 100) + " %"
                    onCommitted: (v) => viewportModel.SetCloudErosion(v)
                }
                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Vento"
                    from: 0; to: 0.05; step: 0.001
                    value: viewportModel.cloudWindSpeed
                    valueText: Math.round(viewportModel.cloudWindSpeed * 1000) + " m/s"
                    onCommitted: (v) => viewportModel.SetCloudWindSpeed(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Anisotropia da fase (g)"
                    from: 0; to: 0.95; step: 0.01
                    value: viewportModel.cloudPhaseG
                    valueText: viewportModel.cloudPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetCloudPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Powder (auto-sombra de borda)"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.cloudPowder
                    valueText: Math.round(viewportModel.cloudPowder * 100) + " %"
                    onCommitted: (v) => viewportModel.SetCloudPowder(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Ambiente do céu"
                    from: 0; to: 3.0; step: 0.05
                    value: viewportModel.cloudAmbient
                    valueText: "×" + viewportModel.cloudAmbient.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetCloudAmbient(v)
                }
            }

            Card {
                width: parent.width
                height: 216
                title: "Desempenho"

                Text {
                    x: 20; y: 55
                    text: "Meia resolução"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raymarch em ½ res + upsample bilinear"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: viewportModel.cloudsHalfRes
                    onToggled: viewportModel.SetCloudsHalfRes(!checked)
                }

                Text {
                    x: 20; y: 97
                    text: "Reprojeção temporal"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 116
                    text: "acumula frames; integra o ruído do jitter"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 96
                    checked: viewportModel.cloudsTemporal
                    onToggled: viewportModel.SetCloudsTemporal(!checked)
                }

                ShadowSlider {
                    x: 20; y: 150
                    width: parent.width - 40
                    label: "Passos do raymarch"
                    from: 32; to: 256; step: 8
                    value: viewportModel.cloudMarchSteps
                    valueText: viewportModel.cloudMarchSteps + ""
                    onCommitted: (v) => viewportModel.SetCloudMarchSteps(v)
                }
            }
            } // Column cloudsCol
        }

        // Página 8: Clima — chuva/wetness (FWeather via viewportModel). Veio da janela
        // Time of Day: clima é estado de cena/render, não de relógio.
        Flickable {
            id: weatherPage
            visible: root.selectedPage === 8
            anchors.fill: parent
            anchors.topMargin: 84
            contentWidth: width
            contentHeight: weatherCol.height + 40
            clip: true
            ScrollBar.vertical: ThinScrollBar { revealed: weatherPageHover.hovered }
            HoverHandler { id: weatherPageHover }

            Column {
                id: weatherCol
                x: 24
                width: weatherPage.width - 48
                spacing: 16

            Card {
                width: parent.width
                height: 310
                title: "Chuva"

                Text {
                    x: 20; y: 55
                    text: "Intensidade e cortina"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "knob mestre do wetness deferred + streaks na frente da câmera"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Chuva"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.rainAmount
                    valueText: Math.round(viewportModel.rainAmount * 100) + "%"
                    onCommitted: (v) => viewportModel.SetRainAmount(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Cortina de gotas"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.curtainAmount
                    valueText: Math.round(viewportModel.curtainAmount * 100) + "%"
                    onCommitted: (v) => viewportModel.SetCurtainAmount(v)
                }

                Text {
                    x: 20; y: 216
                    text: "Oclusão da chuva"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 216
                    text: "só molha o que vê o céu — interior seco"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 210
                    checked: viewportModel.rainOcclusion
                    onToggled: viewportModel.SetRainOcclusion(!checked)
                }

                Text {
                    x: 20; y: 246
                    text: "Gotas por partícula"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 246
                    text: "quads GPU no near-field; off = só cortina"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 240
                    checked: viewportModel.rainParticles
                    onToggled: viewportModel.SetRainParticles(!checked)
                }

                Text {
                    x: 20; y: 276
                    text: "Chuva dirige o céu"
                    color: root.textNormal
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 276
                    text: "nublado, key light e fog seguem a chuva"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 270
                    checked: viewportModel.weatherDriveSky
                    onToggled: viewportModel.SetWeatherDriveSky(!checked)
                }
            }

            Card {
                width: parent.width
                height: 322
                title: "Molhado e poças"

                Text {
                    x: 20; y: 55
                    text: "Resposta do chão à chuva"
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "acúmulo em ~5 s de chuva, seca em ~30 s depois que para"
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Poças"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.puddleAmount
                    valueText: Math.round(viewportModel.puddleAmount * 100) + "%"
                    onCommitted: (v) => viewportModel.SetPuddleAmount(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Tamanho das poças"
                    from: 2; to: 32; step: 1
                    value: viewportModel.puddleScale
                    valueText: Math.round(viewportModel.puddleScale) + " m"
                    onCommitted: (v) => viewportModel.SetPuddleScale(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Ondulação das gotas"
                    from: 0; to: 2; step: 0.01
                    value: viewportModel.rippleStrength
                    valueText: viewportModel.rippleStrength.toFixed(2).replace(".", ",")
                    onCommitted: (v) => viewportModel.SetRippleStrength(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Escurecimento molhado"
                    from: 0; to: 1; step: 0.01
                    value: viewportModel.wetDarkening
                    valueText: Math.round(viewportModel.wetDarkening * 100) + "%"
                    onCommitted: (v) => viewportModel.SetWetDarkening(v)
                }
            }
            } // Column weatherCol
        }

        // ---- Pagina 1: Iluminacao global ----------------------------------------------------
        // Painel de CALIBRACAO dos epsilons de raio. Uma coluna so (os controles sao largos por
        // causa da explicacao de cada knob) e Column/Repeater: nenhum slider sabe seu proprio y,
        // entao acrescentar ou tirar um knob na tabela do C++ reflui a pagina sozinho.
        Flickable {
            id: giPage
            visible: root.selectedPage === 1
            anchors.fill: parent
            anchors.topMargin: 84
            contentWidth: width
            contentHeight: giCol.height + 40
            clip: true
            ScrollBar.vertical: ThinScrollBar { revealed: giPageHover.hovered }
            HoverHandler { id: giPageHover }

            // SNAPSHOT do model, nao binding direto em viewportModel.rayEpsilons. Escrever um
            // epsilon emite ViewSettingsChanged, o que re-le a propriedade e trocaria a lista do
            // Repeater — reconstruindo os delegates DEBAIXO do mouse durante o arrasto. Recarrega
            // so nos momentos discretos: abrir a pagina e restaurar padroes.
            property var epsModel: []
            function reloadEps() {
                epsModel = viewportModel.rayEpsilons
                biasMaxSlider.uiValue = viewportModel.giSurfaceBiasMax
                fadeSlider.uiValue    = viewportModel.giVolumeFadeProbes
            }
            Component.onCompleted: reloadEps()
            onVisibleChanged: if (visible) reloadEps()

            Column {
                id: giCol
                x: 24
                width: parent.width - 48
                spacing: 16

                Card {
                    id: rayEpsCard
                    width: parent.width
                    title: "Epsilons de raio — calibração"
                    height: epsCol.y + epsCol.height + contentPadding

                    Text {
                        id: epsHelper
                        x: 20
                        y: rayEpsCard.headerHeight + rayEpsCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Geometria dos raios de GI, reflexo e sombra — um perfil só para a " +
                              "engine inteira. Mudar qualquer valor limpa os reservoirs do ReSTIR " +
                              "e o histórico do denoiser, senão o A/B compararia um estado " +
                              "misturado. As três famílias cobrem fenômenos diferentes e não " +
                              "devem ser varridas juntas: origem do raio, intervalo do segmento " +
                              "e idade da amostra."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    Column {
                        id: epsCol
                        x: 20
                        y: epsHelper.y + epsHelper.height + 16
                        width: parent.width - 40
                        spacing: 12

                        Repeater {
                            model: giPage.epsModel
                            delegate: Item {
                                id: knobRow
                                required property var modelData
                                // Valor "ao vivo": o rotulo acompanha o arrasto sem tocar na
                                // engine. A escrita real so acontece ao soltar (ver onReleased),
                                // senao cada pixel de arrasto limparia reservoirs, atlas do DDGI,
                                // historico do NRD e do TAA.
                                property real uiValue: modelData.value
                                width: epsCol.width
                                height: knobSlider.height + knobHint.height

                                ShadowSlider {
                                    id: knobSlider
                                    width: parent.width
                                    label: knobRow.modelData.label
                                    from: knobRow.modelData.min
                                    to: knobRow.modelData.max
                                    step: Math.pow(10, -knobRow.modelData.decimals)
                                    value: knobRow.uiValue
                                    valueText: knobRow.uiValue
                                                   .toFixed(knobRow.modelData.decimals)
                                                   .replace(".", ",") + " " + knobRow.modelData.unit
                                    onCommitted: (v) => knobRow.uiValue = v
                                    onReleased: (v) => {
                                        knobRow.uiValue = v
                                        viewportModel.SetRayEpsilon(knobRow.modelData.key, v)
                                    }
                                }
                                Text {
                                    id: knobHint
                                    y: knobSlider.height
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    text: knobRow.modelData.hint
                                    color: root.textMuted
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 10
                                    lineHeight: 1.3
                                }
                            }
                        }

                        Rectangle {
                            id: epsReset
                            width: 148; height: 30; radius: 7
                            color: epsResetHover.hovered ? "#23241d" : "transparent"
                            border.color: root.borderColor
                            border.width: 1
                            Text {
                                anchors.centerIn: parent
                                text: "Restaurar padrões"
                                color: root.textNormal
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 11
                            }
                            HoverHandler { id: epsResetHover }
                            TapHandler {
                                onTapped: {
                                    viewportModel.ResetRayEpsilons()
                                    giPage.reloadEps() // o snapshot nao se atualiza sozinho
                                }
                            }
                        }
                    }
                }

                // Card proprio, e nao mais um knob da tabela de epsilons: isto nao e um valor
                // continuo p/ varrer, e sim uma troca de REGIME da TLAS (reconstroi a estrutura).
                Card {
                    id: rtCullCard
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
                        checked: viewportModel.reflectionsCullBackface
                        onToggled: viewportModel.ToggleReflectionsCullBackface()
                    }
                }

                Card {
                    id: backfaceCard
                    width: parent.width
                    title: "Backface no gather do ReSTIR"
                    height: bfLabel.y + bfLabel.height + contentPadding + 8

                    Text {
                        id: bfHelper
                        x: 20
                        y: backfaceCard.headerHeight + backfaceCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "O gather traça sem back-face culling e classifica cada acerto por " +
                              "conta própria. Um acerto no verso a menos de 5 cm (ou de material " +
                              "two-sided a menos de 1 cm) é tratado como auto-interseção e o raio " +
                              "é relançado dali; o verso que sobrar é geometria real vista por " +
                              "dentro, e o caminho termina sem luz em vez de seguir até o céu.\n\n" +
                              "Vale para o gather e para a revalidação temporal.\n\n" +
                              "Desligada por padrão: como o gather já traça sem culling, o verso " +
                              "bloqueia o raio por conta própria, e a medição no alvo cru não " +
                              "mostrou diferença visível — só dentro da geometria. Ela existe " +
                              "como defesa: passa a valer se o culling voltar ao gather, e o " +
                              "passo do relançamento protege contra auto-interseção, que é outro " +
                              "sintoma. Ligada, espere alguma sobre-oclusão em cantos."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: bfLabel
                        x: 20
                        y: bfHelper.y + bfHelper.height + 18
                        text: "Política de backface"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        id: bfToggle
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: bfLabel.y - 6
                        checked: viewportModel.giBackfacePolicy
                        onToggled: viewportModel.ToggleGIBackfacePolicy()
                    }
                }

                Card {
                    id: regirCard
                    width: parent.width
                    title: "ReGIR — luzes nos hits secundários"
                    height: regirLabel.y + regirLabel.height + contentPadding + 8

                    Text {
                        id: regirHelper
                        x: 20
                        y: regirCard.headerHeight + regirCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Troca o loop de todas as luzes locais dentro dos hits de GI e " +
                              "reflexão por um pool de reservoirs em grade de mundo: 16×8×16 " +
                              "células, 64 amostras por célula, 8 propostas no hit e um único " +
                              "shadow ray para a vencedora. O histórico é limitado a 8 quadros " +
                              "e luzes dinâmicas são reponderadas a cada construção.\n\n" +
                              "Não substitui o ReSTIR DI: a superfície primária continua no " +
                              "caminho de tela, onde a reprojeção é mais precisa. Fora da AABB " +
                              "do grid, o shader cai no loop completo como referência.\n\n" +
                              "Default OFF para A/B. Compare principalmente reflexos e a " +
                              "estabilidade do DDGI com muitas luzes locais."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: regirLabel
                        x: 20
                        y: regirHelper.y + regirHelper.height + 18
                        text: "ReGIR nos hits secundários"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: regirLabel.y - 6
                        checked: viewportModel.reGIREnabled
                        onToggled: viewportModel.ToggleReGIR()
                    }
                }

                Card {
                    id: diLiteCard
                    width: parent.width
                    title: "Sombra por raio nas luzes sem orçamento (DI-lite)"
                    height: diLabel.y + diLabel.height + contentPadding + 8

                    Text {
                        id: diHelper
                        x: 20
                        y: diLiteCard.headerHeight + diLiteCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "O orçamento de shadow map é fixo: 8 spots e 4 points por quadro. " +
                              "A luz que não ganha slice continua iluminando, mas SEM oclusão " +
                              "nenhuma — vaza parede. Aumentar o orçamento custa VRAM e um passe " +
                              "de rasterização por luz.\n\n" +
                              "Ligado, essas luzes ganham sombra por ray tracing com UM raio por " +
                              "PIXEL, não por luz: todas as excedentes entram num sorteio " +
                              "ponderado, uma vence, e um único raio mede a visibilidade dela. O " +
                              "estimador devolve a contribuição do conjunto sem viés.\n\n" +
                              "A luz com slice não muda de caminho, e a marcada para não projetar " +
                              "sombra continua sem — a divisão é por luz, e o quadro inteiro " +
                              "soma sempre a mesma energia.\n\n" +
                              "DESLIGADO por padrão: o sinal é de um raio por pixel, então tem " +
                              "ruído de visibilidade. Sob DLSS Ray Reconstruction é o que a rede " +
                               "espera receber; sob NRD ele não passa por denoiser nenhum, porque " +
                               "o NRD trata GI e reflexão, não este alvo. O ReSTIR DI experimental " +
                               "abaixo já acrescenta reuso temporal e espacial para A/B.\n\n" +
                              "Quando há excedente: os dois orçamentos são INDEPENDENTES, então " +
                              "basta passar de 8 spots OU de 4 points — contando só os visíveis e " +
                              "com \"projeta sombras\" ligado. Não é o total de luzes: 8 spots e " +
                              "4 points não estouram nada, e 9 spots estouram mesmo sem nenhum " +
                              "point na cena.\n\n" +
                              "Onde olhar: a luz que ficou sem slice — a sombra dela aparece. " +
                              "Costumam ser as mais distantes, porque a seleção do orçamento " +
                              "ranqueia por influência na câmera."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: diLabel
                        x: 20
                        y: diHelper.y + diHelper.height + 18
                        text: "DI-lite"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        id: diToggle
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: diLabel.y - 6
                        checked: viewportModel.diLite
                        onToggled: viewportModel.ToggleDILite()
                    }
                }

                Card {
                    id: restirDICard
                    width: parent.width
                    title: "ReSTIR DI experimental"
                    height: restirDILabel.y + restirDILabel.height + contentPadding + 8

                    Text {
                        id: restirDIHelper
                        x: 20
                        y: restirDICard.headerHeight + restirDICard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Substitui o loop inteiro de luzes locais analíticas por 8 candidatas uniformes " +
                              "por pixel, reuso temporal e 4 vizinhos espaciais. Apenas a amostra " +
                              "final dispara shadow ray; sol e lua continuam no caminho dedicado.\n\n" +
                              "O DI-lite permanece como referência A/B e os dois modos são " +
                              "mutuamente exclusivos. Default OFF enquanto a integração direta " +
                              "com o NRD ainda não estiver fechada."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: restirDILabel
                        x: 20
                        y: restirDIHelper.y + restirDIHelper.height + 18
                        text: "ReSTIR DI"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: restirDILabel.y - 6
                        checked: viewportModel.reSTIRDI
                        onToggled: viewportModel.ToggleReSTIRDI()
                    }
                }

                Card {
                    id: ddgiSampleCard
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
                        // (escrever emite ViewSettingsChanged e o valor saltaria de volta no meio
                        // do arrasto); recarrega em reloadEps().
                        property real uiValue: 0.0
                        value: uiValue
                        valueText: uiValue === 0.0
                                       ? "sem teto"
                                       : uiValue.toFixed(2).replace(".", ",") + " m"
                        onCommitted: (v) => biasMaxSlider.uiValue = v
                        onReleased: (v) => {
                            biasMaxSlider.uiValue = v
                            viewportModel.SetGISurfaceBiasMax(v)
                        }
                    }

                }

                Card {
                    id: volumeFadeCard
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
                            viewportModel.SetGIVolumeFadeProbes(v)
                        }
                    }
                }
            }
        }

        Item {
            visible: root.selectedPage !== 0 && root.selectedPage !== 1 && root.selectedPage !== 6 &&
                     root.selectedPage !== 7 && root.selectedPage !== 8
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
            color: resetHover.hovered ? "#23241d" : "transparent"
            border.color: "#33342c"
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: "Restaurar padrões"
                color: root.textNormal
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
            }
            HoverHandler { id: resetHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: viewportModel.ResetRenderSettings() }
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
                color: "#10110f"
                font.family: C.Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            HoverHandler { id: applyHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: settingsWindow.close() }
        }
    }
}
