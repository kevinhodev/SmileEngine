pragma ComponentBehavior: Bound

import QtQuick
import "components" as C

// Rodape permanente da janela principal. Nao pertence a nenhum dock: fechar console, outliner
// ou inspector libera espaco para o viewport sem esconder os instrumentos de desempenho.
Rectangle {
    id: root

    required property var statusModel
    required property var statsModel

    color: C.Theme.chrome
    implicitWidth: 700
    implicitHeight: 26

    component Metric: Row {
        id: metric

        property string label
        property string value
        spacing: 7

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: metric.label
            color: C.Theme.textMuted
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textBody
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: metric.value
            color: C.Theme.textPrimary
            font.family: C.Theme.fontNum
            font.pixelSize: C.Theme.textBody
            font.weight: Font.Medium
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: C.Theme.hairlineStrong
    }

    // Estado. Verde = pronto; com mensagem transiente, ela assume a leitura e o ponto vira ambar.
    Row {
        id: statusRow

        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.right: metricsRow.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: root.statusModel.message !== "" ? C.Theme.amber : C.Theme.green
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, statusRow.width - 16)
            text: root.statusModel.message !== "" ? root.statusModel.message : "Pronto"
            color: C.Theme.textSecondary
            font.family: C.Theme.fontFamily
            font.pixelSize: C.Theme.textBody
            elide: Text.ElideRight
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }

    Row {
        id: metricsRow

        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 24

        // Locale do sistema nos dois lados: gpuFrameText e vramUsageText ja chegam formatados.
        Metric {
            label: "FPS"
            value: root.statsModel.fps.toLocaleString(Qt.locale(), 'f', 1)
        }
        Metric {
            label: "Frame"
            value: root.statsModel.frameTimeMs.toLocaleString(Qt.locale(), 'f', 2) + " ms"
        }
        Metric {
            visible: root.width > 1100 && root.statsModel.gpuFrameText !== ""
            label: "GPU"
            value: root.statsModel.gpuFrameText
        }
        Metric {
            visible: root.width > 1280
            label: "Lista CPU"
            value: root.statsModel.visibleDrawCount + "/" + root.statsModel.totalDrawCount
        }
        Metric {
            visible: root.width > 1440
            label: "VRAM"
            value: root.statsModel.vramUsageText
        }
    }
}
