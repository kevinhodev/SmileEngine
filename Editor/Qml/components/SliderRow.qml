import QtQuick
import QtQuick.Controls

// Linha de card: label + valor + slider fino. `boundValue` re-sincroniza do modelo
// quando o usuario NAO esta arrastando (Binding com when: !pressed).
Item {
    id: row
    property string label
    property string valueText
    property real from: 0
    property real to: 1
    property real stepSize: 0
    property real boundValue: 0
    property bool interactive: true
    signal moved(real v)
    width: parent.width
    height: 38
    opacity: interactive ? 1.0 : 0.48

    Text {
        x: 0; y: 0
        text: row.label
        color: Theme.textNormal
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }
    Text {
        anchors.right: parent.right; y: 0
        text: row.valueText
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }
    Slider {
        id: sl
        x: 0; y: 16
        width: parent.width
        height: 18
        enabled: row.interactive
        from: row.from
        to: row.to
        stepSize: row.stepSize
        onMoved: row.moved(value)
        Binding {
            target: sl; property: "value"
            value: row.boundValue
            when: !sl.pressed
            restoreMode: Binding.RestoreBinding
        }
        background: Rectangle {
            x: sl.leftPadding
            y: sl.topPadding + sl.availableHeight / 2 - height / 2
            width: sl.availableWidth
            height: 4; radius: 2
            color: Theme.controlBg
            Rectangle {
                width: sl.visualPosition * parent.width
                height: parent.height; radius: 2
                color: Theme.blue
            }
        }
        handle: Rectangle {
            x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
            y: sl.topPadding + sl.availableHeight / 2 - height / 2
            width: 12; height: 12; radius: 6
            color: Theme.knob
            border.color: Theme.blue
            border.width: 1.5
        }
    }
}
