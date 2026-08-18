pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property real from: 0
    property real to: 1
    property real stepSize: 0.01
    property real boundValue: 0
    property real defaultValue: Number.NaN
    property int decimals: 2
    property real displayScale: 1
    property string prefix: ""
    property string suffix: ""
    property string caption: ""
    property bool interactive: true
    property bool showProgress: true
    property real dragPixels: 180

    signal moved(real value)

    readonly property bool modified: !isNaN(defaultValue)
                                     && Math.abs(boundValue - defaultValue)
                                        > Math.max(stepSize * 0.5, 0.000001)
    readonly property real displayValue: dragging ? previewValue : boundValue
    readonly property real visualPosition: to === from ? 0
                                                       : (displayValue - from) / (to - from)

    property bool editing: false
    property bool dragging: false
    property real previewValue: boundValue
    property real pressValue: 0
    property real pressX: 0

    implicitHeight: 24
    opacity: interactive ? 1 : 0.45

    function normalized(value) {
        let result = Math.max(from, Math.min(to, value))
        if (stepSize > 0)
            result = from + Math.round((result - from) / stepSize) * stepSize
        return Math.max(from, Math.min(to, result))
    }

    function formatted(value) {
        return prefix + Number(value * displayScale).toFixed(decimals).replace(".", ",") + suffix
    }

    function beginEdit() {
        if (!interactive)
            return
        editing = true
        editor.text = Number(displayValue * displayScale).toFixed(decimals).replace(".", ",")
        Qt.callLater(function() {
            editor.forceActiveFocus()
            editor.selectAll()
        })
    }

    function commitEdit() {
        if (!editing)
            return
        const parsed = Number(editor.text.trim().replace(",", "."))
        editing = false
        if (!isNaN(parsed))
            moved(normalized(parsed / displayScale))
    }

    function cancelEdit() {
        editing = false
    }

    function reset() {
        if (interactive && !isNaN(defaultValue))
            moved(normalized(defaultValue))
    }

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: root.editing ? Theme.greenBg
                            : dragArea.containsMouse ? Theme.controlHover : "#111209"
        border.width: 1
        border.color: root.editing || root.dragging ? Theme.green
                                                    : dragArea.containsMouse ? Theme.greenBorder
                                                                             : Theme.controlBorder

        Rectangle {
            visible: root.showProgress
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: Math.max(0, Math.min(1, root.visualPosition)) * parent.width
            height: 2
            radius: 1
            color: Theme.green
        }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.caption !== "" && !dragArea.containsMouse && !root.dragging
        text: root.caption.toUpperCase()
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 7
        font.weight: Font.DemiBold
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        visible: (dragArea.containsMouse || root.dragging) && !root.editing
        text: "\u2194"
        color: Theme.greenText
        font.family: Theme.fontFamily
        font.pixelSize: 12
    }

    Text {
        id: valueLabel
        anchors.right: parent.right
        anchors.rightMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.editing
        text: root.formatted(root.displayValue)
        color: root.dragging ? Theme.greenText : Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: 9
        font.weight: Font.DemiBold
    }

    TextInput {
        id: editor
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(82, parent.width - 16)
        visible: root.editing
        color: Theme.textPrimary
        selectionColor: Theme.greenBorder
        selectedTextColor: Theme.textPrimary
        horizontalAlignment: TextInput.AlignRight
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        font.family: Theme.fontMono
        font.pixelSize: 9
        onEditingFinished: root.commitEdit()
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Escape) {
                root.cancelEdit()
                event.accepted = true
            }
        }
    }

    MouseArea {
        id: dragArea
        anchors.fill: parent
        anchors.rightMargin: Math.min(78, parent.width * 0.42)
        enabled: root.interactive && !root.editing
        hoverEnabled: true
        preventStealing: true
        cursorShape: containsMouse ? Qt.SizeHorCursor : Qt.ArrowCursor

        onPressed: mouse => {
            root.pressX = mouse.x
            root.pressValue = root.boundValue
            root.previewValue = root.boundValue
            root.dragging = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const deltaPixels = mouse.x - root.pressX
            if (!root.dragging && Math.abs(deltaPixels) < 2)
                return
            root.dragging = true
            const fineScale = (mouse.modifiers & Qt.ShiftModifier) ? 0.1 : 1
            const valueDelta = deltaPixels / root.dragPixels * (root.to - root.from) * fineScale
            const nextValue = root.normalized(root.pressValue + valueDelta)
            if (nextValue !== root.previewValue) {
                root.previewValue = nextValue
                root.moved(nextValue)
            }
        }
        onReleased: root.dragging = false
        onCanceled: root.dragging = false
        onDoubleClicked: root.reset()
    }

    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(78, parent.width * 0.42)
        enabled: root.interactive && !root.editing
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        onClicked: root.beginEdit()
    }
}
