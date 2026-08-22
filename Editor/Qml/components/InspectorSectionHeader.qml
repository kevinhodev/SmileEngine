pragma ComponentBehavior: Bound

import QtQuick
import ".." as UI

Item {
    id: root

    property string label
    property string summary
    property bool expanded: false

    signal toggled()

    width: parent.width
    height: 31

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.divider
    }

    UI.LucideIcon {
        x: 0
        y: 10
        name: "chevron-right"
        size: 11
        rotation: root.expanded ? 90 : 0
        color: Theme.textSecondary
        Behavior on rotation { NumberAnimation { duration: 100 } }
    }

    Text {
        x: 18
        y: 8
        text: root.label
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: 10
        font.weight: Font.DemiBold
    }

    Text {
        anchors.right: parent.right
        y: 9
        width: parent.width * 0.48
        horizontalAlignment: Text.AlignRight
        text: root.summary
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 8
        elide: Text.ElideLeft
    }

    HoverHandler { cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.toggled() }
}
