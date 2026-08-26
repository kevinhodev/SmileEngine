pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components" as C

// Console inferior do editor no conceito Signal Graphite. Concentra abas, filtros, busca e
// acoes. O rodape de metricas vive no StatusBar.qml, fora deste dock, para continuar visivel
// quando o usuario fecha os paineis e libera mais area para o viewport.
//
// Build/Profiler/Assets aparecem desabilitadas de proposito: o conceito expoe a estrutura em
// vez de esconde-la, e a aba muda ao ganhar painel — nao ha layout novo a inventar depois.
//
// Dependencia explicita da raiz (chega por setInitialProperties):
//  - `logModel` (LogBridge): linhas, contagens por nivel e acoes do console.
Rectangle {
    id: root
    required property var logModel

    color: C.Theme.shell
    implicitWidth: 700
    implicitHeight: 134
    focus: true

    property bool follow: true
    property bool scrollPending: false
    property bool searchExpanded: false
    property string levelFilter: ""
    readonly property string searchText: searchInput.text.trim().toLowerCase()
    readonly property bool compact: width < 980
    readonly property int filteredCount: {
        const revision = logModel.count
        return revision >= 0 ? logModel.MatchCount(levelFilter, searchText) : 0
    }

    function rowMatches(tag, message) {
        const levelMatches = root.levelFilter === "" || tag === root.levelFilter
        if (!levelMatches)
            return false
        if (root.searchText === "")
            return true
        return tag.toLowerCase().indexOf(root.searchText) >= 0
            || message.toLowerCase().indexOf(root.searchText) >= 0
    }

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
        target: root.logModel
        function onRowsAppended() { root.requestScrollToEnd() }
    }

    Shortcut {
        sequence: StandardKey.Find
        onActivated: {
            searchInput.forceActiveFocus()
            searchInput.selectAll()
        }
    }

    component ConsoleIconButton: Rectangle {
        id: iconButton

        property string iconName
        property string tip: ""
        property bool active: false
        property bool enabledVisual: true
        signal tapped()

        implicitWidth: 24
        implicitHeight: 22
        radius: C.Theme.radiusChip
        opacity: enabledVisual ? 1.0 : 0.38
        color: active ? C.Theme.accentBg
                      : (buttonHover.hovered && enabledVisual ? C.Theme.surfaceRaised
                                                              : "transparent")
        border.color: active ? C.Theme.accentBorder : "transparent"
        border.width: 1

        LucideIcon {
            anchors.centerIn: parent
            name: iconButton.iconName
            size: 13
            color: iconButton.active ? C.Theme.accent
                                     : (buttonHover.hovered ? C.Theme.textPrimary
                                                            : C.Theme.textSecondary)
        }

        HoverHandler {
            id: buttonHover
            cursorShape: iconButton.enabledVisual ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler {
            enabled: iconButton.enabledVisual
            onTapped: iconButton.tapped()
        }
        C.ToolTip {
            active: iconButton.tip !== "" && buttonHover.hovered
            text: iconButton.tip
            above: true
        }
    }

    // Aba da faixa. Sublinhado accent de 2 px marca a ativa — sem pilula e sem moldura,
    // que e o que mantem a densidade do chrome.
    component PanelTab: Item {
        id: tab

        required property string label
        property bool current: false
        property bool available: true
        signal picked()

        implicitWidth: tabLabel.implicitWidth + 20
        implicitHeight: 36

        Text {
            id: tabLabel
            anchors.centerIn: parent
            text: tab.label
            color: !tab.available ? C.Theme.textMuted
                 : tab.current ? C.Theme.textPrimary
                 : (tabHover.hovered ? C.Theme.textSecondary : C.Theme.textMuted)
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
            font.weight: tab.current ? Font.DemiBold : Font.Normal
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Rectangle {
            visible: tab.current
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: parent.width - 8
            height: 2
            color: C.Theme.accent
        }
        HoverHandler {
            id: tabHover
            cursorShape: tab.available ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler { enabled: tab.available; onTapped: tab.picked() }
        C.ToolTip {
            active: !tab.available && tabHover.hovered
            text: "Ainda sem painel"
            above: true
        }
    }

    // Chip de filtro por nivel, com a contagem do que esta no historico.
    component FilterChip: Rectangle {
        id: chip

        required property string label
        property string level: ""
        property int badge: -1
        readonly property bool active: root.levelFilter === level

        implicitWidth: chipLabel.implicitWidth + 22
        implicitHeight: 22
        radius: C.Theme.radiusChip
        color: active ? C.Theme.accentBg
                      : (chipHover.hovered ? C.Theme.surfaceRaised : C.Theme.surface)
        border.color: active ? C.Theme.accentBorder : C.Theme.hairline
        border.width: 1

        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.badge >= 0 ? chip.label + " · " + chip.badge : chip.label
            color: chip.active ? C.Theme.accent : C.Theme.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
            font.weight: chip.active ? Font.DemiBold : Font.Normal
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }

        HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: {
                root.levelFilter = chip.level
                root.requestScrollToEnd()
            }
        }
    }

    component DarkMenuItem: MenuItem {
        id: menuItem

        implicitHeight: 29
        contentItem: Text {
            text: menuItem.text
            color: C.Theme.textNormal
            font.family: C.Theme.fontFamily
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            leftPadding: 10
            rightPadding: 16
        }
        background: Rectangle {
            color: menuItem.highlighted ? C.Theme.surfaceRaised : "transparent"
        }
    }

    // Hairline FORTE no topo: e ela que separa a faixa do viewport, e o conceito reserva o
    // fio de #333338 exatamente para as separacoes de primeira ordem.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: C.Theme.hairlineStrong
        z: 2
    }

    // ---- Faixa de abas, filtros e acoes ----
    Item {
        id: commandBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 37

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 8
            spacing: 0

            LucideIcon {
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: 9
                name: "terminal"
                size: 15
                color: C.Theme.textPrimary
            }

            PanelTab { label: "Console"; current: true }
            PanelTab { label: "Build";    available: false }
            PanelTab { label: "Profiler"; available: false }
            PanelTab { label: "Assets";   available: false }

            Rectangle {
                Layout.leftMargin: 10
                Layout.rightMargin: 12
                Layout.preferredWidth: 1
                Layout.preferredHeight: 20
                color: C.Theme.hairline
            }

            Row {
                Layout.alignment: Qt.AlignVCenter
                spacing: 8

                FilterChip { label: "Todos" }
                FilterChip { label: "Info";   level: "INFO"; badge: root.logModel.infoCount }
                FilterChip { label: "Avisos"; level: "WARN"; badge: root.logModel.warnCount }
                FilterChip { label: "Erros";  level: "ERR";  badge: root.logModel.errorCount }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                id: searchBox

                visible: !root.compact || root.searchExpanded || searchInput.text !== ""
                Layout.preferredWidth: root.width > 1400 ? 240 : 180
                Layout.preferredHeight: 24
                Layout.rightMargin: 6
                radius: C.Theme.radiusControl
                color: C.Theme.surface
                border.color: searchInput.activeFocus ? C.Theme.accentBorder : C.Theme.hairline
                border.width: 1

                LucideIcon {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: 13
                    color: searchInput.activeFocus ? C.Theme.accent : C.Theme.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchInput.text === "" && !searchInput.activeFocus
                    text: "Filtrar mensagens…"
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.HighRenderTypeQuality
                }

                TextInput {
                    id: searchInput

                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.right: clearSearch.left
                    anchors.rightMargin: 3
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    verticalAlignment: TextInput.AlignVCenter
                    color: C.Theme.textPrimary
                    selectionColor: C.Theme.accentBorder
                    selectedTextColor: C.Theme.shell
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                    renderType: Text.QtRendering
                    selectByMouse: true
                    clip: true

                    Keys.onEscapePressed: {
                        if (text !== "")
                            clear()
                        else {
                            root.searchExpanded = false
                            focus = false
                        }
                    }
                }

                Item {
                    id: clearSearch

                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: searchInput.text !== "" ? 20 : 0
                    height: 20
                    visible: width > 0

                    LucideIcon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 11
                        color: clearSearchHover.hovered ? C.Theme.textPrimary : C.Theme.textMuted
                    }
                    HoverHandler { id: clearSearchHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: searchInput.clear() }
                }
            }

            ConsoleIconButton {
                visible: root.compact && !searchBox.visible
                iconName: "search"
                tip: "Buscar"
                onTapped: {
                    root.searchExpanded = true
                    Qt.callLater(function() { searchInput.forceActiveFocus() })
                }
            }

            Text {
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: 6
                text: root.logModel.count + "/" + root.logModel.maxLines
                color: root.logModel.droppedCount > 0 ? C.Theme.amber : C.Theme.textMuted
                font.family: C.Theme.fontMono
                font.pixelSize: 10
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality

                C.ToolTip {
                    active: countHover.hovered
                    above: true
                    text: root.logModel.evictedCount + " antigas removidas · "
                        + root.logModel.droppedCount + " mensagens perdidas"
                }
                HoverHandler { id: countHover }
            }

            ConsoleIconButton {
                iconName: "eraser"
                tip: "Limpar console"
                enabledVisual: root.logModel.count > 0
                onTapped: root.logModel.Clear()
            }

            ConsoleIconButton {
                iconName: "pin"
                tip: root.follow ? "Seguindo as novas mensagens" : "Seguir novas mensagens"
                active: root.follow
                onTapped: {
                    root.follow = !root.follow
                    if (root.follow)
                        root.scrollToEnd()
                }
            }

            ConsoleIconButton {
                iconName: "ellipsis-vertical"
                tip: "Mais ações"
                onTapped: contextMenu.popup()
            }

            ConsoleIconButton {
                iconName: "chevron-down"
                tip: "Recolher console"
                onTapped: root.logModel.RequestClose()
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: C.Theme.hairline
        }
    }

    Menu {
        id: contextMenu

        background: Rectangle {
            color: C.Theme.surface
            border.color: C.Theme.hairlineStrong
            border.width: 1
            radius: C.Theme.radiusCard
            implicitWidth: 184
        }
        DarkMenuItem { text: "Copiar tudo"; onTriggered: root.logModel.CopyAll() }
        DarkMenuItem {
            text: "Rolar para o topo"
            onTriggered: {
                root.follow = false
                logView.positionViewAtBeginning()
            }
        }
        DarkMenuItem {
            text: "Rolar para o fim"
            onTriggered: {
                root.follow = true
                logView.positionViewAtEnd()
            }
        }
        MenuSeparator {
            contentItem: Rectangle { implicitHeight: 1; color: C.Theme.hairline }
        }
        DarkMenuItem { text: "Fechar console"; onTriggered: root.logModel.RequestClose() }
    }

    Item {
        id: listArea

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: commandBar.bottom
        anchors.bottom: parent.bottom
        anchors.rightMargin: 5
        anchors.topMargin: 4
        anchors.bottomMargin: 4

        readonly property bool overflow: logView.visibleArea.heightRatio < 0.999
        readonly property bool canUp: overflow && logView.visibleArea.yPosition > 0.001
        readonly property bool canDown: overflow
            && logView.visibleArea.yPosition + logView.visibleArea.heightRatio < 0.999

        function nudge(direction) {
            const step = 66
            const maxY = Math.max(0, logView.contentHeight - logView.height)
            logView.contentY = Math.max(
                0, Math.min(maxY, logView.contentY + direction * step))
            root.follow = logView.atYEnd
        }

        Text {
            anchors.centerIn: parent
            visible: root.filteredCount === 0
            text: root.logModel.count === 0
                ? "Nenhuma mensagem nesta sessão"
                : "Nenhuma mensagem para este filtro"
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 11
        }

        ListView {
            id: logView

            anchors.left: parent.left
            anchors.right: gutter.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            clip: true
            model: root.logModel
            spacing: 0
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            HoverHandler { id: listHover }
            onMovementStarted: root.follow = false
            onMovementEnded: root.follow = atYEnd
            Component.onCompleted: positionViewAtEnd()

            delegate: Item {
                id: logRow

                required property int index
                required property string time
                required property string tag
                required property string message
                readonly property bool accepted: root.rowMatches(tag, message)
                // Azul e info/GPU, ambar e atencao, vermelho e erro — mesma tabela semantica
                // do resto do editor, e nenhum deles e o accent.
                readonly property color tagColor: tag === "ERR" ? C.Theme.error
                                                : tag === "WARN" ? C.Theme.amber
                                                : C.Theme.textSecondary

                width: logView.width
                height: accepted ? 24 : 0
                visible: accepted
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: rowHover.hovered ? C.Theme.surface : "transparent"
                }

                Text {
                    id: timeColumn

                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 82
                    text: logRow.time
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: 10
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
                }

                // Badge do nivel: retangulo de raio 2 com a sigla em mono, no lugar da barra
                // colorida da borda — e o que faz o nivel ser LIDO e nao so percebido.
                Rectangle {
                    id: levelBadge

                    anchors.left: timeColumn.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42
                    height: 16
                    radius: C.Theme.radiusChip
                    color: logRow.tag === "ERR" ? C.Theme.errorBg
                         : logRow.tag === "WARN" ? "#2A2413"
                         : C.Theme.surfaceRaised

                    Text {
                        anchors.centerIn: parent
                        text: logRow.tag
                        color: logRow.tagColor
                        font.family: C.Theme.fontMono
                        font.pixelSize: 9
                        renderType: Text.QtRendering
                        renderTypeQuality: Text.VeryHighRenderTypeQuality
                    }
                }

                Text {
                    id: messageColumn

                    anchors.left: levelBadge.right
                    anchors.leftMargin: 16
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: logRow.message
                    color: logRow.tag === "ERR" ? C.Theme.error
                         : logRow.tag === "WARN" ? C.Theme.textNormal
                         : C.Theme.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 11
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.HighRenderTypeQuality
                }

                HoverHandler { id: rowHover }
            }
        }

        Item {
            id: gutter

            width: 14
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            HoverHandler { id: gutterHover }

            ConsoleIconButton {
                id: chevronUp

                iconName: "chevron-up"
                width: 14
                height: 12
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                visible: listArea.canUp
                onTapped: listArea.nudge(-1)
            }

            ConsoleIconButton {
                id: chevronDown

                iconName: "chevron-down"
                width: 14
                height: 12
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                visible: listArea.canDown
                onTapped: listArea.nudge(1)
            }

            ThinScrollBar {
                id: verticalBar

                orientation: Qt.Vertical
                anchors.top: chevronUp.bottom
                anchors.topMargin: 2
                anchors.bottom: chevronDown.top
                anchors.bottomMargin: 2
                anchors.horizontalCenter: parent.horizontalCenter
                thickness: 6
                revealed: listHover.hovered || gutterHover.hovered
                size: logView.visibleArea.heightRatio
                position: logView.visibleArea.yPosition
                active: logView.movingVertically

                onPositionChanged: if (pressed) {
                    root.follow = false
                    logView.contentY = position * logView.contentHeight
                }
                onPressedChanged: if (!pressed)
                    root.follow = logView.atYEnd
            }
        }
    }

}
