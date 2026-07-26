import QtQuick

// Switch on/off no estilo do editor.
Item {
    id: toggle
    property bool checked: false
    property bool interactive: true
    signal toggled()
    implicitWidth: 36
    implicitHeight: 20
    opacity: interactive ? 1.0 : 0.48
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: toggle.checked ? Theme.blue : Theme.controlBg
        border.color: toggle.checked ? Theme.blue : Theme.controlBorder
        border.width: 1
        Rectangle {
            width: 14; height: 14; radius: 7
            y: 3
            x: toggle.checked ? 19 : 3
            color: toggle.checked ? Theme.knob : Theme.textMuted
            Behavior on x { NumberAnimation { duration: 100 } }
        }
    }
    HoverHandler { cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: toggle.interactive; onTapped: toggle.toggled() }
}
