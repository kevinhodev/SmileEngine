import QtQuick
import QtQuick.Controls as QQC

// Tooltip compartilhado do editor. Uso:
//   ToolTip {
//       active: hover.hovered
//       text: "Enquadrar seleção"
//       shortcut: "F"
//   }
//
// Por padrão abre abaixo e centralizado no item pai. `above` inverte o lado;
// horizontalOffset permite o último ajuste sem duplicar posicionamento nos consumidores.
QQC.ToolTip {
    id: tip

    property bool active: false
    property bool above: false
    property string shortcut: ""
    property int showDelay: 250
    property int gap: 7
    property real horizontalOffset: 0

    visible: active && text.length > 0
    delay: showDelay
    timeout: -1
    popupType: QQC.Popup.Window
    closePolicy: QQC.Popup.NoAutoClose
    margins: 8

    x: Math.round((parent ? (parent.width - width) / 2 : 0) + horizontalOffset)
    y: above ? -height - gap : (parent ? parent.height + gap : 0)

    leftPadding: 10
    rightPadding: 8
    topPadding: 6
    bottomPadding: 6

    contentItem: Row {
        spacing: tip.shortcut !== "" ? 9 : 0

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: tip.text
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: 10
            font.weight: Font.Medium
            renderType: Text.NativeRendering
        }

        Rectangle {
            visible: tip.shortcut !== ""
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: shortcutLabel.implicitWidth + 10
            implicitHeight: 17
            radius: 4
            color: "#171d25"
            border.color: Theme.blueBorder
            border.width: 1

            Text {
                id: shortcutLabel
                anchors.centerIn: parent
                text: tip.shortcut
                color: "#8dbbff"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
                renderType: Text.NativeRendering
            }
        }
    }

    background: Item {
        implicitWidth: 40
        implicitHeight: 28

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 2
            radius: 6
            color: "#050605"
            opacity: 0.55
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            color: "#20221c"
            border.color: "#3b3d34"
            border.width: 1
        }
        Rectangle {
            x: 1
            y: 7
            width: 2
            height: parent.height - 14
            radius: 1
            color: Theme.blue
            opacity: 0.9
        }
    }

    transformOrigin: Item.Top
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 100; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 120; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 70 }
        }
    }
}
