pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components" as C

// Console inferior do editor no conceito Cartao 18%. Concentra abas, filtros, busca e
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
        radius: C.Theme.radiusControl
        opacity: enabledVisual ? 1.0 : 0.38
        color: active ? C.Theme.raisedOn
                      : (buttonHover.hovered && enabledVisual ? C.Theme.raisedHover
                                                              : "transparent")

        LucideIcon {
            anchors.centerIn: parent
            name: iconButton.iconName
            size: 14
            color: iconButton.active ? C.Theme.textPrimary
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

    // Aba da faixa (Cartao 18%): a ativa e um bloco da cor do painel, ligado ao conteudo sem
    // fio por baixo; as outras ficam no chrome. Traco accent em cima so com o foco no console.
    component PanelTab: Item {
        id: tab

        required property string label
        property bool current: false
        property bool available: true
        property int badge: -1
        signal picked()

        implicitWidth: tabLabel.implicitWidth + 28 + (badgeBox.visible ? badgeBox.width + 7 : 0)
        implicitHeight: C.Theme.tabHeight

        Rectangle {
            visible: tab.current
            anchors.fill: parent
            color: C.Theme.shell
            Rectangle { anchors { left: parent.left; top: parent.top; bottom: parent.bottom } width: 1; color: C.Theme.hairlineStrong }
            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: C.Theme.hairlineStrong }
            Rectangle {
                visible: root.activeFocus || searchInput.activeFocus
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 2
                color: C.Theme.accent
            }
        }
        Text {
            id: tabLabel
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            text: tab.label
            color: !tab.available ? C.Theme.textDisabled
                 : tab.current ? C.Theme.textPrimary
                 : (tabHover.hovered ? C.Theme.textSecondary : C.Theme.textMuted)
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textSection
            font.weight: tab.current ? Font.DemiBold : Font.Medium
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Rectangle {
            id: badgeBox
            visible: tab.badge >= 0
            anchors.left: tabLabel.right
            anchors.leftMargin: 7
            anchors.verticalCenter: parent.verticalCenter
            width: badgeText.implicitWidth + 10
            height: 16
            radius: C.Theme.radiusChip
            color: C.Theme.surfaceRaised
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: tab.badge
                color: C.Theme.textSecondary
                font.family: C.Theme.fontNum
                font.pixelSize: C.Theme.textMeta
                font.weight: Font.Medium
            }
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

    // Filtro por nivel: ponto na cor semantica do nivel + rotulo + contagem do historico.
    // Ligado = botao em relevo; desligado = so texto.
    component FilterChip: Rectangle {
        id: chip

        required property string label
        property string level: ""
        property int badge: -1
        property color dotColor: "transparent"
        readonly property bool active: root.levelFilter === level

        implicitWidth: chipRow.implicitWidth + 18
        implicitHeight: 22
        radius: C.Theme.radiusControl
        color: active ? C.Theme.raisedOn
                      : (chipHover.hovered ? C.Theme.raisedHover : "transparent")

        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: 6
            Rectangle {
                visible: chip.dotColor.a > 0
                anchors.verticalCenter: parent.verticalCenter
                width: 7; height: 7; radius: 3.5
                color: chip.badge === 0 ? C.Theme.textDisabled : chip.dotColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                color: chip.active ? C.Theme.textPrimary : C.Theme.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: C.Theme.textBody
                font.weight: Font.Medium
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
            Text {
                visible: chip.badge >= 0
                anchors.verticalCenter: parent.verticalCenter
                text: chip.badge
                color: C.Theme.textMuted
                font.family: C.Theme.fontNum
                font.pixelSize: C.Theme.textBody
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }
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
            color: menuItem.highlighted ? C.Theme.raisedOn : "transparent"
        }
    }

    // Fio escuro no topo: separacao de primeira ordem (painel x viewport).
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
        height: C.Theme.tabHeight

        Rectangle { anchors.fill: parent; color: C.Theme.chrome }

        RowLayout {
            anchors.fill: parent
            anchors.rightMargin: 8
            spacing: 0

            PanelTab { label: "Console"; current: true; badge: root.logModel.count }
            PanelTab { label: "Build";    available: false }
            PanelTab { label: "Profiler"; available: false }
            PanelTab { label: "Assets";   available: false }

            Item { Layout.fillWidth: true }

            Row {
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: 10
                spacing: 4

                FilterChip { label: "Todos" }
                FilterChip { label: "Info";   level: "INFO"; badge: root.logModel.infoCount; dotColor: C.Theme.blue }
                FilterChip { label: "Avisos"; level: "WARN"; badge: root.logModel.warnCount; dotColor: C.Theme.warn }
                FilterChip { label: "Erros";  level: "ERR";  badge: root.logModel.errorCount; dotColor: C.Theme.error }
            }

            Rectangle {
                id: searchBox

                visible: !root.compact || root.searchExpanded || searchInput.text !== ""
                Layout.preferredWidth: root.width > 1400 ? 240 : 180
                Layout.preferredHeight: 22
                Layout.rightMargin: 6
                radius: C.Theme.radiusControl
                color: C.Theme.surface
                border.color: searchInput.activeFocus ? C.Theme.accent : "transparent"
                border.width: 1

                LucideIcon {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: 13
                    color: C.Theme.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchInput.text === "" && !searchInput.activeFocus
                    text: "Filtrar mensagens"
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: C.Theme.textBody
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
                    selectionColor: C.Theme.accent
                    selectedTextColor: C.Theme.accentInk
                    font.family: C.Theme.fontFamily
                    font.pixelSize: C.Theme.textBody
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
                font.family: C.Theme.fontNum
                font.pixelSize: C.Theme.textMeta
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
            color: C.Theme.hairlineStrong
        }
    }

    Menu {
        id: contextMenu

        background: Rectangle {
            color: C.Theme.surfaceRaised
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
            font.pixelSize: C.Theme.textBody
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
                                                : tag === "WARN" ? C.Theme.warn
                                                : tag === "INFO" ? C.Theme.blue
                                                : C.Theme.textMuted

                width: logView.width
                height: accepted ? 22 : 0
                visible: accepted
                clip: true

                // Aviso e erro tingem a LINHA inteira: num log comprido e o fundo que o olho
                // acha primeiro, nao a sigla.
                Rectangle {
                    anchors.fill: parent
                    color: logRow.tag === "ERR" ? C.Theme.errorBg
                         : logRow.tag === "WARN" ? C.Theme.warnRow
                         : rowHover.hovered ? C.Theme.rowHover : "transparent"
                }

                Text {
                    id: timeColumn

                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 96
                    text: logRow.time
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontMono
                    font.pixelSize: C.Theme.textBody
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
                }

                // Nivel: sigla em mono na cor semantica, sem caixa — a coluna fixa ja alinha.
                Text {
                    id: levelBadge

                    anchors.left: timeColumn.right
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    text: logRow.tag
                    color: logRow.tagColor
                    font.family: C.Theme.fontMono
                    font.pixelSize: C.Theme.textBody
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
                }

                Text {
                    id: messageColumn

                    anchors.left: levelBadge.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: logRow.message
                    color: logRow.tag === "ERR" ? C.Theme.errorText
                         : logRow.tag === "WARN" ? C.Theme.warnText
                         : C.Theme.textNormal
                    font.family: C.Theme.fontConsole
                    font.pixelSize: C.Theme.textBody
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
