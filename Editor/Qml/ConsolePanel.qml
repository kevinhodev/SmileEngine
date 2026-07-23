import QtQuick
import QtQuick.Controls

// Console do editor (conteudo inteiro do dock; a barra de titulo nativa foi escondida).
// `logModel` (LogBridge, QAbstractListModel) vem por context property. Roles: time, tag,
// message, levelColor. Acoes C++: logModel.Clear(), CopyAll(), RequestClose().
Rectangle {
    id: root
    color: "#10110f"
    implicitWidth: 600
    implicitHeight: 240

    // Pin = "travar no fim": auto-rola pra ultima msg conforme chegam. Rolar pra cima destrava.
    property bool follow: true
    property bool scrollPending: false

    function scrollToEnd() { logView.positionViewAtEnd() }
    function requestScrollToEnd() {
        if (!root.follow || root.scrollPending)
            return
        root.scrollPending = true
        Qt.callLater(function() {
            root.scrollPending = false
            if (root.follow)
                root.scrollToEnd()
        })
    }

    Connections {
        target: logModel
        function onRowsAppended() { root.requestScrollToEnd() }
    }

    // ---- Componentes reutilizaveis (inline) ----
    component IconButton: Item {
        id: ib
        property string icon
        property color color: "#9a958a"
        property int iconSize: 16
        property string tip: ""
        signal tapped()
        implicitWidth: 26; implicitHeight: 26
        Rectangle {
            anchors.fill: parent; radius: 5
            color: ibHover.hovered ? "#23241d" : "transparent"
        }
        LucideIcon { anchors.centerIn: parent; name: ib.icon; size: ib.iconSize; color: ib.color }
        HoverHandler { id: ibHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: ib.tapped() }
        ToolTip.visible: ib.tip !== "" && ibHover.hovered
        ToolTip.text: ib.tip
        ToolTip.delay: 500
    }

    component DarkMenuItem: MenuItem {
        id: mi
        implicitHeight: 30
        contentItem: Text {
            text: mi.text; color: "#c8c2b4"; font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter; leftPadding: 10; rightPadding: 16
        }
        background: Rectangle { color: mi.highlighted ? "#2a2b24" : "transparent" }
    }

    // ---- Barra de titulo (100% QML) ----
    Rectangle {
        id: titleBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 34
        color: "#141511"

        Row {
            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
            spacing: 9
            // icone do console num quadrado arredondado
            Rectangle {
                width: 24; height: 24; radius: 6
                anchors.verticalCenter: parent.verticalCenter
                color: "#16233f"; border.color: "#27406e"; border.width: 1
                LucideIcon { anchors.centerIn: parent; name: "terminal"; size: 13; color: "#5b9dff" }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Console"; color: "#e6e2d8"; font.pixelSize: 12; font.bold: true
            }
        }

        // Limpar + pin + menu agrupados a direita
        Row {
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    let value = logModel.count + "/" + logModel.maxLines
                    if (logModel.evictedCount > 0)
                        value += " · " + logModel.evictedCount + " antigas"
                    if (logModel.droppedCount > 0)
                        value += " · " + logModel.droppedCount + " perdidas"
                    return value
                }
                color: logModel.droppedCount > 0 ? "#f3b43f" : "#6c6a61"
                font.family: "Consolas"
                font.pixelSize: 10
            }

            Rectangle {
                id: clearBtn
                anchors.verticalCenter: parent.verticalCenter
                height: 24; radius: 6
                width: clearRow.width + 18
                color: clearHover.hovered ? "#23241d" : "#1a1b15"
                border.color: "#30312a"; border.width: 1
                Row {
                    id: clearRow
                    anchors.centerIn: parent
                    spacing: 6
                    LucideIcon { anchors.verticalCenter: parent.verticalCenter
                                 name: "brush-cleaning"; size: 13; color: "#5b9dff" }
                    Text { anchors.verticalCenter: parent.verticalCenter
                           text: "Limpar"; color: "#c8c2b4"; font.pixelSize: 11 }
                }
                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: logModel.Clear() }
            }

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "pin"
                color: root.follow ? "#5b9dff" : "#9a958a"
                tip: root.follow ? "Travado no fim (clique p/ destravar)" : "Travar no fim"
                onTapped: { root.follow = !root.follow; if (root.follow) root.scrollToEnd() }
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "ellipsis-vertical"
                tip: "Mais"
                onTapped: ctxMenu.popup()
            }
        }

        Menu {
            id: ctxMenu
            background: Rectangle {
                color: "#1b1c17"; border.color: "#33342c"; border.width: 1; radius: 6
                implicitWidth: 190
            }
            DarkMenuItem { text: "Copiar tudo";   onTriggered: logModel.CopyAll() }
            DarkMenuItem { text: "Rolar p/ topo";  onTriggered: { root.follow = false; logView.positionViewAtBeginning() } }
            DarkMenuItem { text: "Rolar p/ fim";   onTriggered: { root.follow = true;  logView.positionViewAtEnd() } }
            MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: "#2a2b24" } }
            DarkMenuItem { text: "Fechar";         onTriggered: logModel.RequestClose() }
        }
    }

    // ---- Lista de logs ----
    Item {
        id: listArea
        anchors { left: parent.left; right: parent.right; top: titleBar.bottom; bottom: parent.bottom
                  leftMargin: 6; rightMargin: 6; topMargin: 6; bottomMargin: 6 }

        // ---- Estado de rolagem (alimenta chevrons e o follow) ----
        readonly property bool overflow: logView.visibleArea.heightRatio < 0.999
        readonly property bool canUp:   overflow && logView.visibleArea.yPosition > 0.001
        readonly property bool canDown: overflow &&
            (logView.visibleArea.yPosition + logView.visibleArea.heightRatio) < 0.999

        function nudge(dir) {
            const step = 60
            const maxY = Math.max(0, logView.contentHeight - logView.height)
            logView.contentY = Math.max(0, Math.min(maxY, logView.contentY + dir * step))
            root.follow = logView.atYEnd
        }

        ListView {
            id: logView
            // Deixa a coluna direita (gutter) livre, sem texto correndo por baixo da barra/chevrons.
            anchors { left: parent.left; right: gutter.left; top: parent.top; bottom: parent.bottom }
            clip: true
            model: logModel
            spacing: 1
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true // recicla delegates (sao 100% model-bound, sem estado interno)

            HoverHandler { id: listHover }

            // RowsAppended e explicito: quando o FIFO remove+insere no teto, count/contentHeight
            // podem terminar iguais e nenhum *Changed seria suficiente para seguir o fim.
            onMovementStarted: root.follow = false
            onMovementEnded: root.follow = atYEnd
            Component.onCompleted: positionViewAtEnd()

            delegate: Row {
                width: logView.width
                spacing: 6
                Text {
                    id: timeCol
                    text: model.time; color: "#6c6a61"
                    font.family: "Consolas"; font.pixelSize: 12
                }
                Text {
                    id: tagCol
                    width: 38; text: model.tag; color: model.levelColor
                    font.family: "Consolas"; font.pixelSize: 12; font.bold: true
                }
                Text {
                    width: Math.max(0, logView.width - timeCol.width - tagCol.width - 12)
                    text: model.message; color: "#b9b5aa"
                    font.family: "Consolas"; font.pixelSize: 12
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                }
            }
        }

        // ---- Coluna direita: chevron-cima + scrollbar + chevron-baixo (empilhados/centralizados) ----
        Item {
            id: gutter
            width: 16
            anchors { right: parent.right; top: parent.top; bottom: parent.bottom }

            // Mantem a scrollbar visivel enquanto o mouse esta na coluna (inclusive sobre as setas).
            HoverHandler { id: gutterHover }

            IconButton {
                id: chevUp
                icon: "chevron-up"; color: "#c8c2b4"; iconSize: 12
                width: 16; height: 14
                anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
                visible: listArea.canUp
                onTapped: listArea.nudge(-1)
            }
            IconButton {
                id: chevDown
                icon: "chevron-down"; color: "#c8c2b4"; iconSize: 12
                width: 16; height: 14
                anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
                visible: listArea.canDown
                onTapped: listArea.nudge(1)
            }

            // Scrollbar ENTRE os chevrons (slots de 14px reservados em cima/embaixo, nao por baixo deles).
            // Standalone (nao-anexada) p/ poder ser encurtada: ligamos manualmente ao Flickable.
            ThinScrollBar {
                id: vbar
                orientation: Qt.Vertical
                anchors { top: chevUp.bottom; topMargin: 2
                          bottom: chevDown.top; bottomMargin: 2
                          horizontalCenter: parent.horizontalCenter }
                revealed: listHover.hovered || gutterHover.hovered
                size: logView.visibleArea.heightRatio
                position: logView.visibleArea.yPosition
                active: logView.movingVertically
                onPositionChanged: if (pressed) {
                    root.follow = false
                    logView.contentY = position * logView.contentHeight
                }
                onPressedChanged: if (!pressed) root.follow = logView.atYEnd
            }
        }
    }
}
