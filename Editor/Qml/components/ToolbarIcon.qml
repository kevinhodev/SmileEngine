import QtQuick
import QtQuick.Effects

// Icone da toolbar do viewport. Era um Canvas com os desenhos redigitados a mao; agora le os
// mesmos SVGs de Editor/Qml/icons que o resto do editor usa, que sao os desenhos do conceito
// Signal Graphite exportados da referencia. Duas iconografias para a mesma UI nao se
// mantinham em sincronia — o traco do Canvas ja estava mais grosso e as formas divergiam.
//
// O tipo continua existindo (em vez de virar LucideIcon direto) porque a toolbar dimensiona
// por width/height e nao por `size`, e porque ele mora em components/, onde os consumidores
// nao importam a raiz de Qml/.
Item {
    id: root
    property string name: ""
    property color color: Theme.textSecondary

    implicitWidth: 14
    implicitHeight: 14

    Image {
        id: src
        anchors.fill: parent
        source: root.name ? Qt.resolvedUrl("../icons/" + root.name + ".svg") : ""
        // 2x da caixa real: os icones da toolbar sao pequenos (14 px) e o SVG rasteriza
        // uma vez no sourceSize, nao no tamanho do item.
        sourceSize: Qt.size(Math.round(root.width * 2), Math.round(root.height * 2))
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
