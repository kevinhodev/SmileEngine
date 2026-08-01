import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components" as C

// Console compacto do editor. O painel funciona como um drawer de apoio: uma unica faixa
// concentra titulo, filtros, busca e acoes; cada evento ocupa somente uma linha.
Rectangle {
    id: root

    color: "#0f100e"
    implicitWidth: 700
    implicitHeight: 124
    focus: true

    property bool follow: true
    property bool scrollPending: false
    property bool searchExpanded: false
    property string levelFilter: ""
    readonly property string searchText: searchInput.text.trim().toLowerCase()
    readonly property bool compact: width < 860

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
        target: logModel
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
        radius: 5
        opacity: enabledVisual ? 1.0 : 0.38
        color: active ? C.Theme.greenBg
                      : (buttonHover.hovered && enabledVisual ? "#22241e" : "transparent")
        border.color: active ? C.Theme.greenBorder : "transparent"
        border.width: 1

        LucideIcon {
            anchors.centerIn: parent
            name: iconButton.iconName
            size: 13
            color: iconButton.active ? C.Theme.green
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

    component FilterChip: Rectangle {
        id: chip

        required property string label
        property string level: ""
        property color accent: C.Theme.green
        readonly property bool active: root.levelFilter === level

        implicitWidth: chipRow.implicitWidth + 14
        implicitHeight: 22
        radius: 5
        color: active ? C.Theme.greenBg
                      : (chipHover.hovered ? "#20221c" : "transparent")
        border.color: active ? C.Theme.greenBorder
                             : (chipHover.hovered ? "#33352d" : "transparent")
        border.width: 1

        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: 5

            Rectangle {
                visible: chip.level !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 5
                height: 5
                radius: 2.5
                color: chip.accent
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                color: chip.active ? C.Theme.greenText : C.Theme.textSecondary
                font.family: C.Theme.fontFamily
                font.pixelSize: 10
                font.weight: chip.active ? Font.DemiBold : Font.Normal
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
            font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
            leftPadding: 10
            rightPadding: 16
        }
        background: Rectangle {
            color: menuItem.highlighted ? "#292b24" : "transparent"
        }
    }

    // Faixa unica de comando: 30 px mais borda.
    Rectangle {
        id: commandBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 31
        color: "#141511"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 7
            spacing: 5

            Rectangle {
                Layout.preferredWidth: 21
                Layout.preferredHeight: 21
                radius: 5
                color: C.Theme.greenBg
                border.color: C.Theme.greenBorder
                border.width: 1

                LucideIcon {
                    anchors.centerIn: parent
                    name: "terminal"
                    size: 12
                    color: C.Theme.green
                }
            }

            Text {
                Layout.alignment: Qt.AlignVCenter
                text: "Console"
                color: C.Theme.textPrimary
                font.family: C.Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.DemiBold
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality
            }

            Rectangle {
                Layout.leftMargin: 3
                Layout.rightMargin: 2
                Layout.preferredWidth: 1
                Layout.preferredHeight: 17
                color: C.Theme.borderColor
            }

            Row {
                Layout.alignment: Qt.AlignVCenter
                spacing: 1

                FilterChip { label: "Todos" }
                FilterChip { label: "Info"; level: "INFO"; accent: "#7fa6ff" }
                FilterChip { label: "Avisos"; level: "WARN"; accent: "#e8b45f" }
                FilterChip { label: "Erros"; level: "ERR"; accent: "#ef6961" }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                id: searchBox

                visible: !root.compact || root.searchExpanded || searchInput.text !== ""
                Layout.preferredWidth: root.width > 1150 ? 210 : 165
                Layout.preferredHeight: 22
                radius: 5
                color: "#0d0f0c"
                border.color: searchInput.activeFocus ? C.Theme.greenBorder : "#2d2f28"
                border.width: 1

                LucideIcon {
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: 11
                    color: searchInput.activeFocus ? C.Theme.green : C.Theme.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 23
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchInput.text === "" && !searchInput.activeFocus
                    text: "Buscar nas mensagens"
                    color: C.Theme.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.HighRenderTypeQuality
                }

                TextInput {
                    id: searchInput

                    anchors.left: parent.left
                    anchors.leftMargin: 23
                    anchors.right: clearSearch.left
                    anchors.rightMargin: 3
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    verticalAlignment: TextInput.AlignVCenter
                    color: C.Theme.textNormal
                    selectionColor: C.Theme.green
                    selectedTextColor: "#10110f"
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
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
                    width: searchInput.text !== "" ? 19 : 0
                    height: 19
                    visible: width > 0

                    LucideIcon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 10
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
                Layout.leftMargin: 2
                text: logModel.count + "/" + logModel.maxLines
                color: logModel.droppedCount > 0 ? "#e8b45f" : C.Theme.textMuted
                font.family: C.Theme.fontConsole
                font.pixelSize: 10
                renderType: Text.QtRendering
                renderTypeQuality: Text.HighRenderTypeQuality

                C.ToolTip {
                    active: countHover.hovered
                    above: true
                    text: logModel.evictedCount + " antigas removidas · "
                        + logModel.droppedCount + " mensagens perdidas"
                }
                HoverHandler { id: countHover }
            }

            ConsoleIconButton {
                iconName: "eraser"
                tip: "Limpar console"
                enabledVisual: logModel.count > 0
                onTapped: logModel.Clear()
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
                onTapped: logModel.RequestClose()
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: "#25271f"
        }
    }

    Menu {
        id: contextMenu

        background: Rectangle {
            color: "#1b1c17"
            border.color: "#33342c"
            border.width: 1
            radius: 6
            implicitWidth: 184
        }
        DarkMenuItem { text: "Copiar tudo"; onTriggered: logModel.CopyAll() }
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
            contentItem: Rectangle { implicitHeight: 1; color: "#2a2b24" }
        }
        DarkMenuItem { text: "Fechar console"; onTriggered: logModel.RequestClose() }
    }

    Item {
        id: listArea

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: commandBar.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 6
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
            visible: logModel.count === 0
            text: "Nenhuma mensagem nesta sessão"
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: 10
            renderType: Text.NativeRendering
        }

        ListView {
            id: logView

            anchors.left: parent.left
            anchors.right: gutter.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            clip: true
            model: logModel
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
                required property color levelColor

                readonly property bool accepted: root.rowMatches(tag, message)

                width: logView.width
                height: accepted ? 22 : 0
                visible: accepted
                clip: true

                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    color: rowHover.hovered ? "#1b1d18"
                                            : (logRow.index % 2 === 0 ? "#131410"
                                                                      : "transparent")
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    radius: 1
                    color: logRow.levelColor
                    opacity: 0.9
                }

                Text {
                    id: timeColumn

                    anchors.left: parent.left
                    anchors.leftMargin: 11
                    anchors.verticalCenter: parent.verticalCenter
                    width: 76
                    text: logRow.time
                    color: "#77786d"
                    font.family: C.Theme.fontConsole
                    font.pixelSize: 10
                    font.hintingPreference: Font.PreferVerticalHinting
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
                }

                Text {
                    id: levelColumn

                    anchors.left: timeColumn.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 37
                    text: logRow.tag
                    color: logRow.levelColor
                    font.family: C.Theme.fontConsole
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.hintingPreference: Font.PreferVerticalHinting
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
                }

                Text {
                    id: messageColumn

                    anchors.left: levelColumn.right
                    anchors.leftMargin: 7
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: logRow.message
                    color: logRow.tag === "ERR" ? "#d9b3af"
                                               : (logRow.tag === "WARN" ? "#d3c8ae"
                                                                        : C.Theme.textNormal)
                    font.family: C.Theme.fontConsole
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.hintingPreference: Font.PreferVerticalHinting
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    renderType: Text.QtRendering
                    renderTypeQuality: Text.VeryHighRenderTypeQuality
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
