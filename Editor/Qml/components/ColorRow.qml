import QtQuick

// Linha de cor com picker HSV expansivel (quadrado S/V + barra de matiz) — porte do
// picker das luzes do Outliner. `value` re-sincroniza do modelo quando nao ha arraste.
Item {
    id: colorRow
    property string label
    property color value: "#ffffff"
    property bool open: false
    property bool interactive: true
    signal pushed(color c)
    width: parent.width
    height: open ? 176 : 24
    clip: true
    opacity: interactive ? 1.0 : 0.48
    Behavior on height { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }

    property real hue: 0
    property real sat: 0
    property real val: 1
    property bool syncing: false

    function syncFromValue() {
        syncing = true
        hue = value.hsvHue >= 0 ? value.hsvHue : hue // -1 = acromatico: preserva o matiz
        sat = value.hsvSaturation
        val = value.hsvValue
        syncing = false
    }
    function push() {
        if (!syncing) colorRow.pushed(Qt.hsva(hue, sat, val, 1.0))
    }
    onValueChanged: if (!svDrag.active && !hueDrag.active) syncFromValue()
    Component.onCompleted: syncFromValue()

    Text {
        y: 4
        text: colorRow.label
        color: Theme.textNormal
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }
    Text {
        anchors.right: swatch.left
        anchors.rightMargin: 10
        y: 5
        text: String(colorRow.value).toUpperCase()
        color: Theme.textMuted
        font.family: "Consolas"
        font.pixelSize: 10
    }
    Rectangle {
        id: swatch
        anchors.right: parent.right
        y: 2
        width: 46; height: 18; radius: 4
        color: colorRow.value
        border.color: swHover.hovered ? Theme.blue : Theme.borderColor
        border.width: 1
        HoverHandler { id: swHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            enabled: colorRow.interactive
            onTapped: { if (!colorRow.open) colorRow.syncFromValue(); colorRow.open = !colorRow.open }
        }
    }

    // quadrado SV
    Rectangle {
        id: svBox
        x: 0; y: 30
        width: parent.width - 34
        height: 110
        radius: 6
        clip: true
        visible: colorRow.open
        border.color: Theme.borderColor
        border.width: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: Qt.hsva(colorRow.hue, 1, 1, 1) }
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "#000000" }
            }
        }
        Rectangle {
            x: colorRow.sat * (svBox.width - 1) - width / 2
            y: (1 - colorRow.val) * (svBox.height - 1) - height / 2
            width: 13; height: 13; radius: 6.5
            color: "transparent"
            border.color: colorRow.val > 0.55 && colorRow.sat < 0.5 ? "#00000090" : "#ffffffd0"
            border.width: 2
        }
        MouseArea {
            id: svDrag
            property bool active: pressed
            anchors.fill: parent
            cursorShape: Qt.CrossCursor
            function apply(mx, my) {
                colorRow.sat = Math.max(0, Math.min(1, mx / (width - 1)))
                colorRow.val = 1 - Math.max(0, Math.min(1, my / (height - 1)))
                colorRow.push()
            }
            onPressed: (e) => apply(e.x, e.y)
            onPositionChanged: (e) => { if (pressed) apply(e.x, e.y) }
        }
    }

    // barra de matiz
    Rectangle {
        id: hueBar
        anchors.right: parent.right
        y: 30
        width: 22
        height: 110
        radius: 6
        clip: true
        visible: colorRow.open
        border.color: Theme.borderColor
        border.width: 1
        gradient: Gradient {
            GradientStop { position: 0.000; color: "#ff0000" }
            GradientStop { position: 0.167; color: "#ffff00" }
            GradientStop { position: 0.333; color: "#00ff00" }
            GradientStop { position: 0.500; color: "#00ffff" }
            GradientStop { position: 0.667; color: "#0000ff" }
            GradientStop { position: 0.833; color: "#ff00ff" }
            GradientStop { position: 1.000; color: "#ff0000" }
        }
        Rectangle {
            x: 1
            y: colorRow.hue * (hueBar.height - 5) + 1
            width: parent.width - 2
            height: 4
            radius: 2
            color: "transparent"
            border.color: "#ffffffd0"
            border.width: 1.5
        }
        MouseArea {
            id: hueDrag
            property bool active: pressed
            anchors.fill: parent
            function apply(my) {
                colorRow.hue = Math.max(0, Math.min(1, my / (height - 1)))
                colorRow.push()
            }
            onPressed: (e) => apply(e.y)
            onPositionChanged: (e) => { if (pressed) apply(e.y) }
        }
    }
    Text {
        y: 148
        visible: colorRow.open
        text: "arraste no quadrado (S/V) e na barra (matiz)"
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }
}
