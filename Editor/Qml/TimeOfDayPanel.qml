import QtQuick
import QtQuick.Controls

// Painel Time of Day (dock lateral). Liga em `todModel` (TimeOfDayBridge): relogio, geografia,
// lua/estrelas e sol manual, com um arco do ceu (trajetorias de sol e lua do dia corrente)
// desenhado em Canvas. Estilo do SettingsWindow (dark, cards, accent azul).
Rectangle {
    id: root
    color: "#141511"

    readonly property color cardBg: "#1a1b15"
    readonly property color borderColor: "#2a2b24"
    readonly property color divider: "#23241d"
    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textSecondary: "#9a958a"
    readonly property color textMuted: "#6c6a61"
    readonly property color blue: "#5b9dff"
    readonly property color sunColor: "#d8a03a"
    readonly property color moonColor: "#8fa9d8"

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }

    function timeLabel(h) {
        var hh = Math.floor(h)
        var mm = Math.floor((h - hh) * 60)
        return (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
    }

    function doyLabel(d) {
        const days = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
        const names = ["jan", "fev", "mar", "abr", "mai", "jun",
                       "jul", "ago", "set", "out", "nov", "dez"]
        var rest = d
        for (var i = 0; i < 12; ++i) {
            if (rest <= days[i]) return rest + " " + names[i]
            rest -= days[i]
        }
        return "31 dez"
    }

    function phaseName(offsetH) {
        var o = ((offsetH % 24) + 24) % 24
        if (o < 3 || o >= 21) return "Nova"
        if (o < 9)  return "Crescente"
        if (o < 15) return "Cheia"
        return "Minguante"
    }

    // ---- Componentes locais (mesma linguagem do SettingsWindow) ----
    component Toggle: Item {
        id: toggle
        property bool checked: false
        property bool interactive: true
        signal toggled()
        implicitWidth: 36
        implicitHeight: 20
        opacity: interactive ? 1.0 : 0.48
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: toggle.checked ? root.blue : "#23241d"
            border.color: toggle.checked ? root.blue : "#33342c"
            border.width: 1
            Rectangle {
                width: 14; height: 14; radius: 7
                y: 3
                x: toggle.checked ? 19 : 3
                color: toggle.checked ? "#f2efe6" : root.textMuted
                Behavior on x { NumberAnimation { duration: 100 } }
            }
        }
        HoverHandler { cursorShape: toggle.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
        TapHandler { enabled: toggle.interactive; onTapped: toggle.toggled() }
    }

    component Card: Rectangle {
        default property alias content: inner.data
        property string title
        property string iconName
        property alias headerItem: headerSlot.data
        width: parent.width
        radius: 8
        color: root.cardBg
        border.color: root.borderColor
        border.width: 1
        implicitHeight: 40 + inner.implicitHeight + 14

        LucideIcon {
            visible: parent.iconName !== ""
            x: 14; y: 13
            name: parent.iconName
            size: 14
            color: root.textSecondary
        }
        Text {
            x: parent.iconName !== "" ? 36 : 16; y: 12
            text: parent.title
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Item {
            id: headerSlot
            anchors.right: parent.right
            anchors.rightMargin: 14
            y: 10
            width: childrenRect.width
            height: 20
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            y: 36; height: 1
            color: root.divider
        }
        Column {
            id: inner
            x: 14; y: 44
            width: parent.width - 28
            spacing: 10
        }
    }

    // Linha com label + valor + slider fino. `live: true` escreve enquanto arrasta (o TOD e
    // barato de atualizar e o feedback no viewport e o ponto do painel).
    component SliderRow: Item {
        id: row
        property string label
        property string valueText
        property real from: 0
        property real to: 1
        property real stepSize: 0
        property real boundValue: 0
        signal moved(real v)
        width: parent.width
        height: 38

        Text {
            x: 0; y: 0
            text: row.label
            color: root.textNormal
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Text {
            anchors.right: parent.right; y: 0
            text: row.valueText
            color: root.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 11
        }
        Slider {
            id: sl
            x: 0; y: 16
            width: parent.width
            height: 18
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
                color: "#23241d"
                Rectangle {
                    width: sl.visualPosition * parent.width
                    height: parent.height; radius: 2
                    color: root.blue
                }
            }
            handle: Rectangle {
                x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: 12; height: 12; radius: 6
                color: "#f2efe6"
                border.color: root.blue
                border.width: 1.5
            }
        }
    }

    // ---- Header do painel ----
    Rectangle {
        id: header
        anchors.left: parent.left; anchors.right: parent.right
        height: 34
        color: "#10110f"

        LucideIcon {
            x: 12; anchors.verticalCenter: parent.verticalCenter
            name: "sun"; size: 15
            color: todModel.enabled ? root.sunColor : root.textMuted
        }
        Text {
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            text: "Time of Day"
            color: root.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Toggle {
            anchors.right: closeBtn.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            checked: todModel.enabled
            onToggled: todModel.enabled = !checked
        }
        Item {
            id: closeBtn
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24
            Rectangle {
                anchors.fill: parent; radius: 5
                color: closeHover.hovered ? "#23241d" : "transparent"
            }
            LucideIcon {
                anchors.centerIn: parent
                name: "x"; size: 14
                color: closeHover.hovered ? root.textPrimary : root.textMuted
            }
            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: todModel.closePanel() }
        }
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1; color: root.divider
        }
    }

    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        anchors.topMargin: 10
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: col
            width: parent.width
            spacing: 10
            opacity: todModel.available ? 1.0 : 0.45

            // ---- Arco do ceu ----
            Rectangle {
                width: parent.width
                height: 148
                radius: 8
                color: root.cardBg
                border.color: root.borderColor
                border.width: 1
                clip: true

                Canvas {
                    id: arc
                    anchors.fill: parent
                    anchors.margins: 1

                    // elevacao [-70, +80] graus -> y do canvas
                    function elToY(el) {
                        var top = 10, bottom = height - 24
                        return bottom - ((el + 70) / 150) * (bottom - top)
                    }

                    onPaint: {
                        var ctx = getContext("2d")
                        var w = width, h = height
                        ctx.reset()

                        var horizonY = elToY(0)

                        // ceu / chao
                        ctx.fillStyle = "#191c22"
                        ctx.fillRect(0, 0, w, horizonY)
                        ctx.fillStyle = "#15150f"
                        ctx.fillRect(0, horizonY, w, h - horizonY)

                        // linha do horizonte
                        ctx.strokeStyle = "rgba(230,226,216,0.18)"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, horizonY); ctx.lineTo(w, horizonY)
                        ctx.stroke()

                        // grade das horas (6/12/18)
                        ctx.strokeStyle = "rgba(230,226,216,0.05)"
                        for (var g = 6; g <= 18; g += 6) {
                            var gx = (g / 24) * w
                            ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, h - 18); ctx.stroke()
                        }

                        // trajetoria da lua (tracejada)
                        ctx.setLineDash([3, 4])
                        ctx.strokeStyle = "rgba(143,169,216,0.55)"
                        ctx.lineWidth = 1.2
                        ctx.beginPath()
                        for (var i = 0; i <= 96; ++i) {
                            var hM = i / 4.0
                            var yM = elToY(todModel.moonElevationAt(hM))
                            if (i === 0) ctx.moveTo((hM / 24) * w, yM)
                            else         ctx.lineTo((hM / 24) * w, yM)
                        }
                        ctx.stroke()
                        ctx.setLineDash([])

                        // trajetoria do sol
                        ctx.strokeStyle = "rgba(216,160,58,0.85)"
                        ctx.lineWidth = 1.6
                        ctx.beginPath()
                        for (var j = 0; j <= 96; ++j) {
                            var hS = j / 4.0
                            var yS = elToY(todModel.sunElevationAt(hS))
                            if (j === 0) ctx.moveTo((hS / 24) * w, yS)
                            else         ctx.lineTo((hS / 24) * w, yS)
                        }
                        ctx.stroke()

                        // linha do agora
                        var nowX = (todModel.timeHours / 24) * w
                        ctx.strokeStyle = "rgba(230,226,216,0.14)"
                        ctx.beginPath(); ctx.moveTo(nowX, 0); ctx.lineTo(nowX, h - 18); ctx.stroke()

                        // marcador da lua
                        var moonEl = todModel.moonElevationDeg
                        var my = elToY(moonEl)
                        ctx.globalAlpha = moonEl > -8 ? 1.0 : 0.35
                        ctx.fillStyle = "#cfd8ea"
                        ctx.beginPath(); ctx.arc(nowX, my, 3.5, 0, 6.2832); ctx.fill()
                        ctx.globalAlpha = 1.0

                        // marcador do sol (halo + disco, quente perto do horizonte)
                        var sunEl = todModel.sunElevationDeg
                        var sy = elToY(sunEl)
                        var t = Math.max(0, Math.min(1, sunEl / 40))
                        var r = Math.round(255 - 30 * t)
                        var gr = Math.round(140 + 80 * t)
                        var b = Math.round(60 + 40 * t)
                        if (sunEl > -12) {
                            var glow = ctx.createRadialGradient(nowX, sy, 1, nowX, sy, 14)
                            glow.addColorStop(0, "rgba(" + r + "," + gr + "," + b + ",0.55)")
                            glow.addColorStop(1, "rgba(" + r + "," + gr + "," + b + ",0)")
                            ctx.fillStyle = glow
                            ctx.beginPath(); ctx.arc(nowX, sy, 14, 0, 6.2832); ctx.fill()
                        }
                        ctx.globalAlpha = sunEl > -12 ? 1.0 : 0.3
                        ctx.fillStyle = "rgb(" + r + "," + gr + "," + b + ")"
                        ctx.beginPath(); ctx.arc(nowX, sy, 5, 0, 6.2832); ctx.fill()
                        ctx.globalAlpha = 1.0

                        // horas no rodape
                        ctx.fillStyle = "rgba(154,149,138,0.7)"
                        ctx.font = "9px Segoe UI"
                        ctx.textAlign = "center"
                        var labels = [0, 6, 12, 18, 24]
                        for (var k = 0; k < labels.length; ++k) {
                            var lx = (labels[k] / 24) * w
                            lx = Math.max(8, Math.min(w - 10, lx))
                            ctx.fillText(labels[k] + "h", lx, h - 6)
                        }
                    }

                    Connections {
                        target: todModel
                        function onTimeChanged()  { arc.requestPaint() }
                        function onStateChanged() { arc.requestPaint() }
                    }
                    onWidthChanged: requestPaint()
                }

                // leitura rapida sol/lua no canto
                Text {
                    x: 8; y: 6
                    text: "sol " + Math.round(todModel.sunElevationDeg) + "°  ·  az " +
                          Math.round(todModel.sunAzimuthDeg) + "°"
                    color: root.textSecondary
                    font.family: "Segoe UI"
                    font.pixelSize: 9
                }
                Text {
                    anchors.right: parent.right; anchors.rightMargin: 8; y: 6
                    text: "lua " + Math.round(todModel.moonElevationDeg) + "°"
                    color: Qt.alpha(root.moonColor, 0.8)
                    font.family: "Segoe UI"
                    font.pixelSize: 9
                }
            }

            // ---- Relogio ----
            Card {
                title: "Relógio"
                iconName: "sun"
                visible: todModel.enabled
                headerItem: [
                    Text {
                        text: root.timeLabel(todModel.timeHours)
                        color: root.textPrimary
                        font.family: "Segoe UI"
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                ]

                Item {
                    width: parent.width
                    height: 34

                    // play/pause
                    Rectangle {
                        id: playBtn
                        width: 30; height: 30; radius: 15
                        y: 2
                        color: todModel.running ? "#16233f" : "#23241d"
                        border.color: todModel.running ? "#27406e" : "#33342c"
                        border.width: 1
                        LucideIcon {
                            anchors.centerIn: parent
                            name: todModel.running ? "pause" : "play"
                            size: 13
                            color: todModel.running ? root.blue : root.textNormal
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: todModel.running = !todModel.running }
                    }

                    // slider da hora com gradiente dia/noite
                    Slider {
                        id: hourSlider
                        anchors.left: playBtn.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        y: 8
                        height: 18
                        from: 0; to: 24
                        onMoved: todModel.timeHours = value
                        Binding {
                            target: hourSlider; property: "value"
                            value: todModel.timeHours
                            when: !hourSlider.pressed
                            restoreMode: Binding.RestoreBinding
                        }
                        background: Rectangle {
                            x: hourSlider.leftPadding
                            y: hourSlider.topPadding + hourSlider.availableHeight / 2 - height / 2
                            width: hourSlider.availableWidth
                            height: 6; radius: 3
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.00; color: "#10131d" }
                                GradientStop { position: 0.22; color: "#241f30" }
                                GradientStop { position: 0.27; color: "#b06038" }
                                GradientStop { position: 0.35; color: "#6f9cc8" }
                                GradientStop { position: 0.50; color: "#8fb8dd" }
                                GradientStop { position: 0.65; color: "#6f9cc8" }
                                GradientStop { position: 0.73; color: "#b06038" }
                                GradientStop { position: 0.78; color: "#241f30" }
                                GradientStop { position: 1.00; color: "#10131d" }
                            }
                        }
                        handle: Rectangle {
                            x: hourSlider.leftPadding +
                               hourSlider.visualPosition * (hourSlider.availableWidth - width)
                            y: hourSlider.topPadding + hourSlider.availableHeight / 2 - height / 2
                            width: 14; height: 14; radius: 7
                            color: "#f2efe6"
                            border.color: "#8a8578"
                            border.width: 1
                        }
                    }
                }

                SliderRow {
                    label: "Duração do dia"
                    valueText: root.fmt(todModel.dayLengthSec / 60) + " min"
                    from: 0.5; to: 30; stepSize: 0.5
                    boundValue: todModel.dayLengthSec / 60
                    onMoved: v => todModel.dayLengthSec = v * 60
                }
            }

            // ---- Sol manual (TOD desligado) ----
            Card {
                title: "Sol manual"
                iconName: "sun"
                visible: !todModel.enabled

                Text {
                    width: parent.width
                    text: "O relógio está desligado — o sol é posicionado direto."
                    color: root.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
                SliderRow {
                    label: "Elevação"
                    valueText: Math.round(todModel.manualElevationDeg) + "°"
                    from: -20; to: 90
                    boundValue: todModel.manualElevationDeg
                    onMoved: v => todModel.manualElevationDeg = v
                }
                SliderRow {
                    label: "Azimute"
                    valueText: Math.round(todModel.manualAzimuthDeg) + "°"
                    from: 0; to: 360
                    boundValue: todModel.manualAzimuthDeg
                    onMoved: v => todModel.manualAzimuthDeg = v
                }
            }

            // ---- Geografia ----
            Card {
                title: "Geografia"
                iconName: "pin"

                SliderRow {
                    label: "Latitude"
                    valueText: Math.abs(Math.round(todModel.latitudeDeg)) + "° " +
                               (todModel.latitudeDeg >= 0 ? "N" : "S")
                    from: -66; to: 66
                    boundValue: todModel.latitudeDeg
                    onMoved: v => todModel.latitudeDeg = v
                }
                SliderRow {
                    label: "Dia do ano"
                    valueText: root.doyLabel(todModel.dayOfYear)
                    from: 1; to: 365; stepSize: 1
                    boundValue: todModel.dayOfYear
                    onMoved: v => todModel.dayOfYear = Math.round(v)
                }
                SliderRow {
                    label: "Norte da cena"
                    valueText: Math.round(todModel.northOffsetDeg) + "°"
                    from: 0; to: 360
                    boundValue: todModel.northOffsetDeg
                    onMoved: v => todModel.northOffsetDeg = v
                }
            }

            // ---- Lua ----
            Card {
                title: "Lua"
                iconName: "moon"
                headerItem: [
                    Toggle {
                        checked: todModel.moonEnabled
                        onToggled: todModel.moonEnabled = !checked
                    }
                ]

                SliderRow {
                    enabled: todModel.moonEnabled
                    opacity: todModel.moonEnabled ? 1.0 : 0.4
                    label: "Fase (offset do sol)"
                    valueText: root.phaseName(todModel.moonPhaseOffsetHours) + " · " +
                               Math.round(todModel.moonPhaseFraction * 100) + "%"
                    from: 0; to: 24
                    boundValue: todModel.moonPhaseOffsetHours
                    onMoved: v => todModel.moonPhaseOffsetHours = v
                }
                SliderRow {
                    enabled: todModel.moonEnabled
                    opacity: todModel.moonEnabled ? 1.0 : 0.4
                    label: "Intensidade do luar"
                    valueText: root.fmt(todModel.moonIntensity, 2)
                    from: 0; to: 1
                    boundValue: todModel.moonIntensity
                    onMoved: v => todModel.moonIntensity = v
                }
                SliderRow {
                    enabled: todModel.moonEnabled
                    opacity: todModel.moonEnabled ? 1.0 : 0.4
                    label: "Tamanho do disco"
                    valueText: root.fmt(todModel.moonDiskSize) + "×"
                    from: 0.5; to: 4
                    boundValue: todModel.moonDiskSize
                    onMoved: v => todModel.moonDiskSize = v
                }
            }

            // ---- Estrelas ----
            Card {
                title: "Estrelas"
                iconName: "sparkles"

                SliderRow {
                    label: "Intensidade"
                    valueText: root.fmt(todModel.starIntensity)
                    from: 0; to: 3
                    boundValue: todModel.starIntensity
                    onMoved: v => todModel.starIntensity = v
                }
            }
        }
    }
}
