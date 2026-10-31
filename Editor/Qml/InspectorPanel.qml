pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C
import "Scene" as Scene

// Inspector — dock DIREITO do conceito Cartao 18%. O conteudo se adapta ao TIPO da
// selecao (mesh, luz, componente de ambiente); a hierarquia fica no SceneOutlinerPanel.
//
// Sem cards: as secoes sao faixas de borda a borda (SectionHeader) e as propriedades seguem
// duas colunas — rotulo (Theme.labelRatio da largura) e valor —, entao campos de secoes diferentes
// comecam na mesma vertical.
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

    function fmtCount(n) { return n.toLocaleString(Qt.locale("pt-BR"), 'f', 0) }

    // "1.24M TRI" — a contagem exata mora na secao de estatisticas; aqui e so a ordem de
    // grandeza que cabe na linha do subtitulo.
    function shortTris(n) {
        if (n >= 1000000) return (n / 1000000).toFixed(2).replace(".", ",") + " M tri"
        return fmtCount(n) + " tri"
    }

    // ---- Blocos compartilhados ----
    // Os quatro vivem em components/ porque o LightInspector usa os mesmos; o alias local
    // mantem os pontos de uso curtos.
    component SectionHeader: C.SectionHeader {}
    component VectorRow: C.VectorRow {}
    component StatRow: C.StatRow {}
    component SwitchRow: C.SwitchRow {}

    // Fio escuro da divisa com o viewport (espelho do outliner, do outro lado).
    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: C.Theme.hairlineStrong
        z: 1
    }

    C.DockPanelHeader {
        id: header
        anchors.left: parent.left; anchors.right: parent.right
        title: "Detalhes"
    }

    // ---- Identidade da selecao ----
    // Responde "o que esta selecionado?" antes de qualquer propriedade: icone na cor do TIPO
    // (azul mesh, ambar luz, verde-agua ambiente), nome e uma linha de meta.
    Item {
        id: identity
        anchors.top: header.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 12; anchors.rightMargin: 12
        visible: root.hasAny
        height: 64

        Rectangle {
            id: identityIcon
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            width: 38; height: 38
            radius: C.Theme.radiusCard
            color: C.Theme.surface
            LucideIcon {
                anchors.centerIn: parent
                size: 20
                color: root.hasMesh ? C.Theme.blue : root.hasLight ? C.Theme.amber : C.Theme.teal
                name: root.hasMesh ? "box"
                    : root.hasLight ? (root.lightsModel.lightType === 1 ? "cone" : "lightbulb")
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
            y: 13
            text: root.hasMesh ? root.outlinerModel.meshName
                : root.hasLight ? root.lightsModel.name
                : root.hasClouds ? "Nuvens Volumétricas"
                : root.hasWeather ? "Clima"
                : "Oceano FFT"
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textTitle
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
            y: 34
            text: root.hasMesh
                    ? (root.outlinerModel.meshDynamic ? "Mesh dinâmica  ·  " : "Mesh estática  ·  ")
                      + root.shortTris(root.outlinerModel.meshTris)
                : root.hasLight
                    ? (root.lightsModel.lightType === 1 ? "Luz spot" : "Luz point")
                : "Ambiente  ·  tempo real"
            color: C.Theme.textMuted
            font.family: C.Theme.fontNum
            font.pixelSize: C.Theme.textMeta + 1
            elide: Text.ElideRight
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        // Liga/desliga a coisa selecionada, quando ela tem esse estado.
        C.Toggle {
            id: identityToggle
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            visible: root.hasLight || root.hasClouds
            checked: root.hasLight ? root.lightsModel.lightEnabled
                   : root.hasClouds ? root.cloudsModel.enabled
                   : false
            onToggled: {
                if (root.hasLight) root.lightsModel.lightEnabled = !checked
                else if (root.hasClouds) root.cloudsModel.SetEnabled(!root.cloudsModel.enabled)
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
        font.pixelSize: C.Theme.textBody
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
        visible: root.hasAny
        contentHeight: content.implicitHeight + 16
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        // Margem lateral na COLUNA, nao no Flickable: as faixas de secao vazam 14 px para
        // cada lado e o clip do Flickable cortaria o vazamento.
        Column {
            id: content
            x: 14
            width: body.width - 28
            spacing: 0

            // ======== MESH ========
            SectionHeader {
                visible: root.hasMesh
                firstInColumn: true
                label: "Transform"
            }
            VectorRow {
                visible: root.hasMesh
                label: "Posição"
                decimals: 3
                xValue: root.outlinerModel.meshPosX
                yValue: root.outlinerModel.meshPosY
                zValue: root.outlinerModel.meshPosZ
                onXCommitted: (v) => root.outlinerModel.meshPosX = v
                onYCommitted: (v) => root.outlinerModel.meshPosY = v
                onZCommitted: (v) => root.outlinerModel.meshPosZ = v
            }
            VectorRow {
                visible: root.hasMesh
                label: "Rotação"
                decimals: 1
                xValue: root.outlinerModel.meshRotX
                yValue: root.outlinerModel.meshRotY
                zValue: root.outlinerModel.meshRotZ
                onXCommitted: (v) => root.outlinerModel.meshRotX = v
                onYCommitted: (v) => root.outlinerModel.meshRotY = v
                onZCommitted: (v) => root.outlinerModel.meshRotZ = v
            }
            VectorRow {
                visible: root.hasMesh
                label: "Escala"
                decimals: 2
                tinted: false        // escala nao tem convencao de cor por eixo
                showLink: true
                linked: root.outlinerModel.meshScaleLocked
                xValue: root.outlinerModel.meshScaleX
                yValue: root.outlinerModel.meshScaleY
                zValue: root.outlinerModel.meshScaleZ
                onLinkToggled: root.outlinerModel.meshScaleLocked = !root.outlinerModel.meshScaleLocked
                onXCommitted: (v) => root.outlinerModel.meshScaleX = v
                onYCommitted: (v) => root.outlinerModel.meshScaleY = v
                onZCommitted: (v) => root.outlinerModel.meshScaleZ = v
            }

            SectionHeader {
                visible: root.hasMesh && root.outlinerModel.meshMaterial !== ""
                label: "Material"
            }
            Item {
                visible: root.hasMesh && root.outlinerModel.meshMaterial !== ""
                width: parent.width
                height: 26
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.outlinerModel.meshMaterial
                    color: C.Theme.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: C.Theme.textBody
                    font.weight: Font.Medium
                    elide: Text.ElideMiddle
                }
            }
            // Flags do material (folhagem/two-sided/masked/translucido) em chips.
            Flow {
                visible: root.hasMesh && root.outlinerModel.meshFlags.length > 0
                width: parent.width
                topPadding: 4
                bottomPadding: 4
                spacing: 5
                Repeater {
                     model: root.hasMesh ? root.outlinerModel.meshFlags : []
                     delegate: Rectangle {
                         id: flagChip
                         required property string modelData
                        width: flagText.implicitWidth + 14
                        height: 20
                        radius: C.Theme.radiusChip
                        color: C.Theme.surfaceRaised
                        Text {
                            id: flagText
                            anchors.centerIn: parent
                             text: flagChip.modelData
                            color: C.Theme.textSecondary
                            font.family: C.Theme.fontFamily
                            font.pixelSize: C.Theme.textMeta
                        }
                    }
                }
            }

            SectionHeader { visible: root.hasMesh; label: "Renderização" }
            // Mobilidade: estatico deixa o CSM reaproveitar o shadow map entre frames. O hint
            // diz o que a marcacao CUSTA, nao o que ela e — quem abre este painel ja sabe o que
            // "dinamico" significa, e o que nao e obvio e que marcar tudo como dinamico devolve
            // a engine ao custo de antes do cache.
            SwitchRow {
                visible: root.hasMesh
                label: "Dinâmico"
                hint: root.outlinerModel.meshDynamic ? "sombra redesenhada" : "sombra cacheada"
                checked: root.outlinerModel.meshDynamic
                onToggled: root.outlinerModel.meshDynamic = !root.outlinerModel.meshDynamic
            }
            SwitchRow {
                visible: root.hasMesh
                label: "Visível"
                checked: root.outlinerModel.meshVisible
                onToggled: {
                    var r = root.outlinerModel.selectedRowIndex()
                    if (r >= 0) root.outlinerModel.toggleEye(r)
                }
            }

            SectionHeader { visible: root.hasMesh; label: "Estatísticas" }
            StatRow {
                visible: root.hasMesh
                label: "Triângulos"; value: root.fmtCount(root.outlinerModel.meshTris)
            }
            StatRow {
                visible: root.hasMesh
                label: "Vértices"; value: root.fmtCount(root.outlinerModel.meshVerts)
            }
            StatRow {
                visible: root.hasMesh
                label: "VRAM geometria"; value: root.outlinerModel.meshVramText
            }

            Text {
                visible: root.hasMesh
                width: parent.width
                topPadding: 12
                text: "Duplo-clique na árvore (ou F) enquadra a câmera. Ctrl+D duplica, Del remove."
                color: C.Theme.textMuted
                font.family: C.Theme.fontFamily
                font.pixelSize: C.Theme.textMeta
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
            // Os tres componentes compartilham preset, linhas de valor e secoes expansivas;
            // a identidade ja foi respondida pelo card acima e nao se repete aqui.
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
