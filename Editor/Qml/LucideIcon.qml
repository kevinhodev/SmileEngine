import QtQuick
import QtQuick.Effects

// Icone Lucide tintavel. Os SVGs (Editor/Qml/icons/<name>.svg) tem stroke branco; o MultiEffect
// recolore pra `color` em runtime, entao o mesmo arquivo serve cinza/azul/hover/disabled so
// mudando a cor. `name` e o nome do arquivo sem extensao.
Item {
    id: root
    property string name
    property color color: "#c8c2b4"
    property int size: 16

    implicitWidth: size
    implicitHeight: size

    Image {
        id: src
        anchors.fill: parent
        source: root.name ? Qt.resolvedUrl("icons/" + root.name + ".svg") : ""
        sourceSize: Qt.size(root.size * 2, root.size * 2) // 2x p/ nitidez em telas hi-dpi
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        visible: false // so o MultiEffect e desenhado
    }

    MultiEffect {
        anchors.fill: src
        source: src
        colorization: 1.0
        colorizationColor: root.color
        Behavior on colorizationColor { ColorAnimation { duration: 120 } }
    }
}
