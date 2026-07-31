import QtQuick
import QtQuick.Controls

// Barra de menus do editor (Arquivo/Janela/Ajuda), estilo escuro. Liga em `menuBridge`
// (MenuBridge, context property). Popups usam popupType Window pra escapar do QQuickWidget fino
// da barra (senao recortariam na altura dela) e largura dinamica (cabe ao conteudo).
MenuBar {
    id: menuBar

    // ---- Item de topo (Arquivo, Janela, ...) ----
    delegate: MenuBarItem {
        id: mbi
        implicitHeight: 26
        padding: 0
        contentItem: Text {
            text: mbi.text
            color: "#c8c2b4"
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            leftPadding: 10; rightPadding: 10
        }
        background: Rectangle {
            radius: 5
            color: mbi.highlighted ? "#23241d" : "transparent"
        }
    }
    background: Rectangle { color: "transparent" }

    // ---- Item de dropdown: Item ancorado, com implicitWidth EXPLICITO no contentItem ----
    // O Menu NAO auto-dimensiona pela largura dos itens (contentWidth fica fixo), por isso o item
    // mais largo estourava e o atalho (ancorado a direita) colava no texto. A largura real fica
    // aqui (padding + check + label + gap + atalho + padding) e o DarkMenu le isso pra setar o
    // contentWidth = max dos itens.
    component MItem: MenuItem {
        id: mi
        property string shortcutText: ""
        property bool checkColumn: checkable
        property bool markVisible: checkable && checked

        readonly property int padX: 12                         // padding esquerda/direita
        readonly property int checkW: checkColumn ? 12 : 0     // coluna do check (so se checkable)
        readonly property int gap: shortcutText !== "" ? 28 : 0 // folga minima label<->atalho

        implicitHeight: 26
        padding: 0
        indicator: null   // desenho o checkmark no contentItem
        arrow: null       // sem submenu aqui

        contentItem: Item {
            implicitHeight: mi.implicitHeight
            // largura real do item; o DarkMenu usa isto pra dimensionar o popup
            implicitWidth: mi.padX + mi.checkW + label.implicitWidth
                           + mi.gap + shortcut.implicitWidth + mi.padX

            Text { // check, centrado na sua coluna [padX, padX+checkW]
                visible: mi.markVisible
                anchors.horizontalCenter: parent.left
                anchors.horizontalCenterOffset: mi.padX + mi.checkW / 2
                anchors.verticalCenter: parent.verticalCenter
                text: "✓"; color: "#5b9dff"; font.pixelSize: 12
            }
            Text {
                id: label
                anchors.left: parent.left
                anchors.leftMargin: mi.padX + mi.checkW
                anchors.verticalCenter: parent.verticalCenter
                text: mi.text
                color: mi.enabled ? "#c8c2b4" : "#5a5950"
                font.pixelSize: 12
            }
            Text {
                id: shortcut
                visible: mi.shortcutText !== ""
                anchors.right: parent.right
                anchors.rightMargin: mi.padX
                anchors.verticalCenter: parent.verticalCenter
                text: mi.shortcutText
                color: "#6c6a61"; font.pixelSize: 11
            }
        }
        background: Rectangle { color: mi.highlighted ? "#2a2b24" : "transparent" }
    }

    component MSep: MenuSeparator {
        // padding minimo (bem menor que o default): so o suficiente pra linha de 1px aparecer.
        // O grosso da folga vertical ja vem da altura dos itens vizinhos (26px).
        topPadding: 4; bottomPadding: 4; leftPadding: 0; rightPadding: 0
        contentItem: Rectangle { implicitHeight: 1; color: "#2a2b24" }
    }

    // ---- Menu escuro (popup como janela propria, largura pelo conteudo) ----
    component DarkMenu: Menu {
        id: dm
        popupType: Popup.Window
        topPadding: 6; bottomPadding: 6 // folga p/ 1o e ultimo item nao colarem na borda
        leftPadding: 0; rightPadding: 0 // padding horizontal vive dentro de cada item (padX)
        // O Menu nao ajusta a largura aos itens sozinho: calculamos contentWidth = max dos itens.
        // Reavalia quando count ou o implicitWidth de algum contentItem muda (ex.: fonte carregar).
        contentWidth: {
            var w = 0;
            for (var i = 0; i < dm.count; ++i) {
                var it = dm.itemAt(i);
                if (it && it.contentItem)
                    w = Math.max(w, it.contentItem.implicitWidth);
            }
            return w;
        }
        background: Rectangle {
            color: "#1b1c17"; border.color: "#33342c"; border.width: 1; radius: 6
        }
    }

    DarkMenu {
        title: "Arquivo"
        // Qt.callLater: deixa o popup (janela on-top) fechar ANTES do QFileDialog modal, que
        // senao bloqueia o loop dentro do handler e prende o menu aberto por cima de tudo.
        MItem { text: "Carregar Cena…";  shortcutText: "Ctrl+O";       onTriggered: Qt.callLater(menuBridge.loadScene) }
        MItem { text: "Adicionar Cena…"; shortcutText: "Ctrl+Shift+O"; onTriggered: Qt.callLater(menuBridge.addScene) }
        MSep {}
        MItem { text: "Sair";            shortcutText: "Ctrl+Q";       onTriggered: menuBridge.quit() }
    }

    DarkMenu {
        title: "Janela"
        MItem {
            text: "Console"; checkColumn: true
            markVisible: menuBridge.consoleVisible // estado real do dock (pode fechar sozinho)
            onTriggered: menuBridge.toggleConsole()
        }
        MItem {
            text: "Time of Day"; checkColumn: true
            markVisible: menuBridge.timeOfDayVisible
            onTriggered: menuBridge.toggleTimeOfDay()
        }
        MItem {
            text: "Cena"; checkColumn: true
            markVisible: menuBridge.lightsVisible
            onTriggered: menuBridge.toggleLights()
        }
        MItem {
            text: "Mini Profiler"; checkColumn: true
            markVisible: menuBridge.statsVisible
            onTriggered: menuBridge.toggleStats()
        }
        MItem {
            text: "Render targets"; checkColumn: true; shortcutText: "Ctrl+Shift+T"
            markVisible: menuBridge.debugTargetsVisible
            onTriggered: menuBridge.toggleDebugTargets()
        }
        MItem {
            text: "Materiais"; checkColumn: true
            markVisible: menuBridge.materialsVisible
            onTriggered: menuBridge.toggleMaterials()
        }
    }

    DarkMenu {
        title: "Opções"
        MItem {
            text: "Configurações…"; shortcutText: "Ctrl+,"
            onTriggered: menuBridge.openSettings()
        }
    }

    DarkMenu {
        title: "Ajuda"
        MItem { text: "Sobre o Smile Engine…"; onTriggered: menuBridge.about() }
    }
}
