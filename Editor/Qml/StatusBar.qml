import QtQuick

// Barra de status do editor. `statusModel` (StatusBridge) vem por context property:
// esquerda = mensagem transiente; direita = stats por frame (FPS/Frame/meshes/ocean).
Rectangle {
    id: bar
    color: "#141511"
    implicitWidth: 800
    implicitHeight: 22

    // fio sutil no topo, separando do viewport
    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1; color: "#24251f" }

    // ---- Mensagem (esquerda) ----
    Text {
        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter
                  right: stats.left; rightMargin: 12 }
        text: statusModel.message
        color: "#9a958a"
        font.pixelSize: 11
        elide: Text.ElideRight
    }

    // ---- Stats (direita) ----
    Text {
        id: stats
        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
        color: "#b9b5aa"
        font.family: "Consolas"
        font.pixelSize: 11
        text: {
            var s = "FPS: " + statusModel.fps.toFixed(1)
                  + "   |   Frame: " + statusModel.frameMs.toFixed(2) + " ms"
            if (statusModel.sceneText !== "") s += "   |   " + statusModel.sceneText
            if (statusModel.oceanText !== "") s += "   |   " + statusModel.oceanText
            return s
        }
    }
}
