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
    height: 38

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 8
        height: 1
        color: Theme.divider
    }

    UI.LucideIcon {
        x: 0
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 5
        name: "chevron-right"
        size: 11
        rotation: root.expanded ? 90 : 0
        color: root.expanded ? Theme.textSecondary : Theme.textMuted
        Behavior on rotation { NumberAnimation { duration: 100 } }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.right: summaryText.left
        anchors.rightMargin: 8
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 4
        text: root.label.toUpperCase()
        color: Theme.textSecondary
        font.family: Theme.fontMono
        font.pixelSize: 10
        font.letterSpacing: 1.2
        font.weight: Font.DemiBold
        elide: Text.ElideRight
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }

    Text {
        id: summaryText
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 5
        width: parent.width * 0.48
        horizontalAlignment: Text.AlignRight
        text: root.summary
        color: Theme.textMuted
        font.family: Theme.fontMono
        font.pixelSize: 8
        elide: Text.ElideLeft
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }

    HoverHandler { cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.toggled() }
}
