import QtQuick

// Faixa de aba de um dock (Cartao 18%): a aba do painel e um bloco da cor do corpo, ligado a
// ele sem fio por baixo; o resto da faixa fica no chrome. `focused` acende o traco accent em
// cima da aba — e o unico sinal de "este painel recebe o teclado".
Item {
    id: root

    property string title
    property string detail
    property bool focused: false
    property Component actions

    height: Theme.tabHeight

    Rectangle { anchors.fill: parent; color: Theme.chrome }
    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Theme.hairlineStrong
    }

    Rectangle {
        id: tab
        width: titleText.implicitWidth + 28
        height: parent.height
        color: Theme.shell
        Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: Theme.hairlineStrong }
        Rectangle {
            visible: root.focused
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 2
            color: Theme.accent
        }
        Text {
            id: titleText
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSection
            font.weight: Font.DemiBold
            renderType: Text.QtRendering
            renderTypeQuality: Text.HighRenderTypeQuality
        }
    }

    Text {
        visible: root.detail !== ""
        anchors.left: tab.right
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.detail
        color: Theme.textMuted
        font.family: Theme.fontNum
        font.pixelSize: Theme.textMeta
        renderType: Text.QtRendering
        renderTypeQuality: Text.HighRenderTypeQuality
    }

    Loader {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        sourceComponent: root.actions
    }
}
