import QtQuick
import QtQuick.Controls

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

    readonly property color bg: "#141511"
    readonly property color sidebarBg: "#10110f"
    readonly property color cardBg: "#1a1b15"
    readonly property color hoverBg: "#22231c"
    readonly property color borderColor: "#2a2b24"
    readonly property color divider: "#23241d"
    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textSecondary: "#9a958a"
    readonly property color textMuted: "#6c6a61"
    readonly property color blue: "#5b9dff"
    readonly property color blueBg: "#16233f"
    readonly property color blueBorder: "#27406e"
    readonly property color green: "#9ac055"

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
            return "Upscaling, anti-aliasing e resolução interna do viewport"
        if (selectedPage === 6)
            return "Sombras do sol (CSM), sun shafts: cascatas, cache, bias e debug"
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

    component WindowButton: Rectangle {
        id: windowButton
        property string glyph
        property bool danger: false
        signal tapped()
        width: 46
        height: 40
        color: winHover.hovered ? (danger ? "#c42b1c" : "#23241d") : "transparent"
        Text {
            anchors.centerIn: parent
            text: windowButton.glyph
            color: winHover.hovered && windowButton.danger ? "#ffffff" : "#9a958a"
            font.family: "Segoe UI Symbol"
            font.pixelSize: windowButton.glyph === "□" ? 14 : 16
        }
        HoverHandler { id: winHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: windowButton.tapped() }
    }

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
        HoverHandler {
            cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler {
            enabled: toggle.interactive
            onTapped: toggle.toggled()
        }
    }

    component ShadowSlider: Item {
        id: srow
        property string label
        property real from: 0
        property real to: 1
        property real step: 0.01
        property real value: 0
        property string valueText: ""
        signal committed(real v)
        height: 46

        Text {
            x: 0; y: 0
            text: srow.label
            color: root.textNormal
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
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
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
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
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        HoverHandler { id: abtnHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: abtn.tapped() }
    }

    component Card: Rectangle {
        property string title
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1
        Text {
            x: 20; y: 18
            text: parent.title
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 13
            font.weight: Font.Medium
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: 40
            height: 1
            color: root.divider
        }
    }

    component StatusRow: Item {
        id: statusRow
        property string label
        property string value
        height: 20
        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: statusRow.label
            color: root.textMuted
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: statusRow.value
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
    }

    component RayStatusRow: Item {
        id: rayRow
        property string label
        property bool active: false
        height: 24
        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 7; height: 7; radius: 3.5
            color: rayRow.active ? root.green : root.textMuted
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: rayRow.label
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 12
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: rayRow.active ? "ligado" : "desligado"
            color: root.textMuted
            font.family: "Segoe UI"
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
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
                font.pixelSize: 11
                background: null
                onTextChanged: root.searchText = text
            }
        }

        Text {
            x: 20; y: 70
            text: "Gráficos"
            color: root.textMuted
            font.family: "Segoe UI"
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
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
            Text {
                x: 14; y: 27
                text: "Ultra — RT completo"
                color: root.textNormal
                font.family: "Segoe UI"
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
            font.family: "Segoe UI"
            font.pixelSize: 20
            font.weight: Font.Medium
        }
        Text {
            x: 24; y: 49
            text: root.pageSubtitle()
            color: root.textSecondary
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
                font.pixelSize: 11
            }
        }

        Item {
            id: renderingPage
            visible: root.selectedPage === 0
            anchors.fill: parent

            readonly property int rightW: 180
            readonly property int gap: 16
            readonly property int leftW: width - 48 - rightW - gap

            Card {
                id: upscalingCard
                x: 24; y: 84
                width: renderingPage.leftW
                height: 272
                title: "Upscaling e anti-aliasing"

                Text {
                    x: 20; y: 55
                    text: "FSR 2"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: viewportModel.fsr2Available
                          ? "Substitui o TAA quando ativo"
                          : "Disponível apenas em build Release"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 54
                    checked: viewportModel.fsr2Enabled
                    interactive: viewportModel.fsr2Available
                    onToggled: viewportModel.SetFsr2Enabled(!checked)
                }

                Text {
                    x: 20; y: 106
                    text: "Qualidade do FSR 2"
                    color: viewportModel.fsr2Available ? root.textNormal : root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Rectangle {
                    id: qualitySelector
                    x: 20; y: 126
                    width: parent.width - 40
                    height: 26
                    radius: 6
                    color: "#23241d"
                    border.color: root.borderColor
                    border.width: 1
                    opacity: viewportModel.fsr2Available ? 1.0 : 0.48

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: ["Nativo", "Qualidade", "Balanceado", "Performance", "Ultra"]
                            delegate: Rectangle {
                                required property string modelData
                                required property int index
                                width: qualitySelector.width / 5
                                height: qualitySelector.height
                                radius: 6
                                color: viewportModel.fsr2Quality === index ? root.blueBg
                                                                          : (qualityHover.hovered ? "#2a2b24" : "transparent")
                                border.color: viewportModel.fsr2Quality === index ? root.blueBorder : "transparent"
                                border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: viewportModel.fsr2Quality === index ? root.blue : root.textSecondary
                                    font.family: "Segoe UI"
                                    font.pixelSize: 10
                                }
                                HoverHandler {
                                    id: qualityHover
                                    enabled: viewportModel.fsr2Available
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler {
                                    enabled: viewportModel.fsr2Available
                                    onTapped: viewportModel.SetFsr2Quality(index)
                                }
                            }
                        }
                    }
                }
                Text {
                    x: 20; y: 163
                    text: "Render interno " + viewportModel.internalResolution +
                          " · saída " + viewportModel.outputResolution
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }

                Text {
                    x: 20; y: 190
                    text: "Render scale (SSAA)"
                    color: root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 183
                    width: 54; height: 22; radius: 4
                    color: "#23241d"
                    border.color: root.borderColor
                    Text {
                        anchors.centerIn: parent
                        text: viewportModel.renderScale.toFixed(2).replace(".", ",") + "×"
                        color: root.textPrimary
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                }
                Slider {
                    id: renderScaleSlider
                    x: 20; y: 207
                    width: parent.width - 60
                    height: 18
                    from: 0.5
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
                        height: 4
                        radius: 2
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
                    x: 20; y: 229
                    text: "0,5×"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }
                Text {
                    anchors.horizontalCenter: renderScaleSlider.horizontalCenter
                    y: 229
                    text: "1,0×"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }
                Text {
                    anchors.right: renderScaleSlider.right
                    y: 229
                    text: "2,0×"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }

                Text {
                    x: 20; y: 251
                    text: "TAA"
                    color: viewportModel.fsr2Enabled ? root.textMuted : root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 70
                    y: 251
                    text: viewportModel.fsr2Enabled ? "desligado — FSR 2 controla a resolução" : ""
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 246
                    checked: viewportModel.taaEnabled && !viewportModel.fsr2Enabled
                    interactive: !viewportModel.fsr2Enabled
                    onToggled: viewportModel.SetTAAEnabled(!checked)
                }
            }

            Card {
                x: 24; y: 368
                width: renderingPage.leftW
                height: 146
                title: "Cena e geometria"

                Text {
                    x: 20; y: 56
                    text: "Frustum culling"
                    color: root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 56
                    text: "descarta o que está fora da câmera"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 50
                    checked: viewportModel.frustumCullingEnabled
                    onToggled: viewportModel.SetFrustumCullingEnabled(!checked)
                }

                Text {
                    x: 20; y: 88
                    text: "Depth prepass"
                    color: root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 88
                    text: "pré-passe de profundidade"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 82
                    checked: viewportModel.depthPrepassEnabled
                    onToggled: viewportModel.SetDepthPrepassEnabled(!checked)
                }

                Text {
                    x: 20; y: 120
                    text: "Mesclar por material"
                    color: root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 120
                    text: "agrupa draws por material"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Toggle {
                    anchors.right: parent.right; anchors.rightMargin: 20; y: 114
                    checked: viewportModel.mergeByMaterialEnabled
                    onToggled: viewportModel.SetMergeByMaterialEnabled(!checked)
                }
            }

            Card {
                x: 24 + renderingPage.leftW + renderingPage.gap
                y: 84
                width: renderingPage.rightW
                height: 170
                title: "Desempenho"

                Text {
                    x: 16; y: 54
                    text: Math.round(viewportModel.fps)
                    color: root.blue
                    font.family: "Segoe UI"
                    font.pixelSize: 28
                    font.weight: Font.Medium
                }
                Text {
                    x: 66; y: 70
                    text: "fps"
                    color: root.textSecondary
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 16; y: 90
                    text: viewportModel.frameTimeMs.toFixed(1).replace(".", ",") + " ms por frame"
                    color: root.textSecondary
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Rectangle { x: 0; y: 112; width: parent.width; height: 1; color: root.divider }
                StatusRow {
                    x: 16; y: 119; width: parent.width - 32
                    label: "Draws visíveis"
                    value: viewportModel.visibleDrawCount + " / " + viewportModel.totalDrawCount
                }
                StatusRow {
                    x: 16; y: 141; width: parent.width - 32
                    label: "Res. interna"
                    value: viewportModel.internalResolution
                }
            }

            Card {
                x: 24 + renderingPage.leftW + renderingPage.gap
                y: 266
                width: renderingPage.rightW
                height: 210
                title: "Ray tracing"

                Column {
                    x: 16; y: 46
                    width: parent.width - 32
                    RayStatusRow { width: parent.width; label: "DDGI"; active: viewportModel.ddgiEnabled }
                    RayStatusRow { width: parent.width; label: "ReSTIR GI"; active: viewportModel.restirGIEnabled }
                    RayStatusRow {
                        width: parent.width
                        label: "Visibility ray"
                        active: viewportModel.restirGIEnabled && viewportModel.restirGIVisibilityEnabled
                    }
                    RayStatusRow { width: parent.width; label: "NRD REBLUR"; active: viewportModel.nrdEnabled }
                    RayStatusRow { width: parent.width; label: "Reflexos RT"; active: viewportModel.reflectionsEnabled }
                }
                Rectangle { x: 0; y: 168; width: parent.width; height: 1; color: root.divider }
                StatusRow {
                    x: 16; y: 178; width: parent.width - 32
                    label: "VRAM dedicada"
                    value: viewportModel.vramText
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
                height: 312
                title: "Sombras do sol (CSM)"

                Text {
                    x: 20; y: 55
                    text: "Sombras dinâmicas"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "4 cascatas · 2048² · PCF Poisson 16 taps"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 158; y: 272
                    text: "tinge a cena pela cascata usada"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raio de verdade: janela, fresta, copa — via height fog"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 409
                    text: "integra o ruído do raymarch ao longo dos frames"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                height: 420
                title: "Cascatas — custo e bias"

                Text {
                    x: 20; y: 55
                    text: "Cache round-robin"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "cascatas 2/3 re-renderizam a cada 2/4 frames"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raymarch acoplado à atmosfera"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 335
                    text: "shadow map 512² projetado da camada"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "raymarch em ½ res + upsample bilinear"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 116
                    text: "acumula frames; integra o ruído do jitter"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "knob mestre do wetness deferred + streaks na frente da câmera"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 216
                    text: "só molha o que vê o céu — interior seco"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 246
                    text: "quads GPU no near-field; off = só cortina"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                }
                Text {
                    x: 190; y: 276
                    text: "nublado, key light e fog seguem a chuva"
                    color: root.textMuted
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                Text {
                    x: 20; y: 74
                    text: "acúmulo em ~5 s de chuva, seca em ~30 s depois que para"
                    color: root.textMuted
                    font.family: "Segoe UI"
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

        Item {
            visible: root.selectedPage !== 0 && root.selectedPage !== 6 && root.selectedPage !== 7 &&
                     root.selectedPage !== 8
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
                    font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
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
            font.family: "Segoe UI"
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
                font.family: "Segoe UI"
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
                font.family: "Segoe UI"
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            HoverHandler { id: applyHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: settingsWindow.close() }
        }
    }
}
