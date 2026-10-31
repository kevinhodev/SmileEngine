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
    // `moved` previews continuously; `committed` fires once after text entry, reset or drag.
    // Consumers that invalidate histories/caches should write only on `committed`.
    signal committed(real value)

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
        if (!isNaN(parsed)) {
            const nextValue = normalized(parsed / displayScale)
            moved(nextValue)
            committed(nextValue)
        }
    }

    function cancelEdit() {
        editing = false
    }

    function reset() {
        if (interactive && !isNaN(defaultValue)) {
            const nextValue = normalized(defaultValue)
            moved(nextValue)
            committed(nextValue)
        }
    }

    // Slider de preenchimento (Cartao 18%): o campo rebaixado enche ate o valor em cinza; ao
    // arrastar o preenchimento esquenta e a borda do valor acende no accent.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: dragArea.containsMouse && !root.dragging ? Theme.surfaceHover : Theme.surface
        border.width: 1
        border.color: root.editing ? Theme.accent : "transparent"
        clip: true

        Rectangle {
            id: fill
            visible: root.showProgress && !root.editing
            width: Math.max(0, Math.min(1, root.visualPosition)) * parent.width
            height: parent.height
            color: root.dragging ? Theme.sliderDrag : Theme.sliderFill
        }
        Rectangle {
            visible: fill.visible && fill.width > 0
            x: fill.width - 1
            width: 1
            height: parent.height
            color: root.dragging ? Theme.accent : Theme.sliderEdge
        }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.caption !== "" && !root.editing
        text: root.caption
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textMeta
        font.weight: Font.DemiBold
    }

    Text {
        id: valueLabel
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.editing
        text: root.formatted(root.displayValue)
        color: Theme.textPrimary
        font.family: Theme.fontNum
        font.pixelSize: Theme.textBody
    }

    TextInput {
        id: editor
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(82, parent.width - 16)
        visible: root.editing
        color: Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentInk
        horizontalAlignment: TextInput.AlignRight
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        font.family: Theme.fontNum
        font.pixelSize: Theme.textBody
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
        onReleased: {
            if (root.dragging)
                root.committed(root.previewValue)
            root.dragging = false
        }
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
