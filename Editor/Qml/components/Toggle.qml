import QtQuick

// Switch on/off no estilo do editor.
Item {
    id: toggle
    property bool checked: false
    property bool interactive: true
    signal toggled()
    implicitWidth: 30
    implicitHeight: 16
    opacity: interactive ? 1.0 : 0.48
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: toggle.checked ? Theme.greenBorder : Theme.controlBg
        border.color: toggle.checked ? Theme.green : Theme.controlBorder
        border.width: 1
        Rectangle {
            width: 10
            height: width
            radius: width / 2
            y: (parent.height - height) / 2
            x: toggle.checked ? parent.width - width - y : y
            color: toggle.checked ? Theme.knob : Theme.textMuted
            Behavior on x { NumberAnimation { duration: 100 } }
        }
    }
    HoverHandler { cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: toggle.interactive; onTapped: toggle.toggled() }
}
