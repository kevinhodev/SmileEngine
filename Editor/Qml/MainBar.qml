import QtQuick
import QtQuick.Controls

// Barra unificada (titulo + janela) 100% QML. Menus virao na esquerda (Fase 3); por ora:
// logo da engine centralizado + botoes de janela (min/max/fechar) a direita.
// `windowBridge` (WindowBridge) vem por context property; `image://smilelogo/<px>` e o logo.
// A barra reporta ao bridge a sua altura e os retangulos interativos (botoes) -> o
// NativeWindowFilter usa isso pra decidir HTCAPTION (arrasto) x HTCLIENT (controles).
Rectangle {
    id: bar
    color: "#141511"
    implicitWidth: 800
    implicitHeight: 36

    // ---- Botao de janela (estilo Windows: cheio, edge-to-edge) ----
    component WinButton: Item {
        id: wb
        property string icon
        property color hoverColor: "#23241d"
        property color iconColor: "#c8c2b4"
        signal clicked()
        implicitWidth: 46; implicitHeight: bar.height
        Rectangle { anchors.fill: parent; color: wbHover.hovered ? wb.hoverColor : "transparent" }
        LucideIcon { anchors.centerIn: parent; name: wb.icon; size: 15
                     color: wbHover.hovered ? "#ffffff" : wb.iconColor }
        HoverHandler { id: wbHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: wb.clicked() }
    }

    // ---- Menus (esquerda) ----
    EditorMenuBar {
        id: menuBar
        anchors { left: parent.left; leftMargin: 6; verticalCenter: parent.verticalCenter }
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

    // ---- Logo central ----
    Image {
        anchors.centerIn: parent
        source: "image://smilelogo/40"
        sourceSize: Qt.size(height * 2, height * 2) // 2x p/ nitidez
        height: 22; width: 22
        fillMode: Image.PreserveAspectFit
        smooth: true; mipmap: true
    }

    // ---- Botoes de janela (direita) ----
    Row {
        id: winButtons
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        WinButton { icon: "minus"; onClicked: windowBridge.minimize() }
        WinButton {
            icon: windowBridge.maximized ? "copy" : "square"
            onClicked: windowBridge.toggleMaximize()
        }
        WinButton {
            icon: "x"; hoverColor: "#c42b1c"
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
    Connections { target: menuBar; function onWidthChanged() { reportRects() } }
}
