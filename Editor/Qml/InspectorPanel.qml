import QtQuick
import QtQuick.Controls
import "components" as C
import "Scene" as Scene

// Inspector — dock DIREITO do conceito Signal Graphite. O conteudo se adapta ao TIPO da
// selecao (mesh, luz, componente de ambiente); a hierarquia fica no SceneOutlinerPanel.
//
// Sem cards: o conceito separa secoes por header em caixa alta + hairline de 1 px, o que
// devolve a largura que a moldura dos cards consumia num dock de 320 px. Os inspectores de
// ambiente (nuvens/clima/oceano) seguem com os proprios cards por enquanto — eles nao fazem
// parte desta leva de redesign.
//
// Dependencias explicitas da raiz (chegam por setInitialProperties):
//  - `outlinerModel` (SceneOutlinerBridge): selecao, transform, estatisticas e material da mesh.
//  - `lightsModel`   (LightsBridge):        propriedades da luz selecionada.
//  - `cloudsModel` / `weatherModel` / `oceanModel`: componentes de ambiente.
Rectangle {
    id: root
    required property var outlinerModel
    required property var lightsModel
    required property var cloudsModel
    required property var weatherModel
    required property var oceanModel

    color: C.Theme.shell

    readonly property bool hasLight: lightsModel.selectedIndex >= 0
    readonly property bool hasMesh: outlinerModel.meshSelected
    readonly property bool hasClouds: outlinerModel.selectedEnvironment === 1
    readonly property bool hasWeather: outlinerModel.selectedEnvironment === 4
    readonly property bool hasOcean: outlinerModel.selectedEnvironment === 2
    readonly property bool hasEnv: hasClouds || hasWeather || hasOcean
    readonly property bool hasAny: hasLight || hasMesh || hasEnv

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }
    function fmtCount(n) { return n.toLocaleString(Qt.locale("pt-BR"), 'f', 0) }

    // "1.24M TRI" — a contagem exata mora na secao de estatisticas; aqui e so a ordem de
    // grandeza que cabe na linha do subtitulo.
    function shortTris(n) {
        if (n >= 1000000) return (n / 1000000).toFixed(2) + "M TRI"
        if (n >= 1000)    return (n / 1000).toFixed(0) + "K TRI"
        return n + " TRI"
    }

    // ---- Blocos compartilhados ----
    // Os quatro vivem em components/ porque o LightInspector usa os mesmos; o alias local
    // mantem os pontos de uso curtos.
    component SectionHeader: C.SectionHeader {}
    component VectorRow: C.VectorRow {}
    component StatRow: C.StatRow {}
    component SwitchRow: C.SwitchRow {}

    // Hairline da divisa com o viewport (espelho da do outliner, do outro lado).
    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: C.Theme.hairline
        z: 1
    }

    C.DockPanelHeader {
        id: header
        anchors.left: parent.left; anchors.right: parent.right
        title: "INSPECTOR"
    }

    // ---- Card de identidade da selecao ----
    // Unico card do painel: e ele que responde "o que esta selecionado?" antes de qualquer
    // propriedade, e a moldura e o que o separa do resto do conteudo.
    Rectangle {
        id: identity
        anchors.top: header.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 14; anchors.rightMargin: 14
        anchors.topMargin: 14
        visible: root.hasAny
        height: 54
        radius: C.Theme.radiusCard
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1

        Rectangle {
            id: identityIcon
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 30; height: 30
            radius: C.Theme.radiusControl
            color: C.Theme.accentBg
            border.color: C.Theme.accentBorder
            border.width: 1
            LucideIcon {
                anchors.centerIn: parent
                size: 16
                color: C.Theme.accent
                name: root.hasMesh ? "box"
                    : root.hasLight ? (lightsModel.lightType === 1 ? "cone" : "lightbulb")
                    : root.hasClouds ? "cloud"
                    : root.hasWeather ? "cloud-rain"
                    : "waves"
            }
        }
        Text {
            id: identityName
            anchors.left: identityIcon.right
            anchors.leftMargin: 12
            anchors.right: identityToggle.left
            anchors.rightMargin: 8
            y: 12
            text: root.hasMesh ? outlinerModel.meshName
                : root.hasLight ? lightsModel.name
                : root.hasClouds ? "Nuvens Volumétricas"
                : root.hasWeather ? "Clima"
                : "Oceano FFT"
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideMiddle
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Text {
            anchors.left: identityIcon.right
            anchors.leftMargin: 12
            anchors.right: identityToggle.left
            anchors.rightMargin: 8
            y: 32
            text: root.hasMesh
                    ? (outlinerModel.meshDynamic ? "MESH DINÂMICA · " : "STATIC MESH · ")
                      + root.shortTris(outlinerModel.meshTris)
                : root.hasLight
                    ? (lightsModel.lightType === 1 ? "LUZ SPOT" : "LUZ POINT")
                : "AMBIENTE · TEMPO REAL"
            color: C.Theme.textMuted
            font.family: C.Theme.fontMono
            font.pixelSize: 10
            font.letterSpacing: 0.6
            elide: Text.ElideRight
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        // Liga/desliga a coisa selecionada, quando ela tem esse estado.
        C.Toggle {
            id: identityToggle
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: root.hasLight || root.hasClouds
            checked: root.hasLight ? lightsModel.lightEnabled
                   : root.hasClouds ? cloudsModel.enabled
                   : false
            onToggled: {
                if (root.hasLight) lightsModel.lightEnabled = !checked
                else if (root.hasClouds) cloudsModel.SetEnabled(!cloudsModel.enabled)
            }
        }
    }

    // ---- Estado vazio ----
    Text {
        anchors.centerIn: parent
        width: parent.width - 60
        visible: !root.hasAny
        text: "Selecione um objeto na árvore da cena ou clique nele no viewport."
        color: C.Theme.textMuted
        font.family: C.Theme.fontFamily
        font.pixelSize: 11
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        lineHeight: 1.3
    }

    // ---- Conteudo por tipo de selecao ----
    Flickable {
        id: body
        anchors.top: identity.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 14; anchors.rightMargin: 14
        anchors.topMargin: 6
        visible: root.hasAny
        contentHeight: content.implicitHeight + 16
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: content
            width: parent.width
            spacing: 0

            // ======== MESH ========
            SectionHeader {
                visible: root.hasMesh
                firstInColumn: true
                label: "TRANSFORM"
            }
            VectorRow {
                visible: root.hasMesh
                label: "Posição"
                decimals: 3
                xValue: outlinerModel.meshPosX
                yValue: outlinerModel.meshPosY
                zValue: outlinerModel.meshPosZ
                onXCommitted: (v) => outlinerModel.meshPosX = v
                onYCommitted: (v) => outlinerModel.meshPosY = v
                onZCommitted: (v) => outlinerModel.meshPosZ = v
            }
            VectorRow {
                visible: root.hasMesh
                label: "Rotação"
                decimals: 1
                xValue: outlinerModel.meshRotX
                yValue: outlinerModel.meshRotY
                zValue: outlinerModel.meshRotZ
                onXCommitted: (v) => outlinerModel.meshRotX = v
                onYCommitted: (v) => outlinerModel.meshRotY = v
                onZCommitted: (v) => outlinerModel.meshRotZ = v
            }
            VectorRow {
                visible: root.hasMesh
                label: "Escala"
                decimals: 2
                tinted: false        // escala nao tem convencao de cor por eixo
                showLink: true
                linked: outlinerModel.meshScaleLocked
                xValue: outlinerModel.meshScaleX
                yValue: outlinerModel.meshScaleY
                zValue: outlinerModel.meshScaleZ
                onLinkToggled: outlinerModel.meshScaleLocked = !outlinerModel.meshScaleLocked
                onXCommitted: (v) => outlinerModel.meshScaleX = v
                onYCommitted: (v) => outlinerModel.meshScaleY = v
                onZCommitted: (v) => outlinerModel.meshScaleZ = v
            }

            SectionHeader {
                visible: root.hasMesh && outlinerModel.meshMaterial !== ""
                label: "MATERIAL"
            }
            Item {
                visible: root.hasMesh && outlinerModel.meshMaterial !== ""
                width: parent.width
                height: 26
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: outlinerModel.meshMaterial
                    color: C.Theme.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                }
            }
            // Flags do material (folhagem/two-sided/masked/translucido) em chips mono.
            Flow {
                visible: root.hasMesh && outlinerModel.meshFlags.length > 0
                width: parent.width
                topPadding: 4
                bottomPadding: 4
                spacing: 5
                Repeater {
                    model: root.hasMesh ? outlinerModel.meshFlags : []
                    delegate: Rectangle {
                        required property string modelData
                        width: flagText.implicitWidth + 14
                        height: 18
                        radius: C.Theme.radiusChip
                        color: C.Theme.surface
                        border.color: C.Theme.hairline
                        border.width: 1
                        Text {
                            id: flagText
                            anchors.centerIn: parent
                            text: modelData
                            color: C.Theme.textSecondary
                            font.family: C.Theme.fontMono
                            font.pixelSize: 9
                        }
                    }
                }
            }

            SectionHeader { visible: root.hasMesh; label: "RENDERIZAÇÃO" }
            // Mobilidade: estatico deixa o CSM reaproveitar o shadow map entre frames. O hint
            // diz o que a marcacao CUSTA, nao o que ela e — quem abre este painel ja sabe o que
            // "dinamico" significa, e o que nao e obvio e que marcar tudo como dinamico devolve
            // a engine ao custo de antes do cache.
            SwitchRow {
                visible: root.hasMesh
                label: "Dinâmico"
                hint: outlinerModel.meshDynamic ? "sombra redesenhada" : "sombra cacheada"
                checked: outlinerModel.meshDynamic
                onToggled: outlinerModel.meshDynamic = !outlinerModel.meshDynamic
            }
            SwitchRow {
                visible: root.hasMesh
                label: "Visível"
                checked: outlinerModel.meshVisible
                onToggled: {
                    var r = outlinerModel.selectedRowIndex()
                    if (r >= 0) outlinerModel.toggleEye(r)
                }
            }

            SectionHeader { visible: root.hasMesh; label: "ESTATÍSTICAS" }
            StatRow {
                visible: root.hasMesh
                label: "Triângulos"; value: root.fmtCount(outlinerModel.meshTris)
            }
            StatRow {
                visible: root.hasMesh
                label: "Vértices"; value: root.fmtCount(outlinerModel.meshVerts)
            }
            StatRow {
                visible: root.hasMesh
                label: "VRAM geometria"; value: outlinerModel.meshVramText
            }

            Text {
                visible: root.hasMesh
                width: parent.width
                topPadding: 12
                text: "Duplo-clique na árvore (ou F) enquadra a câmera. Ctrl+D duplica, Del remove."
                color: C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }

            // ======== LUZ ========
            Scene.LightInspector {
                visible: root.hasLight
                width: parent.width
                lightsModel: root.lightsModel
            }

            // ======== AMBIENTE ========
            // Os tres inspectores de ambiente ainda usam a linguagem de cards da leva
            // anterior; entram aqui inteiros e serao redesenhados na sua propria leva.
            Item {
                visible: root.hasEnv
                width: parent.width
                height: envColumn.implicitHeight + 12
                Column {
                    id: envColumn
                    y: 8
                    width: parent.width
                    spacing: 10
                    Scene.CloudsInspector {
                        width: parent.width
                        visible: root.hasClouds
                        model: root.cloudsModel
                    }
                    Scene.WeatherInspector {
                        width: parent.width
                        visible: root.hasWeather
                        model: root.weatherModel
                    }
                    Scene.OceanInspector {
                        width: parent.width
                        visible: root.hasOcean
                        model: root.oceanModel
                    }
                }
            }
        }
    }
}
