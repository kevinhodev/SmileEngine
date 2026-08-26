pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import ".." as UI

// Seletor de preset compartilhado pelos componentes de ambiente do Inspector.
Item {
    id: root

    property var options: []
    property int currentIndex: 0

    signal activated(int index)

    width: parent ? parent.width : 0
    height: 32

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: "Preset"
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }

    ComboBox {
        id: combo
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(182, parent.width * 0.62)
        height: Theme.controlHeight
        model: root.options
        currentIndex: root.currentIndex
        onActivated: index => root.activated(index)
        font.family: Theme.fontFamily
        font.pixelSize: 10

        contentItem: Text {
            leftPadding: 10
            rightPadding: 24
            verticalAlignment: Text.AlignVCenter
            text: combo.displayText
            color: Theme.textPrimary
            font: combo.font
            elide: Text.ElideRight
        }
        indicator: UI.LucideIcon {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-down"
            size: 11
            color: Theme.textSecondary
        }
        background: Rectangle {
            radius: Theme.radiusControl
            color: combo.hovered ? Theme.surfaceRaised : Theme.surface
            border.color: combo.activeFocus ? Theme.accentBorder : Theme.controlBorder
            border.width: 1
        }
        popup: Popup {
            y: combo.height + 3
            width: combo.width
            implicitHeight: contentItem.implicitHeight + 8
            padding: 4
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
            }
            background: Rectangle {
                radius: Theme.radiusCard
                color: Theme.surfaceRaised
                border.color: Theme.controlBorder
                border.width: 1
            }
        }
        delegate: ItemDelegate {
            id: option
            required property int index
            required property var modelData

            width: combo.width - 8
            height: 26
            highlighted: combo.highlightedIndex === index
            contentItem: Text {
                text: option.modelData
                color: option.highlighted ? Theme.textPrimary : Theme.textNormal
                font.family: Theme.fontFamily
                font.pixelSize: 10
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusChip
                color: option.highlighted ? Theme.accentBg : "transparent"
            }
        }
    }
}
