pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property string label
    property int from: 0
    property int to: 100
    property int stepSize: 1
    property int boundValue: 0
    property string suffix: ""
    property bool interactive: true

    signal moved(int value)

    property bool editing: false

    width: parent.width
    height: 28
    opacity: interactive ? 1 : 0.45

    function normalized(value) {
        const stepped = from + Math.round((value - from) / stepSize) * stepSize
        return Math.max(from, Math.min(to, stepped))
    }

    function change(delta) {
        if (interactive)
            moved(normalized(boundValue + delta))
    }

    function beginEdit() {
        if (!interactive)
            return
        editing = true
        editor.text = boundValue.toString()
        Qt.callLater(function() {
            editor.forceActiveFocus()
            editor.selectAll()
        })
    }

    function commitEdit() {
        if (!editing)
            return
        const parsed = Number(editor.text.trim())
        editing = false
        if (!isNaN(parsed))
            moved(normalized(parsed))
    }

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: Theme.textNormal
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Rectangle {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(142, parent.width * 0.52)
        height: 24
        radius: 5
        color: "#111209"
        border.color: root.editing ? Theme.green : Theme.controlBorder
        border.width: 1

        Rectangle {
            id: minusButton
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 34
            radius: 5
            color: minusHover.containsMouse ? Theme.controlHover : Theme.controlBg

            Text {
                anchors.centerIn: parent
                text: "\u2212"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            MouseArea {
                id: minusHover
                anchors.fill: parent
                enabled: root.interactive
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.change(-root.stepSize)
            }
        }

        Rectangle {
            id: plusButton
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 34
            radius: 5
            color: plusHover.containsMouse ? Theme.greenBg : "transparent"
            border.color: plusHover.containsMouse ? Theme.greenBorder : "transparent"
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: "+"
                color: Theme.greenText
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            MouseArea {
                id: plusHover
                anchors.fill: parent
                enabled: root.interactive
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.change(root.stepSize)
            }
        }

        Text {
            anchors.left: minusButton.right
            anchors.right: plusButton.left
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.editing
            horizontalAlignment: Text.AlignHCenter
            text: root.boundValue + root.suffix
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: 9
            font.weight: Font.DemiBold
        }

        TextInput {
            id: editor
            anchors.left: minusButton.right
            anchors.right: plusButton.left
            anchors.verticalCenter: parent.verticalCenter
            visible: root.editing
            color: Theme.textPrimary
            selectionColor: Theme.greenBorder
            selectedTextColor: Theme.textPrimary
            horizontalAlignment: TextInput.AlignHCenter
            inputMethodHints: Qt.ImhDigitsOnly
            font.family: Theme.fontMono
            font.pixelSize: 9
            onEditingFinished: root.commitEdit()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape) {
                    root.editing = false
                    event.accepted = true
                }
            }
        }

        MouseArea {
            anchors.left: minusButton.right
            anchors.right: plusButton.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            enabled: root.interactive && !root.editing
            cursorShape: Qt.IBeamCursor
            onClicked: root.beginEdit()
        }
    }
}
