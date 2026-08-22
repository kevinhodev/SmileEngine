pragma ComponentBehavior: Bound

import QtQuick
import "components" as C

// Barra superior unificada (titulo + janela) 100% QML, no conceito Signal Graphite:
// marca e menus a esquerda, breadcrumb da cena no centro, telemetria e controles de janela
// a direita. Chrome fino (44 px) fechado por uma hairline de 1 px, sem gradiente.
//
// Dependencias explicitas da raiz (chegam por setInitialProperties; sem o `required property`
// aqui a propriedade fica undefined e o handler morre em silencio):
//  - `windowBridge` (WindowBridge):  minimizar/maximizar/fechar e arrasto nativo.
//  - `menuBridge`   (MenuBridge):    acoes dos menus.
//  - `sceneDoc`     (SceneDocument): nome da cena e dirty state do breadcrumb.
//  - `mcpModel`     (McpBridge):     estado do canal do SmileMCP.
//  - `statusModel`  (StatusBridge):  mensagem transiente do editor.
// `image://smilelogo/<px>` e o logo.
//
// A barra reporta ao bridge a sua altura e os retangulos interativos (menus e botoes) -> o
// NativeWindowFilter usa isso pra decidir HTCAPTION (arrasto) x HTCLIENT (controles).
Rectangle {
    id: bar
    required property var windowBridge
    required property var menuBridge
    required property var sceneDoc
    required property var mcpModel
    required property var statusModel

    color: C.Theme.shell
    implicitWidth: 800
    implicitHeight: 44

    // Hairline inferior: no conceito ela e elemento de identidade, nao so separador.
    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: C.Theme.hairline
    }

    // ---- Chip de dado (mono, superficie + hairline) ----
    component DataChip: Rectangle {
        id: dataChip
        property color dotColor: "transparent"
        property alias text: dataChipLabel.text
        property color textColor: C.Theme.textSecondary
        property int maxTextWidth: 100000
        implicitWidth: dataChipRow.implicitWidth + 24
        implicitHeight: 22
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1
        Row {
            id: dataChipRow
            anchors.centerIn: parent
            spacing: 8
            Rectangle {
                visible: dataChip.dotColor.a > 0
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: dataChip.dotColor
            }
            Text {
                id: dataChipLabel
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, dataChip.maxTextWidth)
                color: dataChip.textColor
                font.family: C.Theme.fontMono
                font.pixelSize: 10
                elide: Text.ElideRight
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
        }
    }

    // ---- Botao de janela (estilo Windows: cheio, edge-to-edge) ----
    component WinButton: Item {
        id: wb
        property string icon
        property color hoverColor: C.Theme.surfaceRaised
        signal clicked()
        implicitWidth: 46; implicitHeight: bar.height - 1
        Rectangle { anchors.fill: parent; color: wbHover.hovered ? wb.hoverColor : "transparent" }
        LucideIcon { anchors.centerIn: parent; name: wb.icon; size: 15
                     color: wbHover.hovered ? C.Theme.textPrimary : C.Theme.textMuted }
        HoverHandler { id: wbHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: wb.clicked() }
    }

    // ---- Marca (esquerda) ----
    Row {
        id: brand
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 9
        Image {
            anchors.verticalCenter: parent.verticalCenter
            source: "image://smilelogo/48"
            sourceSize: Qt.size(48, 48) // 2x p/ nitidez
            height: 24; width: 24
            fillMode: Image.PreserveAspectFit
            smooth: true; mipmap: true
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Smile"
            color: C.Theme.textPrimary
            font.family: C.Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 1
            text: "EDITOR"
            color: C.Theme.textMuted
            font.family: C.Theme.fontMono
            font.pixelSize: 9
            font.letterSpacing: 1.2
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }

    // ---- Menus ----
    EditorMenuBar {
        id: menuBar
        menuBridge: bar.menuBridge
        anchors { left: brand.right; leftMargin: 18; verticalCenter: parent.verticalCenter }
    }

    // ---- Area de arrasto (caption) ----
    // Cobre a barra a esquerda dos botoes. startSystemMove (no 1o movimento) move a janela e
    // habilita Aero Snap; double-click maximiza/restaura. E o que substitui o HTCAPTION, que
    // nao funciona aqui porque a barra e uma janela filha nativa.
    MouseArea {
        id: caption
        anchors { left: menuBar.right; right: winButtons.left; top: parent.top; bottom: parent.bottom }
        property bool moving: false
        onPressed: moving = false
        onPositionChanged: {
            if (pressed && !moving) { moving = true; bar.windowBridge.startSystemMove() }
        }
        onReleased: moving = false
        onDoubleClicked: bar.windowBridge.toggleMaximize()
    }

    // ---- Breadcrumb da cena (centro) ----
    // O ponto accent e o dirty state: mesma linguagem do ponto no botao de salvar do outliner.
    Rectangle {
        id: breadcrumb
        readonly property real availableWidth: Math.max(
            0, rightChips.x - (menuBar.x + menuBar.width) - 24)
        readonly property real naturalWidth: 6 + folderLabel.implicitWidth
            + sceneLabel.implicitWidth + (dirtyLabel.visible ? dirtyLabel.implicitWidth + 8 : 0)
            + 24 + 16

        x: Math.max(
            menuBar.x + menuBar.width + 12,
            Math.min((bar.width - width) / 2, rightChips.x - width - 12))
        anchors.verticalCenter: parent.verticalCenter
        visible: bar.sceneDoc.available && availableWidth >= 120
        width: Math.min(naturalWidth, bar.width * 0.34, availableWidth)
        height: 26
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1
        clip: true

        Row {
            id: breadcrumbRow
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            readonly property real textWidth: Math.max(
                0, width - (dirtyLabel.visible ? dirtyLabel.width : 0)
                   - 6 - spacing * (dirtyLabel.visible ? 3 : 2))

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: bar.sceneDoc.dirty ? C.Theme.accent : C.Theme.textMuted
            }
            Text {
                id: folderLabel
                anchors.verticalCenter: parent.verticalCenter
                width: breadcrumbRow.textWidth * 0.45
                text: bar.sceneDoc.sceneFolder + " /"
                color: C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 11
                elide: Text.ElideMiddle
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
            Text {
                id: sceneLabel
                anchors.verticalCenter: parent.verticalCenter
                width: breadcrumbRow.textWidth * 0.55
                text: bar.sceneDoc.sceneName
                color: C.Theme.textPrimary
                font.family: C.Theme.fontMono
                font.pixelSize: 11
                elide: Text.ElideMiddle
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
            Text {
                id: dirtyLabel
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.sceneDoc.dirty
                text: "modificado"
                color: C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 10
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
        }
    }

    // ---- Telemetria (direita, antes dos controles de janela) ----
    Row {
        id: rightChips
        anchors { right: winButtons.left; rightMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 8

        DataChip {
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.statusModel.message !== ""
            dotColor: C.Theme.amber
            text: bar.statusModel.message
            textColor: C.Theme.textSecondary
            maxTextWidth: 220
        }

        DataChip {
            anchors.verticalCenter: parent.verticalCenter
            // Verde = agente conectado, ambar = ouvindo sem cliente, muted = pipe nao subiu.
            // Papel semantico fixo: o accent nunca entra aqui, ele e so selecao/ativo/dirty.
            dotColor: !bar.mcpModel.listening ? C.Theme.textMuted
                    : bar.mcpModel.clientCount > 0 ? C.Theme.green
                    : C.Theme.amber
            text: "MCP"
        }
    }

    Row {
        id: winButtons
        anchors { right: parent.right; top: parent.top }
        WinButton { icon: "minus"; onClicked: bar.windowBridge.minimize() }
        WinButton {
            icon: bar.windowBridge.maximized ? "restore" : "square"
            onClicked: bar.windowBridge.toggleMaximize()
        }
        WinButton {
            icon: "x"; hoverColor: "#C42B1C"
            onClicked: bar.windowBridge.close()
        }
    }

    // ---- Reporta geometria pro hit-test nativo ----
    function reportRects() {
        // Coords da janela (a barra fica no topo, em y=0). Menus e botoes = HTCLIENT; o resto
        // da faixa = HTCAPTION (arrasto). Coords logicas; o filtro converte p/ fisico via DPI.
        windowBridge.setInteractiveRects([
            Qt.rect(menuBar.x,    menuBar.y,    menuBar.width,    menuBar.height),
            Qt.rect(winButtons.x, winButtons.y, winButtons.width, winButtons.height)
        ])
    }
    onWidthChanged: reportRects()
    onHeightChanged: windowBridge.setTitleBarHeight(height)
    Component.onCompleted: {
        windowBridge.setTitleBarHeight(height)
        Qt.callLater(reportRects) // espera o layout do menuBar p/ pegar width/x corretos
    }
    // A marca fica ANTES dos menus, entao a largura dela empurra o x do menuBar: os dois
    // precisam re-reportar, senao a faixa de arrasto cobre os menus ate o proximo resize.
    Connections { target: menuBar; function onWidthChanged() { bar.reportRects() } }
    Connections { target: brand;   function onWidthChanged() { Qt.callLater(bar.reportRects) } }
}
