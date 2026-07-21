import QtQuick
import QtQuick.Controls
import "components" as C

// Janela flutuante de Estatisticas (menu Janela > Estatísticas). Liga em `viewportModel`
// (ViewportWidget) e `statsWindow` (WindowBridge da QDialog frameless). Foco: consumo de
// VRAM — uso/budget do processo (DXGI QueryVideoMemoryInfo, cache de 500ms na engine) e
// breakdown por categoria (VramTracker, estilo Flax). Mesmo chrome do TimeOfDayWindow.
Rectangle {
    id: root
    color: "#141511"
    border.color: "#2e2f28"
    border.width: 1
    implicitWidth: 420
    implicitHeight: 680

    readonly property color cardBg: C.Theme.cardBg
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color green: C.Theme.green
    readonly property color warn: C.Theme.warn

    // ---- Componentes compartilhados (Editor/Qml/components) ----
    component WindowButton: C.WindowButton {}

    // ---- Barra de titulo (drag + minimizar/fechar) ----
    Item {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 40

        Text {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "▥"
            color: root.blue
            font.pixelSize: 14
        }
        Text {
            id: titleText
            x: 40
            anchors.verticalCenter: parent.verticalCenter
            text: "Estatísticas"
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 13
            font.weight: Font.Medium
        }
        Text {
            anchors.left: titleText.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: viewportModel.gpuName
            color: root.textMuted
            font.family: "Segoe UI"
            font.pixelSize: 10
            elide: Text.ElideRight
            width: Math.max(0, windowButtons.x - x - 8)
        }

        MouseArea {
            anchors.left: parent.left
            anchors.right: windowButtons.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            property bool moving: false
            onPressed: moving = false
            onPositionChanged: {
                if (pressed && !moving) {
                    moving = true
                    statsWindow.startSystemMove()
                }
            }
            onReleased: moving = false
        }

        Row {
            id: windowButtons
            anchors.right: parent.right
            anchors.top: parent.top
            WindowButton { glyph: "−"; onTapped: statsWindow.minimize() }
            WindowButton { glyph: "×"; danger: true; onTapped: statsWindow.close() }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.divider
        }
    }

    Flickable {
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        anchors.topMargin: 10
        contentHeight: contentCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: contentCol
            width: parent.width
            spacing: 10

            // ---- Hero: uso vs budget do OS ----
            Rectangle {
                width: parent.width
                height: 128
                radius: 8
                color: root.cardBg
                border.color: root.borderColor

                Text {
                    x: 16; y: 14
                    text: "VRAM do processo"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }
                Text {
                    x: 16; y: 40
                    text: viewportModel.vramUsageText
                    color: viewportModel.vramOverBudget ? root.warn : root.blue
                    font.family: "Segoe UI"
                    font.pixelSize: 22
                    font.weight: Font.Medium
                }
                // Barra uso/budget: verde -> laranja perto do teto (>85% ja e zona de risco;
                // acima do budget o OS comeca a despejar recurso pra RAM = stutter).
                Rectangle {
                    x: 16; y: 82
                    width: parent.width - 32
                    height: 8
                    radius: 4
                    color: "#23241d"
                    Rectangle {
                        width: parent.width * viewportModel.vramBudgetFrac
                        height: parent.height
                        radius: 4
                        color: viewportModel.vramOverBudget ? root.warn
                             : viewportModel.vramBudgetFrac > 0.85 ? root.warn : root.green
                        Behavior on width { NumberAnimation { duration: 250 } }
                    }
                }
                Text {
                    x: 16; y: 98
                    text: viewportModel.vramOverBudget
                          ? "ACIMA do budget do OS — recursos podem ser despejados pra RAM"
                          : "budget dinâmico do OS · placa com " + viewportModel.vramText + " dedicados"
                    color: viewportModel.vramOverBudget ? root.warn : root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }
            }

            // ---- GPU por passe (FGpuProfiler: timestamps, EMA, lido 2 frames depois) ----
            Rectangle {
                width: parent.width
                height: gpuCol.implicitHeight + 56
                radius: 8
                color: root.cardBg
                border.color: root.borderColor

                Text {
                    x: 16; y: 14
                    text: "GPU por passe"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: 16
                    text: "frame: " + viewportModel.gpuFrameText
                    color: root.textSecondary
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }

                Column {
                    id: gpuCol
                    x: 16; y: 42
                    width: parent.width - 32
                    spacing: 6

                    Repeater {
                        model: viewportModel.gpuTimings
                        delegate: Item {
                            required property var modelData
                            width: gpuCol.width
                            height: 26

                            Text {
                                anchors.left: parent.left
                                y: 0
                                text: modelData.name
                                color: root.textNormal
                                font.family: "Segoe UI"
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.right: parent.right
                                y: 0
                                text: modelData.text
                                color: root.textNormal
                                font.family: "Segoe UI"
                                font.pixelSize: 11
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 3
                                height: 4
                                radius: 2
                                color: "#23241d"
                                Rectangle {
                                    width: parent.width * modelData.frac
                                    height: parent.height
                                    radius: 2
                                    color: root.green
                                }
                            }
                        }
                    }

                    Text {
                        visible: viewportModel.gpuTimings.length === 0
                        text: "Coletando timestamps…"
                        color: root.textMuted
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                }
            }

            // ---- Breakdown por categoria (VramTracker) ----
            Rectangle {
                width: parent.width
                height: catCol.implicitHeight + 56
                radius: 8
                color: root.cardBg
                border.color: root.borderColor

                Text {
                    x: 16; y: 14
                    text: "Por categoria"
                    color: root.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: 16
                    text: "% do uso total"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                }

                Column {
                    id: catCol
                    x: 16; y: 42
                    width: parent.width - 32
                    spacing: 6

                    Repeater {
                        model: viewportModel.vramBreakdown
                        delegate: Item {
                            required property var modelData
                            width: catCol.width
                            height: 26

                            Text {
                                anchors.left: parent.left
                                y: 0
                                text: modelData.name
                                color: modelData.name === "Não rastreado" ? root.textMuted
                                                                          : root.textNormal
                                font.family: "Segoe UI"
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.right: parent.right
                                y: 0
                                text: modelData.text
                                color: root.textNormal
                                font.family: "Segoe UI"
                                font.pixelSize: 11
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 3
                                height: 4
                                radius: 2
                                color: "#23241d"
                                Rectangle {
                                    width: parent.width * modelData.frac
                                    height: parent.height
                                    radius: 2
                                    color: modelData.name === "Não rastreado" ? "#3a3b32"
                                                                              : root.blue
                                }
                            }
                        }
                    }

                    Text {
                        visible: viewportModel.vramBreakdown.length === 0
                        text: "Sem dados ainda — carregue uma cena."
                        color: root.textMuted
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                    }
                }
            }

            // ---- Rodape: segmento non-local (RAM compartilhada visivel pela GPU) ----
            Rectangle {
                width: parent.width
                height: 52
                radius: 8
                color: root.cardBg
                border.color: root.borderColor

                Text {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "RAM compartilhada (non-local)"
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: viewportModel.vramNonLocalText
                    color: root.textNormal
                    font.family: "Segoe UI"
                    font.pixelSize: 11
                }
            }
        }
    }
}
