import QtQuick
import QtQuick.Controls
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
//  - `statusModel`  (StatusBridge):  FPS do chip de telemetria.
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
                color: dataChip.textColor
                font.family: C.Theme.fontMono
                font.pixelSize: 10
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
            if (pressed && !moving) { moving = true; windowBridge.startSystemMove() }
        }
        onReleased: moving = false
        onDoubleClicked: windowBridge.toggleMaximize()
    }

    // ---- Breadcrumb da cena (centro) ----
    // O ponto accent e o dirty state: mesma linguagem do ponto no botao de salvar do outliner.
    Rectangle {
        id: breadcrumb
        anchors.centerIn: parent
        visible: sceneDoc.available
        width: Math.min(breadcrumbRow.implicitWidth + 32, bar.width * 0.34)
        height: 26
        radius: C.Theme.radiusControl
        color: C.Theme.surface
        border.color: C.Theme.hairline
        border.width: 1

        Row {
            id: breadcrumbRow
            anchors.centerIn: parent
            spacing: 8

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: sceneDoc.dirty ? C.Theme.accent : C.Theme.textMuted
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: sceneDoc.sceneFolder + " /"
                color: C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 11
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: sceneDoc.sceneName
                color: C.Theme.textPrimary
                font.family: C.Theme.fontMono
                font.pixelSize: 11
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: sceneDoc.dirty
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
            // Verde = agente conectado, ambar = ouvindo sem cliente, muted = pipe nao subiu.
            // Papel semantico fixo: o accent nunca entra aqui, ele e so selecao/ativo/dirty.
            dotColor: !mcpModel.listening ? C.Theme.textMuted
                    : mcpModel.clientCount > 0 ? C.Theme.green
                    : C.Theme.amber
            text: "MCP"
        }
        DataChip {
            anchors.verticalCenter: parent.verticalCenter
            text: Math.round(statusModel.fps) + " FPS"
        }
    }

    Row {
        id: winButtons
        anchors { right: parent.right; top: parent.top }
        WinButton { icon: "minus"; onClicked: windowBridge.minimize() }
        WinButton {
            icon: windowBridge.maximized ? "copy" : "square"
            onClicked: windowBridge.toggleMaximize()
        }
        WinButton {
            icon: "x"; hoverColor: "#C42B1C"
            onClicked: windowBridge.close()
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
