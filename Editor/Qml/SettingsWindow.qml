import QtQuick
import QtQuick.Controls
import "components" as C

// Janela de configuracoes do editor. A pagina de Renderizacao esta funcional; as demais
// categorias preservam a estrutura da referencia e recebem conteudo nas proximas iteracoes.
Rectangle {
    id: root
    required property var viewportModel
    // Knobs de render (RenderSettingsBridge). Separado do viewportModel, que segue dono do
    // view mode, do visualizador de debug e da telemetria.
    required property var renderModel
    // Bookmarks de camera (CameraBookmarksBridge). Como as demais, TEM de ser declarada aqui: a
    // propriedade injetada pelo CreateQmlPanel so chega ao QML por esta porta. Sem a declaracao
    // ela fica undefined, e todo binding e todo clique falham em silencio — sem erro visivel,
    // porque um handler que chama metodo de undefined so aborta aquele handler.
    required property var cameraBookmarks
    // Captura determinística (CaptureBridge). Mesma regra da de cima — sem esta linha o botão de
    // capturar não faz nada e não diz nada.
    required property var capture
    required property var settingsWindow

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
        if (selectedPage === 4)
            return "Espectro físico, ganho das ondas e deslocamento geométrico do oceano FFT"
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
                            visible: renderModel.upscalerMode === 1
                            text: "FSR"
                            color: root.amdRed
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: renderModel.upscalerMode === 2
                            text: "DLSS"
                            color: root.nvidiaGreen
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: renderModel.upscalerMode === 0
                            text: "1:1"
                            color: root.textNormal
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: renderModel.upscalerMode === 2
                              ? "NVIDIA DLSS"
                              : renderModel.upscalerMode === 1
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
                        text: renderModel.upscalerMode === 2
                              ? "DLSS · Reconstrução por IA (Super Resolution)"
                              : renderModel.upscalerMode === 1
                                ? "FSR 3.1 · Reconstrução Temporal"
                                : "TAA · Escala Manual de Renderização"
                        color: root.textMuted
                        elide: Text.ElideRight
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Rectangle {
                        visible: renderModel.upscalerMode === renderModel.recommendedUpscalerMode
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
                    text: renderModel.fsrAvailable
                          ? "O recomendado é escolhido conforme o hardware e os backends disponíveis."
                          : "FSR 3.1 indisponível; usando o caminho nativo com TAA."
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }

                Item {
                    id: fsrControls
                    visible: renderModel.upscalerMode === 1 || renderModel.upscalerMode === 2
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
                        text: (renderModel.upscalerMode === 2 ? "DLSS" : "FSR") +
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
                                    readonly property bool isCurrent: renderModel.upscalerQuality === index
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
                                        onTapped: renderModel.SetUpscalerQuality(index)
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
                                text: renderModel.renderScale < 0.999
                                      ? "−" + Math.round((1.0 - renderModel.renderScale *
                                                         renderModel.renderScale) * 100) + "% Pixels"
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
                    visible: renderModel.upscalerMode === 0
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
                        checked: renderModel.taaEnabled
                        onToggled: renderModel.SetTAAEnabled(!checked)
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
                            text: renderModel.renderScale.toFixed(2).replace(".", ",") + "×"
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
                        value: renderModel.renderScale
                        onPressedChanged: {
                            if (!pressed) renderModel.SetRenderScale(value)
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
                    visible: renderModel.upscalerMode !== renderModel.recommendedUpscalerMode
                    Text {
                        text: "Recomendado para esta GPU:"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Text {
                        id: useRecommendedLink
                        text: "Usar " + renderModel.recommendedUpscalerName
                        color: root.blue
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                        font.underline: useRecommendedHover.hovered
                        HoverHandler {
                            id: useRecommendedHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: renderModel.SetUpscalerMode(renderModel.recommendedUpscalerMode)
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
                            selected: renderModel.upscalerMode === 0
                            badge: renderModel.recommendedUpscalerMode === 0 ? "RECOMENDADO" : ""
                            onChosen: {
                                renderModel.SetUpscalerMode(0)
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
                            selected: renderModel.upscalerMode === 1
                            available: renderModel.fsrAvailable
                            badge: renderModel.recommendedUpscalerMode === 1 ? "RECOMENDADO" : ""
                            onChosen: {
                                renderModel.SetUpscalerMode(1)
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
                            selected: renderModel.upscalerMode === 2
                            available: renderModel.dlssAvailable
                            badge: renderModel.recommendedUpscalerMode === 2 ? "RECOMENDADO" : ""
                            onChosen: {
                                renderModel.SetUpscalerMode(2)
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
                            text: renderModel.denoiserMode === 2 ? "RR"
                                  : (renderModel.denoiserMode === 1 ? "NRD" : "OFF")
                            color: renderModel.denoiserMode === 2 ? root.nvidiaGreen
                                   : (renderModel.denoiserMode === 1 ? root.textNormal : root.textMuted)
                            font.family: C.Theme.fontFamily
                            font.pixelSize: renderModel.denoiserMode === 1 ? 8 : 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        x: 49; y: 7
                        text: renderModel.denoiserMode === 2 ? "DLSS Ray Reconstruction"
                              : (renderModel.denoiserMode === 1 ? "NRD RELAX" : "Nenhum")
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                    Text {
                        x: 49; y: 24
                        width: parent.width - 80
                        text: renderModel.denoiserMode === 2
                              ? "Denoiser neural (NVIDIA) · substitui NRD + upscaler"
                              : (renderModel.denoiserMode === 1
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
                    text: renderModel.rrAvailable
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
                            selected: renderModel.denoiserMode === 0
                            onChosen: { renderModel.SetDenoiserMode(0); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 1
                            label: "NRD RELAX"
                            detail: "A-trous · difuso (GI) + especular (reflexão)"
                            selected: renderModel.denoiserMode === 1
                            onChosen: { renderModel.SetDenoiserMode(1); denoiserPopup.close() }
                        }
                        DenoiserOption {
                            width: denoiserPopup.availableWidth
                            mode: 2
                            label: "DLSS Ray Reconstruction"
                            detail: available ? "IA · substitui NRD + upscaler (trava DLSS)"
                                              : "Indisponível (requer GPU NVIDIA RTX)"
                            selected: renderModel.denoiserMode === 2
                            available: renderModel.rrAvailable
                            onChosen: { renderModel.SetDenoiserMode(2); denoiserPopup.close() }
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
                    checked: renderModel.depthPrepassEnabled
                    onToggled: renderModel.SetDepthPrepassEnabled(!checked)
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
                    checked: renderModel.frustumCullingEnabled
                    onToggled: renderModel.SetFrustumCullingEnabled(!checked)
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
                    checked: renderModel.occlusionCullingEnabled
                    onToggled: renderModel.SetOcclusionCullingEnabled(!checked)
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
            id: oceanPage
            visible: root.selectedPage === 4
            anchors.fill: parent
            anchors.topMargin: 84
            contentWidth: width
            contentHeight: oceanCol.height + 40
            clip: true
            ScrollBar.vertical: ThinScrollBar { revealed: oceanPageHover.hovered }
            HoverHandler { id: oceanPageHover }

            Column {
                id: oceanCol
                x: 24
                width: oceanPage.width - 48
                spacing: 16

                Card {
                    width: parent.width
                    height: 112
                    title: "Oceano FFT"

                    Text {
                        x: 20; y: 55
                        text: "Renderizar superfície do oceano"
                        color: root.textPrimary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Text {
                        x: 20; y: 76
                        text: "mantém o toggle do ambiente e todos os parâmetros configurados"
                        color: root.textMuted
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                    }
                    Toggle {
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        y: 54
                        checked: renderModel.oceanEnabled
                        onToggled: renderModel.SetOceanEnabled(!checked)
                    }
                }

                Card {
                    width: parent.width
                    height: 390
                    title: "Espectro físico"

                    ShadowSlider {
                        x: 20; y: 55
                        width: parent.width - 40
                        label: "Direção do vento"
                        from: 0; to: 360; step: 1
                        value: renderModel.oceanWindDirectionDegrees
                        valueText: Math.round(renderModel.oceanWindDirectionDegrees) + "°"
                        onReleased: (v) => renderModel.SetOceanWindDirectionDegrees(v)
                    }
                    ShadowSlider {
                        x: 20; y: 107
                        width: parent.width - 40
                        label: "Velocidade do vento"
                        from: 0.1; to: 40; step: 0.1
                        value: renderModel.oceanWindSpeed
                        valueText: renderModel.oceanWindSpeed.toFixed(1).replace(".", ",") + " m/s"
                        onReleased: (v) => renderModel.SetOceanWindSpeed(v)
                    }
                    ShadowSlider {
                        x: 20; y: 159
                        width: parent.width - 40
                        label: "Fetch"
                        from: 1; to: 1000; step: 1
                        value: renderModel.oceanFetchKm
                        valueText: Math.round(renderModel.oceanFetchKm) + " km"
                        onReleased: (v) => renderModel.SetOceanFetchKm(v)
                    }
                    ShadowSlider {
                        x: 20; y: 211
                        width: parent.width - 40
                        label: "Profundidade"
                        from: 1; to: 5000; step: 1
                        value: renderModel.oceanDepthM
                        valueText: Math.round(renderModel.oceanDepthM) + " m"
                        onReleased: (v) => renderModel.SetOceanDepthM(v)
                    }
                    ShadowSlider {
                        x: 20; y: 263
                        width: parent.width - 40
                        label: "Swell"
                        from: 0; to: 1; step: 0.01
                        value: renderModel.oceanSwell
                        valueText: renderModel.oceanSwell.toFixed(2).replace(".", ",")
                        onReleased: (v) => renderModel.SetOceanSwell(v)
                    }
                    ShadowSlider {
                        x: 20; y: 315
                        width: parent.width - 40
                        label: "Ganho das ondas (linear)"
                        from: 0; to: 4; step: 0.05
                        value: renderModel.oceanWavesAmount
                        valueText: "×" + renderModel.oceanWavesAmount.toFixed(2).replace(".", ",")
                        onReleased: (v) => renderModel.SetOceanWavesAmount(v)
                    }
                }

                Card {
                    width: parent.width
                    height: 166
                    title: "Deslocamento FFT"

                    ShadowSlider {
                        x: 20; y: 55
                        width: parent.width - 40
                        label: "Amplitude do deslocamento"
                        from: 0; to: 4; step: 0.05
                        value: renderModel.oceanFFTDisplacement
                        valueText: "×" + renderModel.oceanFFTDisplacement.toFixed(2).replace(".", ",")
                        onCommitted: (v) => renderModel.SetOceanFFTDisplacement(v)
                    }
                    ShadowSlider {
                        x: 20; y: 107
                        width: parent.width - 40
                        label: "Choppy horizontal"
                        from: 0; to: 4; step: 0.05
                        value: renderModel.oceanFFTChoppy
                        valueText: "×" + renderModel.oceanFFTChoppy.toFixed(2).replace(".", ",")
                        onCommitted: (v) => renderModel.SetOceanFFTChoppy(v)
                    }
                }
            }
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
                    checked: renderModel.sunShadowsEnabled
                    onToggled: renderModel.SetSunShadowsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Distância máxima"
                    from: 100; to: 3000; step: 50
                    value: renderModel.shadowMaxDistance
                    valueText: Math.round(renderModel.shadowMaxDistance) + " m"
                    onCommitted: (v) => renderModel.SetShadowMaxDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Bias de profundidade"
                    from: 0; to: 8; step: 0.1
                    value: renderModel.shadowDepthBias
                    valueText: renderModel.shadowDepthBias.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => renderModel.SetShadowDepthBias(v)
                }
                // Em texels, como o bias — e portanto também 4× maior em metros a cada
                // cascata. Junto com o bias é o que apaga sombra de contato nas cascatas
                // distantes: 2,5 tx valem 5,9 cm na cascata 0 e 23,2 cm na 1.
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Normal offset"
                    from: 0; to: 6; step: 0.1
                    value: renderModel.shadowNormalOffset
                    valueText: renderModel.shadowNormalOffset.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => renderModel.SetShadowNormalOffset(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Tamanho angular do sol (PCSS)"
                    from: 0; to: 2.0; step: 0.01
                    value: renderModel.shadowSunAngle
                    valueText: renderModel.shadowSunAngle < 0.005
                               ? "off"
                               : renderModel.shadowSunAngle.toFixed(2).replace(".", ",") + "°"
                    onCommitted: (v) => renderModel.SetShadowSunAngle(v)
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
                    checked: renderModel.shadowDebugCascades
                    onToggled: renderModel.SetShadowDebugCascades(!checked)
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
                    checked: renderModel.sunShaftsEnabled
                    onToggled: renderModel.SetSunShaftsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Intensidade"
                    from: 0; to: 5.0; step: 0.1
                    value: renderModel.sunShaftsIntensity
                    valueText: renderModel.sunShaftsIntensity.toFixed(1).replace(".", ",")
                    onCommitted: (v) => renderModel.SetSunShaftsIntensity(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Poeira — densidade do feixe"
                    from: 1; to: 64; step: 1
                    value: renderModel.sunShaftsDust
                    valueText: "×" + Math.round(renderModel.sunShaftsDust)
                    onCommitted: (v) => renderModel.SetSunShaftsDust(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.95; step: 0.01
                    value: renderModel.sunShaftsPhaseG
                    valueText: renderModel.sunShaftsPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetSunShaftsPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Passos do raymarch"
                    from: 8; to: 64; step: 4
                    value: renderModel.sunShaftsSteps
                    valueText: renderModel.sunShaftsSteps + ""
                    onCommitted: (v) => renderModel.SetSunShaftsSteps(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Alcance da marcha (m)"
                    from: 32; to: 400; step: 8
                    value: renderModel.sunShaftsRange
                    valueText: Math.round(renderModel.sunShaftsRange) + " m"
                    onCommitted: (v) => renderModel.SetSunShaftsRange(v)
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
                    checked: renderModel.sunShaftsTemporal
                    onToggled: renderModel.SetSunShaftsTemporal(!checked)
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
                    checked: renderModel.volFogEnabled
                    onToggled: renderModel.SetVolFogEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Alcance do volume (m)"
                    from: 30; to: 300; step: 10
                    value: renderModel.volFogDistance
                    valueText: Math.round(renderModel.volFogDistance) + " m"
                    onCommitted: (v) => renderModel.SetVolFogDistance(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Densidade do volume"
                    from: 0.25; to: 8.0; step: 0.25
                    value: renderModel.volFogDensity
                    valueText: "×" + renderModel.volFogDensity.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetVolFogDensity(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Fase HG (g) — lobo contra a luz"
                    from: 0; to: 0.9; step: 0.05
                    value: renderModel.volFogPhaseG
                    valueText: renderModel.volFogPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetVolFogPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Ambiente (DDGI/céu)"
                    from: 0; to: 4.0; step: 0.1
                    value: renderModel.volFogAmbient
                    valueText: "×" + renderModel.volFogAmbient.toFixed(1).replace(".", ",")
                    onCommitted: (v) => renderModel.SetVolFogAmbient(v)
                }
                ShadowSlider {
                    x: 20; y: 316
                    width: parent.width - 40
                    label: "Luzes no ar (halo de point/spot)"
                    from: 0; to: 4.0; step: 0.1
                    value: renderModel.volFogLights
                    valueText: "×" + renderModel.volFogLights.toFixed(1).replace(".", ",")
                    onCommitted: (v) => renderModel.SetVolFogLights(v)
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
                    checked: renderModel.volFogTemporal
                    onToggled: renderModel.SetVolFogTemporal(!checked)
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
                    checked: renderModel.volFogConsDepth
                    onToggled: renderModel.SetVolFogConsDepth(!checked)
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
                    value: renderModel.heightFogSkyContribution
                    valueText: renderModel.heightFogSkyContribution.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetHeightFogSkyContribution(v)
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
                    checked: renderModel.perPixelAtmoTransmittance
                    onToggled: renderModel.SetPerPixelAtmoTransmittance(!checked)
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
                    checked: renderModel.skyAmbientSH
                    onToggled: renderModel.SetSkyAmbientSH(!checked)
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
                    checked: renderModel.shadowCacheEnabled
                    onToggled: renderModel.SetShadowCacheEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Caster mínimo (texels da cascata)"
                    from: 0; to: 8; step: 0.5
                    value: renderModel.shadowMinCasterTexels
                    valueText: renderModel.shadowMinCasterTexels.toFixed(1).replace(".", ",") + " tx"
                    onCommitted: (v) => renderModel.SetShadowMinCasterTexels(v)
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
                            value: Number(renderModel.shadowCascadeBias[index])
                            valueText: "×" + Number(renderModel.shadowCascadeBias[index]).toFixed(2).replace(".", ",")
                            onCommitted: (v) => renderModel.SetShadowCascadeBiasScale(index, v)
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
                    checked: renderModel.cloudsEnabled
                    onToggled: renderModel.SetCloudsEnabled(!checked)
                }

                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Cobertura"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.cloudCoverage
                    valueText: Math.round(renderModel.cloudCoverage * 100) + " %"
                    onCommitted: (v) => renderModel.SetCloudCoverage(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Densidade (extinção /km)"
                    from: 0.1; to: 10.0; step: 0.1
                    value: renderModel.cloudDensity
                    valueText: renderModel.cloudDensity.toFixed(1).replace(".", ",")
                    onCommitted: (v) => renderModel.SetCloudDensity(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Altitude da base"
                    from: 0.5; to: 8.0; step: 0.1
                    value: renderModel.cloudBottomKm
                    valueText: renderModel.cloudBottomKm.toFixed(1).replace(".", ",") + " km"
                    onCommitted: (v) => renderModel.SetCloudAltitude(v, renderModel.cloudThicknessKm)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Espessura da camada"
                    from: 0.5; to: 8.0; step: 0.1
                    value: renderModel.cloudThicknessKm
                    valueText: renderModel.cloudThicknessKm.toFixed(1).replace(".", ",") + " km"
                    onCommitted: (v) => renderModel.SetCloudAltitude(renderModel.cloudBottomKm, v)
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
                    value: renderModel.cloudWeatherCells
                    valueText: renderModel.cloudWeatherCells + "×"
                    onCommitted: (v) => renderModel.SetCloudWeatherCells(v)
                }
                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Semente"
                    from: 0; to: 9999; step: 1
                    value: renderModel.cloudWeatherSeed
                    valueText: renderModel.cloudWeatherSeed + ""
                    onCommitted: (v) => renderModel.SetCloudWeatherSeed(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Viés de tipo (stratus ↔ cumulonimbus)"
                    from: -0.5; to: 0.5; step: 0.01
                    value: renderModel.cloudTypeBias
                    valueText: (renderModel.cloudTypeBias >= 0 ? "+" : "") +
                               renderModel.cloudTypeBias.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetCloudTypeBias(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Variação de topo (peak height, canal B)"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.cloudPeakVariation
                    valueText: Math.round(renderModel.cloudPeakVariation * 100) + " %"
                    onCommitted: (v) => renderModel.SetCloudPeakVariation(v)
                }

                Row {
                    x: 20; y: 268
                    spacing: 10
                    ActionButton {
                        label: "Semente aleatória"
                        onTapped: renderModel.SetCloudWeatherSeed(Math.floor(Math.random() * 10000))
                    }
                    ActionButton {
                        label: "Carregar textura…"
                        onTapped: renderModel.LoadCloudWeatherTexture()
                    }
                    ActionButton {
                        label: "Voltar ao procedural"
                        visible: renderModel.cloudWeatherAuthored
                        onTapped: renderModel.ClearCloudWeatherTexture()
                    }
                }
                Text {
                    x: 20; y: 306
                    text: renderModel.cloudWeatherAuthored
                          ? "fonte: textura autorada — R = cobertura · G = tipo · B = altura de topo"
                          : "fonte: procedural (seed " + renderModel.cloudWeatherSeed + ")"
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
                    checked: renderModel.cloudShadows
                    onToggled: renderModel.SetCloudShadowsEnabled(!checked)
                }
                ShadowSlider {
                    x: 20; y: 362
                    width: parent.width - 40
                    label: "Intensidade da sombra"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.cloudShadowStrength
                    valueText: Math.round(renderModel.cloudShadowStrength * 100) + " %"
                    onCommitted: (v) => renderModel.SetCloudShadowStrength(v)
                }

                ShadowSlider {
                    x: 20; y: 56
                    width: parent.width - 40
                    label: "Erosão de detalhe"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.cloudErosion
                    valueText: Math.round(renderModel.cloudErosion * 100) + " %"
                    onCommitted: (v) => renderModel.SetCloudErosion(v)
                }
                ShadowSlider {
                    x: 20; y: 108
                    width: parent.width - 40
                    label: "Vento"
                    from: 0; to: 0.05; step: 0.001
                    value: renderModel.cloudWindSpeed
                    valueText: Math.round(renderModel.cloudWindSpeed * 1000) + " m/s"
                    onCommitted: (v) => renderModel.SetCloudWindSpeed(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Anisotropia da fase (g)"
                    from: 0; to: 0.95; step: 0.01
                    value: renderModel.cloudPhaseG
                    valueText: renderModel.cloudPhaseG.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetCloudPhaseG(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Powder (auto-sombra de borda)"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.cloudPowder
                    valueText: Math.round(renderModel.cloudPowder * 100) + " %"
                    onCommitted: (v) => renderModel.SetCloudPowder(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Ambiente do céu"
                    from: 0; to: 3.0; step: 0.05
                    value: renderModel.cloudAmbient
                    valueText: "×" + renderModel.cloudAmbient.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetCloudAmbient(v)
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
                    checked: renderModel.cloudsHalfRes
                    onToggled: renderModel.SetCloudsHalfRes(!checked)
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
                    checked: renderModel.cloudsTemporal
                    onToggled: renderModel.SetCloudsTemporal(!checked)
                }

                ShadowSlider {
                    x: 20; y: 150
                    width: parent.width - 40
                    label: "Passos do raymarch"
                    from: 32; to: 256; step: 8
                    value: renderModel.cloudMarchSteps
                    valueText: renderModel.cloudMarchSteps + ""
                    onCommitted: (v) => renderModel.SetCloudMarchSteps(v)
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
                    value: renderModel.rainAmount
                    valueText: Math.round(renderModel.rainAmount * 100) + "%"
                    onCommitted: (v) => renderModel.SetRainAmount(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Cortina de gotas"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.curtainAmount
                    valueText: Math.round(renderModel.curtainAmount * 100) + "%"
                    onCommitted: (v) => renderModel.SetCurtainAmount(v)
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
                    checked: renderModel.rainOcclusion
                    onToggled: renderModel.SetRainOcclusion(!checked)
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
                    checked: renderModel.rainParticles
                    onToggled: renderModel.SetRainParticles(!checked)
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
                    checked: renderModel.weatherDriveSky
                    onToggled: renderModel.SetWeatherDriveSky(!checked)
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
                    value: renderModel.puddleAmount
                    valueText: Math.round(renderModel.puddleAmount * 100) + "%"
                    onCommitted: (v) => renderModel.SetPuddleAmount(v)
                }
                ShadowSlider {
                    x: 20; y: 160
                    width: parent.width - 40
                    label: "Tamanho das poças"
                    from: 2; to: 32; step: 1
                    value: renderModel.puddleScale
                    valueText: Math.round(renderModel.puddleScale) + " m"
                    onCommitted: (v) => renderModel.SetPuddleScale(v)
                }
                ShadowSlider {
                    x: 20; y: 212
                    width: parent.width - 40
                    label: "Ondulação das gotas"
                    from: 0; to: 2; step: 0.01
                    value: renderModel.rippleStrength
                    valueText: renderModel.rippleStrength.toFixed(2).replace(".", ",")
                    onCommitted: (v) => renderModel.SetRippleStrength(v)
                }
                ShadowSlider {
                    x: 20; y: 264
                    width: parent.width - 40
                    label: "Escurecimento molhado"
                    from: 0; to: 1; step: 0.01
                    value: renderModel.wetDarkening
                    valueText: Math.round(renderModel.wetDarkening * 100) + "%"
                    onCommitted: (v) => renderModel.SetWetDarkening(v)
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

            // SNAPSHOT do model, nao binding direto em renderModel.rayEpsilons. Escrever um
            // epsilon emite GISettingsChanged, o que re-le a propriedade e trocaria a lista do
            // Repeater — reconstruindo os delegates DEBAIXO do mouse durante o arrasto. Recarrega
            // so nos momentos discretos: abrir a pagina e restaurar padroes.
            property var epsModel: []
            function reloadEps() {
                epsModel = renderModel.rayEpsilons
                biasMaxSlider.uiValue = renderModel.giSurfaceBiasMax
                fadeSlider.uiValue    = renderModel.giVolumeFadeProbes
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
                                        renderModel.SetRayEpsilon(knobRow.modelData.key, v)
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
                                    renderModel.ResetRayEpsilons()
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
                        checked: renderModel.reflectionsCullBackface
                        onToggled: renderModel.ToggleReflectionsCullBackface()
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
                        checked: renderModel.giBackfacePolicy
                        onToggled: renderModel.ToggleGIBackfacePolicy()
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
                        checked: renderModel.reGIREnabled
                        onToggled: renderModel.ToggleReGIR()
                    }
                }

                Card {
                    id: radianceCacheCard
                    width: parent.width
                    title: "World radiance cache — terminador dos raios secundários"
                    height: rcStatsText.y + rcStatsText.height + contentPadding + 8

                    Text {
                        id: rcHelper
                        x: 20
                        y: radianceCacheCard.headerHeight + radianceCacheCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Guarda a radiância de saída que o ShadeSurfaceHit já calculou, num " +
                              "hash de mundo com célula de 50 cm perto da câmera dobrando com a " +
                              "distância. Um raio secundário que caia numa célula já preenchida " +
                              "retorna dali e pula albedo, shadow rays e o gather do DDGI.\n\n" +
                              "Não substitui o DDGI: o atlas de irradiância continua alimentando " +
                              "o deferred por pixel. O que muda é o 2º bounce, que passa a ler " +
                              "radiância de verdade em vez de irradiância tratada como tal.\n\n" +
                              "Ligue a ESCRITA primeiro, espere a ocupação subir, e só então a " +
                              "LEITURA. Ligando as duas juntas os primeiros quadros consultam uma " +
                              "tabela vazia e o resultado parece bug.\n\n" +
                              "O PRODUTOR DEDICADO troca quem alimenta a tabela. Desligado, ela " +
                              "aprende dos hits do render — cujo terminador é o DDGI, ou seja, o " +
                              "cache guarda um sinal cuja origem continua sendo o volume. Ligado, " +
                              "um passe próprio traça a partir do G-buffer e nunca lê sonda: é " +
                              "essa a fonte independente que o SHaRC exige. São dois estimadores, " +
                              "não dois níveis de qualidade — trocar limpa a tabela."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    Text {
                        id: rcEnabledLabel
                        x: 20
                        y: rcHelper.y + rcHelper.height + 18
                        text: "Escrita (alimenta o cache)"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: rcEnabledLabel.y - 6
                        checked: renderModel.radianceCacheEnabled
                        onToggled: renderModel.ToggleRadianceCache()
                    }

                    Text {
                        id: rcQueryLabel
                        x: 20
                        y: rcEnabledLabel.y + 30
                        text: "Leitura (consulta no hit)"
                        color: renderModel.radianceCacheEnabled ? root.textNormal : root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: rcQueryLabel.y - 6
                        enabled: renderModel.radianceCacheEnabled
                        checked: renderModel.radianceCacheQuery
                        onToggled: renderModel.ToggleRadianceCacheQuery()
                    }

                    Text {
                        id: rcStatsLabel
                        x: 20
                        y: rcQueryLabel.y + 30
                        text: "Instrumentar acerto/erro"
                        color: renderModel.radianceCacheQuery ? root.textNormal : root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: rcStatsLabel.y - 6
                        enabled: renderModel.radianceCacheQuery
                        checked: renderModel.radianceCacheStats
                        onToggled: renderModel.ToggleRadianceCacheStats()
                    }

                    Text {
                        id: rcDedicatedLabel
                        x: 20
                        y: rcStatsLabel.y + 30
                        text: "Produtor dedicado (path tracer esparso)"
                        color: renderModel.radianceCacheEnabled ? root.textNormal : root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: rcDedicatedLabel.y - 6
                        enabled: renderModel.radianceCacheEnabled
                        checked: renderModel.radianceCacheDedicatedUpdate
                        onToggled: renderModel.ToggleRadianceCacheDedicatedUpdate()
                    }

                    Text {
                        id: rcTerminalLabel
                        x: 20
                        y: rcDedicatedLabel.y + 30
                        text: "Terminal no cache anterior (multi-bounce)"
                        color: renderModel.radianceCacheDedicatedUpdate ? root.textNormal
                                                                        : root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: rcTerminalLabel.y - 6
                        enabled: renderModel.radianceCacheDedicatedUpdate
                        checked: renderModel.radianceCachePrevTerminal
                        onToggled: renderModel.ToggleRadianceCachePrevTerminal()
                    }

                    ShadowSlider {
                        id: rcFractionSlider
                        x: 20
                        y: rcTerminalLabel.y + 26
                        width: parent.width - 40
                        // Sem produtor dedicado a fração não controla nada. O ShadowSlider não
                        // tem estado desabilitado próprio (o label dele é sempre textNormal),
                        // então a opacidade é o que dá o sinal visual junto do enabled.
                        enabled: renderModel.radianceCacheDedicatedUpdate
                        opacity: renderModel.radianceCacheDedicatedUpdate ? 1.0 : 0.45
                        label: "Pixels que lançam caminho por quadro"
                        // Passo de 1/25: a seleção é uma permutação sobre o tile 5x5, então
                        // valores intermediários arredondam para a mesma contagem de células.
                        from: 0.04; to: 0.40; step: 0.04
                        value: renderModel.radianceCacheUpdateFraction
                        valueText: (renderModel.radianceCacheUpdateFraction * 100).toFixed(0) + "%"
                        // committed, e nao released: mudar a fração não invalida a tabela —
                        // muda só a taxa de amostragem, e o que já está guardado continua valendo.
                        onCommitted: (v) => renderModel.SetRadianceCacheUpdateFraction(v)
                    }

                    ShadowSlider {
                        id: rcVertsSlider
                        x: 20
                        y: rcFractionSlider.y + 50
                        width: parent.width - 40
                        enabled: renderModel.radianceCacheDedicatedUpdate
                        opacity: renderModel.radianceCacheDedicatedUpdate ? 1.0 : 0.45
                        label: "Vértices por caminho (custo = V+1 raios)"
                        from: 1; to: 4; step: 1
                        value: renderModel.radianceCacheMaxVertices
                        valueText: renderModel.radianceCacheMaxVertices + (
                                   renderModel.radianceCacheMaxVertices === 1 ? " vért." : " vért.")
                        // released, e nao committed: trocar o número de vértices limpa a tabela
                        // (a célula guarda energias diferentes em cada regime). Fazer isso a cada
                        // passo de um arraste limparia o cache quatro vezes seguidas.
                        onReleased: (v) => renderModel.SetRadianceCacheMaxVertices(v)
                    }

                    ShadowSlider {
                        id: rcMinRoughSlider
                        x: 20
                        y: rcVertsSlider.y + 50
                        width: parent.width - 40
                        enabled: renderModel.radianceCacheDedicatedUpdate
                        opacity: renderModel.radianceCacheDedicatedUpdate ? 1.0 : 0.45
                        label: "Roughness mínima do lobo que chega"
                        from: 0.0; to: 1.0; step: 0.05
                        value: renderModel.radianceCacheMinRoughness
                        valueText: renderModel.radianceCacheMinRoughness.toFixed(2).replace(".", ",")
                        onCommitted: (v) => renderModel.SetRadianceCacheMinRoughness(v)
                    }

                    ShadowSlider {
                        id: rcCellSlider
                        x: 20
                        y: rcMinRoughSlider.y + 50
                        width: parent.width - 40
                        label: "Aresta da célula (nível 0)"
                        from: 0.05; to: 2.0; step: 0.05
                        value: renderModel.radianceCacheCellSize
                        valueText: renderModel.radianceCacheCellSize.toFixed(2).replace(".", ",") + " m"
                        // released, e nao committed: trocar a aresta muda a CHAVE do hash e
                        // invalida a tabela inteira. Fazer isso a cada passo de um arraste
                        // limparia o cache dezenas de vezes por segundo.
                        onReleased: (v) => renderModel.SetRadianceCacheCellSize(v)
                    }

                    ShadowSlider {
                        id: rcLodSlider
                        x: 20
                        y: rcCellSlider.y + 50
                        width: parent.width - 40
                        label: "Distância do primeiro nível"
                        from: 2.0; to: 60.0; step: 1.0
                        value: renderModel.radianceCacheLodDistance
                        valueText: renderModel.radianceCacheLodDistance.toFixed(0) + " m"
                        onReleased: (v) => renderModel.SetRadianceCacheLodDistance(v)
                    }

                    ShadowSlider {
                        id: rcModeSlider
                        x: 20
                        y: rcLodSlider.y + 50
                        width: parent.width - 40
                        label: "Visualizador (janela de debug: \"radiance\")"
                        from: 0; to: 3; step: 1
                        value: renderModel.radianceCacheDebugMode
                        valueText: ["células", "cobertura", "nível", "convergência"][renderModel.radianceCacheDebugMode]
                        // Só troca o modo do visualizador: não invalida nada, então commit contínuo.
                        onCommitted: (v) => renderModel.SetRadianceCacheDebugMode(v)
                    }

                    Text {
                        id: rcStatsText
                        x: 20
                        y: rcModeSlider.y + 52
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: renderModel.radianceCacheSummary + "  ·  " +
                              renderModel.radianceCacheOccupancy.toFixed(1).replace(".", ",") +
                              "% de " + renderModel.radianceCacheMemoryMB.toFixed(0) + " MB"
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }

                    // Só enquanto o card está visível: o readback já existe de qualquer forma, mas
                    // formatar QString e relayout a 60 Hz sem ninguém olhando é custo puro.
                    Timer {
                        interval: 200
                        repeat: true
                        running: radianceCacheCard.visible && renderModel.radianceCacheEnabled
                        onTriggered: renderModel.RefreshRadianceCacheStats()
                    }
                }

                Card {
                    id: cameraBookmarksCard
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
                        visible: cameraBookmarks.sidecarWarning !== ""
                        height: visible ? implicitHeight : 0
                        text: "⚠ " + cameraBookmarks.sidecarWarning
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
                                width: camSlotsColumn.width
                                height: 26
                                readonly property bool filled:
                                    cameraBookmarks.slotsFilled.length > index
                                    && cameraBookmarks.slotsFilled[index]

                                Text {
                                    id: camSlotName
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "Câmera " + (index + 1)
                                    color: parent.filled ? root.textNormal : root.textSecondary
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 13
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 90
                                    width: parent.width - 90 - 150
                                    elide: Text.ElideRight
                                    // Pose legível: é o que permite conferir num diff do sidecar
                                    // que a captura de ontem e a de hoje partiram do mesmo lugar.
                                    //
                                    // Lê da PROPRIEDADE slotLabels, não do invocável SlotLabel():
                                    // uma chamada é avaliada uma vez e não reavalia, então
                                    // regravar um slot já preenchido deixaria a pose ANTIGA na
                                    // tela — justo a operação em que o usuário acabou de decidir
                                    // que a referência mudou.
                                    text: cameraBookmarks.slotLabels.length > index
                                          && cameraBookmarks.slotLabels[index] !== ""
                                          ? cameraBookmarks.slotLabels[index] : "vazia"
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
                                        onTapped: cameraBookmarks.Save(index)
                                    }
                                    ActionButton {
                                        label: "Ir"
                                        onTapped: cameraBookmarks.Restore(index)
                                    }
                                    // Restaurar E capturar, nesta ordem, num clique só. A ordem
                                    // do protocolo é preset → câmera → reset, e o reset acontece
                                    // dentro do motor no frame seguinte: as duas chamadas abaixo
                                    // terminam antes dele. Fazer isso em dois cliques deixaria
                                    // espaço para capturar da pose errada, que é o erro que os
                                    // slots existem para eliminar.
                                    ActionButton {
                                        label: "Capturar"
                                        opacity: camSlotRow.filled && !capture.busy ? 1.0 : 0.45
                                        onTapped: {
                                            if (capture.busy) return
                                            if (!cameraBookmarks.Restore(index)) return
                                            capture.Shoot(index, captureCard.warmupFrames,
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
                            interactive: !capture.busy
                            onToggled: captureCard.scientific = !captureCard.scientific
                        }

                        C.SliderRow {
                            label: "Frames de aquecimento (N)"
                            valueText: captureCard.warmupFrames.toFixed(0)
                            from: 0
                            to: 512
                            stepSize: 8
                            boundValue: captureCard.warmupFrames
                            interactive: !capture.busy
                            onMoved: (v) => captureCard.warmupFrames = Math.round(v)
                        }

                        Item {
                            width: parent.width
                            height: 26
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Hora do dia fixada"
                                color: capture.busy ? root.textMuted : root.textNormal
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
                                    opacity: capture.busy ? 0.45 : 1.0
                                    onTapped: {
                                        if (capture.busy) return
                                        var h = capture.CurrentTimeOfDayHours()
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
                                text: capture.busy
                                      ? "aquecendo — faltam " + capture.warmupRemaining + " frames"
                                      : (capture.lastResult !== "" ? capture.lastResult
                                                                   : "nenhuma captura nesta sessão")
                                color: capture.busy ? root.blue : root.textSecondary
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 11
                            }
                            ActionButton {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                label: "Capturar aqui"
                                opacity: capture.busy ? 0.45 : 1.0
                                // Sem restaurar pose: captura da câmera livre, para inspecionar um
                                // ponto que ainda não virou bookmark. O manifesto grava slot -1.
                                onTapped: {
                                    if (!capture.busy)
                                        capture.Shoot(-1, captureCard.warmupFrames,
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
                        running: captureCard.visible || capture.busy
                        onTriggered: {
                            capture.Poll()
                            // Semeia a hora fixada uma única vez, com o relógio do mundo. Depois
                            // disso ela é do operador: puxá-la do mundo a cada tique faria
                            // exatamente o que a fixação existe para impedir.
                            if (captureCard.pinnedHours < 0) {
                                var h = capture.CurrentTimeOfDayHours()
                                if (h >= 0) captureCard.pinnedHours = h
                            }
                        }
                    }
                }

                Card {
                    id: restirDICard
                    width: parent.width
                    title: "ReSTIR DI — direta local"
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
                              "Ligado, substitui integralmente a direta local raster e usa uma " +
                              "instância ReLAX dedicada sob NRD; sob Ray Reconstruction entrega " +
                              "o sinal cru. Desligado, o deferred volta ao loop raster com os " +
                              "shadow maps locais."
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
                        checked: renderModel.reSTIRDI
                        onToggled: renderModel.ToggleReSTIRDI()
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
                            renderModel.SetGISurfaceBiasMax(v)
                        }
                    }

                }

                Card {
                    id: ddgiAdaptiveCard
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
                        checked: renderModel.giAdaptiveHysteresis
                        onToggled: renderModel.ToggleGIAdaptiveHysteresis()
                    }
                }

                Card {
                    id: ddgiCascadesCard
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
                            model: [1, 2]
                            delegate: Rectangle {
                                width: 44; height: 26; radius: 4
                                color: renderModel.giCascadeCount === modelData
                                       ? C.Theme.accent : "transparent"
                                border.color: root.textSecondary
                                border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: renderModel.giCascadeCount === modelData
                                           ? "#ffffff" : root.textNormal
                                    font.family: C.Theme.fontFamily
                                    font.pixelSize: 13
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: renderModel.SetGICascadeCount(modelData)
                                }
                            }
                        }
                    }
                }

                Card {
                    id: ddgiAdaptiveRaysCard
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
                        checked: renderModel.giAdaptiveRays
                        onToggled: renderModel.ToggleGIAdaptiveRays()
                    }
                }

                Card {
                    id: ddgiMeasureCard
                    width: parent.width
                    title: "Medição — DDGI como terminador"
                    height: measureTermLabel.y + measureTermLabel.height + contentPadding + 8

                    Text {
                        id: measureTermHelper
                        x: 20
                        y: ddgiMeasureCard.headerHeight + ddgiMeasureCard.contentPadding
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        text: "Isto não é um ajuste de qualidade: é um experimento. Ligado, os " +
                              "raios param de fechar o caminho no DDGI — some o segundo quique " +
                              "das próprias sondas, o valor que o ReSTIR GI grava no reservoir " +
                              "e o indireto dos acertos de reflexão. Todo o resto continua no " +
                              "lugar e rodando: o volume é traçado, as sondas relocam, o atlas " +
                              "atualiza, e o difuso da tela ainda pode lê-lo.\n\n" +
                              "Serve para responder com número, e não com opinião, quanto o " +
                              "DDGI realmente entrega neste pipeline — que é a pergunta que " +
                              "decide se ele continua sendo o núcleo da iluminação indireta ou " +
                              "vira só a rede de segurança para começo frio, névoa e preset " +
                              "barato.\n\n" +
                              "Como comparar: espere convergir dos dois lados antes de olhar, " +
                              "porque trocar reinicia o atlas e os históricos — sem isso o lado " +
                              "desligado ainda estaria mostrando a energia que veio do DDGI. " +
                              "Olhe o interior de ambientes fechados e o fundo de superfícies " +
                              "voltadas para longe da luz, que é onde o terminador pesa mais; " +
                              "e olhe o custo por passe no profiler, porque o ganho de " +
                              "desempenho faz parte da resposta.\n\n" +
                              "Não deixe ligado fora da medição."
                        color: root.textSecondary
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 11
                        lineHeight: 1.35
                    }
                    Text {
                        id: measureTermLabel
                        x: 20
                        y: measureTermHelper.y + measureTermHelper.height + 18
                        text: "Cortar o terminador"
                        color: root.textNormal
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Toggle {
                        anchors.right: parent.right; anchors.rightMargin: 20
                        y: measureTermLabel.y - 6
                        checked: renderModel.giMeasureTerminatorOff
                        onToggled: renderModel.ToggleGIMeasureTerminatorOff()
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
                            renderModel.SetGIVolumeFadeProbes(v)
                        }
                    }
                }
            }
        }

        Item {
            visible: root.selectedPage !== 0 && root.selectedPage !== 1 && root.selectedPage !== 4 && root.selectedPage !== 6 &&
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
            TapHandler { onTapped: renderModel.ResetRenderSettings() }
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
