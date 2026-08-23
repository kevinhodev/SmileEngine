pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "components" as C

// Janela do ciclo solar/lunar, ligada a TimeOfDayBridge e WindowBridge.
Rectangle {
    id: root
    required property var todModel
    required property var todWindow

    color: C.Theme.shell
    border.color: C.Theme.hairlineStrong
    border.width: 1
    implicitWidth: 840
    implicitHeight: 672

    readonly property color cardBg: C.Theme.cardBg
    readonly property color borderColor: C.Theme.borderColor
    readonly property color divider: C.Theme.divider
    readonly property color textPrimary: C.Theme.textPrimary
    readonly property color textNormal: C.Theme.textNormal
    readonly property color textSecondary: C.Theme.textSecondary
    readonly property color textMuted: C.Theme.textMuted
    readonly property color blue: C.Theme.blue
    readonly property color blueBg: C.Theme.blueBg
    readonly property color blueBorder: C.Theme.blueBorder
    readonly property color sunColor: "#d8a03a"
    readonly property color moonColor: "#8fa9d8"

    // -1 indica que o sol nao cruza o horizonte no dia corrente.
    property real sunriseH: -1
    property real sunsetH: -1

    function recomputeSunTimes() {
        var rise = -1, set = -1
        var prev = todModel.sunElevationAt(0)
        for (var i = 1; i <= 96; ++i) {
            var h = i / 4.0
            var el = todModel.sunElevationAt(h)
            if (prev < 0 && el >= 0 && rise < 0) rise = h - 0.25 + 0.25 * (-prev / (el - prev))
            if (prev >= 0 && el < 0 && set < 0) set = h - 0.25 + 0.25 * (prev / (prev - el))
            prev = el
        }
        sunriseH = rise
        sunsetH = set
    }
    Component.onCompleted: recomputeSunTimes()

    function fmt(v, dec) { return v.toFixed(dec === undefined ? 1 : dec).replace(".", ",") }

    function hourDist(a, b) { var d = Math.abs(a - b); return Math.min(d, 24 - d) }
    function dayDist(a, b)  { var d = Math.abs(a - b); return Math.min(d, 365 - d) }

    function timeLabel(h) {
        var w = ((h % 24) + 24) % 24
        var hh = Math.floor(w)
        var mm = Math.floor((w - hh) * 60)
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

    function dayPhase(hours, sunEl) {
        if (sunEl >= 6)   return "dia"
        if (sunEl >= 0)   return hours < 12 ? "amanhecer" : "entardecer"
        if (sunEl >= -12) return "crepúsculo"
        return "noite"
    }

    component Toggle: C.Toggle {}
    component Card: C.Card { contentSpacing: 10 }
    component SliderRow: C.SliderRow {}
    component Chip: C.Chip { height: 26 }

    C.FramelessTitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: 40
        windowBridge: root.todWindow
        title: "Time of Day"
        subtitle: root.doyLabel(root.todModel.dayOfYear) + " · "
            + Math.abs(Math.round(root.todModel.latitudeDeg)) + "° "
            + (root.todModel.latitudeDeg >= 0 ? "N" : "S")
            + (root.sunsetH >= 0 ? " · pôr do sol " + root.timeLabel(root.sunsetH) : "")
        iconName: "sun"
        iconColor: root.todModel.enabled ? root.sunColor : root.textMuted
        showMaximize: false
        actions: Component {
            Toggle {
                checked: root.todModel.enabled
                onToggled: root.todModel.enabled = !checked
            }
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
            opacity: root.todModel.available ? 1.0 : 0.45

            // ---- Arco do ceu (hero interativo: arrastar na horizontal escruba a hora) ----
            Rectangle {
                width: parent.width
                height: 180
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
                        var sunEl = root.todModel.sunElevationDeg
                        var nowX = (root.todModel.timeHours / 24) * w

                        // ceu (gradiente vertical sutil) / chao
                        var sky = ctx.createLinearGradient(0, 0, 0, horizonY)
                        sky.addColorStop(0, "#0e1119")
                        sky.addColorStop(1, "#1b1d24")
                        ctx.fillStyle = sky
                        ctx.fillRect(0, 0, w, horizonY)
                        ctx.fillStyle = "#15150f"
                        ctx.fillRect(0, horizonY, w, h - horizonY)

                        // brilho quente perto do horizonte quando o sol esta baixo (golden hour)
                        if (sunEl > -12 && sunEl < 25) {
                            var sy0 = elToY(sunEl)
                            var warmA = 0.22 * (1 - Math.max(0, sunEl) / 25)
                            var warm = ctx.createRadialGradient(nowX, sy0, 4, nowX, sy0, 100)
                            warm.addColorStop(0, "rgba(216,112,63," + warmA.toFixed(3) + ")")
                            warm.addColorStop(1, "rgba(216,112,63,0)")
                            ctx.fillStyle = warm
                            ctx.fillRect(0, 0, w, h)
                        }

                        // estrelas: pontos pseudo-aleatorios fixos; alpha segue a noite na
                        // hora correspondente aquele x (o arco e um mapa hora -> ceu)
                        ctx.fillStyle = "#cfd8ea"
                        for (var s = 0; s < 26; ++s) {
                            var fx = Math.sin(s * 127.1 + 311.7) * 43758.5453
                            fx -= Math.floor(fx)
                            var fy = Math.sin(s * 269.5 + 183.3) * 28001.8384
                            fy -= Math.floor(fy)
                            var sx = fx * w
                            var syy = 6 + fy * (horizonY - 26)
                            var elS = root.todModel.sunElevationAt(24 * sx / w)
                            var a = Math.max(0, Math.min(1, -elS / 12)) *
                                    (0.2 + 0.35 * ((s * 7) % 10) / 10)
                            if (a > 0.02) {
                                ctx.globalAlpha = a
                                ctx.fillRect(sx, syy, 1.3, 1.3)
                            }
                        }
                        ctx.globalAlpha = 1.0

                        // linha do horizonte + grade das horas (6/12/18)
                        ctx.strokeStyle = "rgba(230,226,216,0.18)"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, horizonY); ctx.lineTo(w, horizonY)
                        ctx.stroke()
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
                            var yM = elToY(root.todModel.moonElevationAt(hM))
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
                            var yS = elToY(root.todModel.sunElevationAt(hS))
                            if (j === 0) ctx.moveTo((hS / 24) * w, yS)
                            else         ctx.lineTo((hS / 24) * w, yS)
                        }
                        ctx.stroke()

                        // marcas de nascer/por do sol no horizonte
                        ctx.strokeStyle = "rgba(216,160,58,0.5)"
                        ctx.fillStyle = "rgba(216,160,58,0.55)"
                        ctx.font = "9px Segoe UI"
                        ctx.textAlign = "center"
                        var cross = [root.sunriseH, root.sunsetH]
                        for (var c = 0; c < 2; ++c) {
                            if (cross[c] < 0) continue
                            var cx = (cross[c] / 24) * w
                            ctx.beginPath()
                            ctx.moveTo(cx, horizonY - 4); ctx.lineTo(cx, horizonY + 4)
                            ctx.stroke()
                            ctx.fillText(root.timeLabel(cross[c]), cx, horizonY + 15)
                        }

                        // linha do agora (accent, mais visivel: o arco e clicavel)
                        ctx.strokeStyle = "rgba(91,157,255,0.35)"
                        ctx.beginPath(); ctx.moveTo(nowX, 0); ctx.lineTo(nowX, h - 18); ctx.stroke()

                        // marcador da lua (anel sutil em volta)
                        var moonEl = root.todModel.moonElevationDeg
                        var my = elToY(moonEl)
                        ctx.globalAlpha = moonEl > -8 ? 1.0 : 0.35
                        ctx.strokeStyle = "rgba(207,216,234,0.25)"
                        ctx.beginPath(); ctx.arc(nowX, my, 6.5, 0, 6.2832); ctx.stroke()
                        ctx.fillStyle = "#cfd8ea"
                        ctx.beginPath(); ctx.arc(nowX, my, 3.5, 0, 6.2832); ctx.fill()
                        ctx.globalAlpha = 1.0

                        // marcador do sol (halo + disco, quente perto do horizonte)
                        var sy = elToY(sunEl)
                        var t = Math.max(0, Math.min(1, sunEl / 40))
                        var r = Math.round(255 - 30 * t)
                        var gr = Math.round(140 + 80 * t)
                        var b = Math.round(60 + 40 * t)
                        if (sunEl > -12) {
                            var glow = ctx.createRadialGradient(nowX, sy, 1, nowX, sy, 15)
                            glow.addColorStop(0, "rgba(" + r + "," + gr + "," + b + ",0.55)")
                            glow.addColorStop(1, "rgba(" + r + "," + gr + "," + b + ",0)")
                            ctx.fillStyle = glow
                            ctx.beginPath(); ctx.arc(nowX, sy, 15, 0, 6.2832); ctx.fill()
                        }
                        ctx.globalAlpha = sunEl > -12 ? 1.0 : 0.3
                        ctx.fillStyle = "rgb(" + r + "," + gr + "," + b + ")"
                        ctx.beginPath(); ctx.arc(nowX, sy, 5, 0, 6.2832); ctx.fill()
                        ctx.globalAlpha = 1.0

                        // horas no rodape
                        ctx.fillStyle = "rgba(154,149,138,0.7)"
                        ctx.textAlign = "center"
                        var labels = [0, 6, 12, 18, 24]
                        for (var k = 0; k < labels.length; ++k) {
                            var lx = (labels[k] / 24) * w
                            lx = Math.max(8, Math.min(w - 10, lx))
                            ctx.fillText(labels[k] + "h", lx, h - 6)
                        }
                    }

                    Connections {
                        target: root.todModel
                        function onTimeChanged()  { arc.requestPaint() }
                        function onStateChanged() { arc.requestPaint() }
                        function onSolarPathChanged() { root.recomputeSunTimes() }
                    }
                    onWidthChanged: requestPaint()

                    // Pausa o relogio durante o scrub para evitar disputa com o tick.
                    MouseArea {
                        anchors.fill: parent
                        enabled: root.todModel.enabled
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        property bool wasRunning: false
                        function scrub(mx) {
                            var x = Math.max(0, Math.min(width, mx))
                            root.todModel.timeHours = 24 * x / width
                        }
                        onPressed: (mouse) => {
                            wasRunning = root.todModel.running
                            root.todModel.running = false
                            scrub(mouse.x)
                        }
                        onPositionChanged: (mouse) => { if (pressed) scrub(mouse.x) }
                        onReleased: root.todModel.running = wasRunning
                        onCanceled: root.todModel.running = wasRunning
                        onWheel: (wheel) => {
                            root.todModel.timeHours = root.todModel.timeHours +
                                                 (wheel.angleDelta.y > 0 ? 0.25 : -0.25)
                        }
                    }
                }

                Rectangle {
                    x: Math.max(60, Math.min(parent.width - 106,
                       (root.todModel.timeHours / 24) * parent.width - 23))
                    y: 6
                    width: 46; height: 18
                    radius: 4
                    color: root.blueBg
                    border.color: root.blueBorder
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: root.timeLabel(root.todModel.timeHours)
                        color: root.blue
                        font.family: C.Theme.fontFamily
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }
                }

                Text {
                    x: 8; y: 6
                    text: "sol " + Math.round(root.todModel.sunElevationDeg) + "°  ·  az " +
                          Math.round(root.todModel.sunAzimuthDeg) + "°"
                    color: root.textSecondary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 9
                }
                Text {
                    anchors.right: parent.right; anchors.rightMargin: 8; y: 6
                    text: "lua " + Math.round(root.todModel.moonElevationDeg) + "°"
                    color: Qt.alpha(root.moonColor, 0.8)
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 9
                }
            }

            Item {
                width: parent.width
                height: 36
                enabled: root.todModel.enabled
                opacity: root.todModel.enabled ? 1.0 : 0.45

                Rectangle {
                    id: playBtn
                    width: 30; height: 30; radius: 15
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.todModel.running ? C.Theme.accentBg : C.Theme.surfaceRaised
                    border.color: root.todModel.running ? C.Theme.accentBorder
                                                   : C.Theme.hairlineStrong
                    border.width: 1
                    LucideIcon {
                        anchors.centerIn: parent
                        name: root.todModel.running ? "pause" : "play"
                        size: 13
                        color: root.todModel.running ? root.blue : root.textNormal
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.todModel.running = !root.todModel.running }
                }

                // Aceita HH:MM; fora de foco acompanha o relogio.
                TextInput {
                    id: bigTime
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.textPrimary
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    horizontalAlignment: TextInput.AlignRight
                    selectByMouse: true
                    selectionColor: root.blue
                    selectedTextColor: C.Theme.shell
                    validator: RegularExpressionValidator {
                        regularExpression: /^(?:[01]?\d|2[0-3]):[0-5]\d$/
                    }
                    Binding {
                        target: bigTime; property: "text"
                        value: root.timeLabel(root.todModel.timeHours)
                        when: !bigTime.activeFocus
                        restoreMode: Binding.RestoreBinding
                    }
                    onAccepted: {
                        var p = text.split(":")
                        root.todModel.timeHours = Number(p[0]) + Number(p[1]) / 60
                        focus = false
                    }
                    Keys.onEscapePressed: focus = false
                    HoverHandler { cursorShape: Qt.IBeamCursor }
                }
                Text {
                    id: phaseLbl
                    anchors.right: bigTime.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.dayPhase(root.todModel.timeHours, root.todModel.sunElevationDeg)
                    color: root.textMuted
                    font.family: C.Theme.fontFamily
                    font.pixelSize: 10
                }

                Slider {
                    id: hourSlider
                    anchors.left: playBtn.right
                    anchors.leftMargin: 10
                    anchors.right: phaseLbl.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 18
                    from: 0; to: 24
                    onMoved: root.todModel.timeHours = value
                    Binding {
                        target: hourSlider; property: "value"
                        value: root.todModel.timeHours
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

            Item {
                width: parent.width
                implicitHeight: Math.max(leftCol.implicitHeight, rightCol.implicitHeight)

                Column {
                    id: leftCol
                    width: (parent.width - 10) / 2
                    spacing: 10

                    Card {
                        title: "Relógio"
                        iconName: "clock"
                        visible: root.todModel.enabled

                        // Amanhecer e entardecer usam os cruzamentos do horizonte.
                        Row {
                            width: parent.width
                            spacing: 6
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Amanhecer"
                                readonly property real t: root.sunriseH >= 0 ? root.sunriseH : 6
                                active: root.hourDist(root.todModel.timeHours, t) < 0.13
                                onTapped: root.todModel.timeHours = t
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Meio-dia"
                                active: root.hourDist(root.todModel.timeHours, 12) < 0.13
                                onTapped: root.todModel.timeHours = 12
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Entardecer"
                                readonly property real t: root.sunsetH >= 0 ? root.sunsetH : 18
                                active: root.hourDist(root.todModel.timeHours, t) < 0.13
                                onTapped: root.todModel.timeHours = t
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Meia-noite"
                                active: root.hourDist(root.todModel.timeHours, 0) < 0.13
                                onTapped: root.todModel.timeHours = 0
                            }
                        }

                        SliderRow {
                            label: "Duração do dia"
                            valueText: root.fmt(root.todModel.dayLengthSec / 60) + " min"
                            from: 0.5; to: 30; stepSize: 0.5
                            boundValue: root.todModel.dayLengthSec / 60
                            onMoved: v => root.todModel.dayLengthSec = v * 60
                        }
                    }

                    Card {
                        title: "Sol manual"
                        iconName: "sun"
                        visible: !root.todModel.enabled

                        Text {
                            width: parent.width
                            text: "O relógio está desligado — o sol é posicionado direto."
                            color: root.textMuted
                            font.family: C.Theme.fontFamily
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }
                        SliderRow {
                            label: "Elevação"
                            valueText: Math.round(root.todModel.manualElevationDeg) + "°"
                            from: -20; to: 90
                            boundValue: root.todModel.manualElevationDeg
                            onMoved: v => root.todModel.manualElevationDeg = v
                        }
                        SliderRow {
                            label: "Azimute"
                            valueText: Math.round(root.todModel.manualAzimuthDeg) + "°"
                            from: 0; to: 360
                            boundValue: root.todModel.manualAzimuthDeg
                            onMoved: v => root.todModel.manualAzimuthDeg = v
                        }
                    }

                    Card {
                        title: "Geografia"
                        iconName: "pin"

                        Row {
                            width: parent.width
                            spacing: 6
                            Chip {
                                width: (parent.width - 12) / 3
                                label: "Verão"
                                readonly property int t: root.todModel.latitudeDeg >= 0 ? 172 : 355
                                active: root.dayDist(root.todModel.dayOfYear, t) <= 3
                                onTapped: root.todModel.dayOfYear = t
                            }
                            Chip {
                                width: (parent.width - 12) / 3
                                label: "Equinócio"
                                active: root.dayDist(root.todModel.dayOfYear, 80) <= 3
                                onTapped: root.todModel.dayOfYear = 80
                            }
                            Chip {
                                width: (parent.width - 12) / 3
                                label: "Inverno"
                                readonly property int t: root.todModel.latitudeDeg >= 0 ? 355 : 172
                                active: root.dayDist(root.todModel.dayOfYear, t) <= 3
                                onTapped: root.todModel.dayOfYear = t
                            }
                        }

                        SliderRow {
                            label: "Latitude"
                            valueText: Math.abs(Math.round(root.todModel.latitudeDeg)) + "° " +
                                       (root.todModel.latitudeDeg >= 0 ? "N" : "S")
                            from: -66; to: 66
                            boundValue: root.todModel.latitudeDeg
                            onMoved: v => root.todModel.latitudeDeg = v
                        }
                        SliderRow {
                            label: "Dia do ano"
                            valueText: root.doyLabel(root.todModel.dayOfYear)
                            from: 1; to: 365; stepSize: 1
                            boundValue: root.todModel.dayOfYear
                            onMoved: v => root.todModel.dayOfYear = Math.round(v)
                        }
                        SliderRow {
                            label: "Norte da cena"
                            valueText: Math.round(root.todModel.northOffsetDeg) + "°"
                            from: 0; to: 360
                            boundValue: root.todModel.northOffsetDeg
                            onMoved: v => root.todModel.northOffsetDeg = v
                        }
                    }

                }

                Column {
                    id: rightCol
                    anchors.right: parent.right
                    width: (parent.width - 10) / 2
                    spacing: 10

                    Card {
                        title: "Lua"
                        iconName: "moon"
                        headerItem: [
                            Toggle {
                                checked: root.todModel.moonEnabled
                                onToggled: root.todModel.moonEnabled = !checked
                            }
                        ]

                        Row {
                            width: parent.width
                            spacing: 6
                            enabled: root.todModel.moonEnabled
                            opacity: root.todModel.moonEnabled ? 1.0 : 0.4
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Nova"
                                active: root.hourDist(root.todModel.moonPhaseOffsetHours, 0) < 1
                                onTapped: root.todModel.moonPhaseOffsetHours = 0
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Crescente"
                                active: root.hourDist(root.todModel.moonPhaseOffsetHours, 6) < 1
                                onTapped: root.todModel.moonPhaseOffsetHours = 6
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Cheia"
                                active: root.hourDist(root.todModel.moonPhaseOffsetHours, 12) < 1
                                onTapped: root.todModel.moonPhaseOffsetHours = 12
                            }
                            Chip {
                                width: (parent.width - 18) / 4
                                label: "Minguante"
                                active: root.hourDist(root.todModel.moonPhaseOffsetHours, 18) < 1
                                onTapped: root.todModel.moonPhaseOffsetHours = 18
                            }
                        }

                        SliderRow {
                            enabled: root.todModel.moonEnabled
                            opacity: root.todModel.moonEnabled ? 1.0 : 0.4
                            label: "Ajuste fino da fase"
                            valueText: root.phaseName(root.todModel.moonPhaseOffsetHours) + " · " +
                                       Math.round(root.todModel.moonPhaseFraction * 100) + "%"
                            from: 0; to: 24
                            boundValue: root.todModel.moonPhaseOffsetHours
                            onMoved: v => root.todModel.moonPhaseOffsetHours = v
                        }
                        SliderRow {
                            enabled: root.todModel.moonEnabled
                            opacity: root.todModel.moonEnabled ? 1.0 : 0.4
                            label: "Intensidade do luar"
                            valueText: root.fmt(root.todModel.moonIntensity, 2)
                            from: 0; to: 1
                            boundValue: root.todModel.moonIntensity
                            onMoved: v => root.todModel.moonIntensity = v
                        }
                        SliderRow {
                            enabled: root.todModel.moonEnabled
                            opacity: root.todModel.moonEnabled ? 1.0 : 0.4
                            label: "Tamanho do disco"
                            valueText: root.fmt(root.todModel.moonDiskSize) + "×"
                            from: 0.5; to: 4
                            boundValue: root.todModel.moonDiskSize
                            onMoved: v => root.todModel.moonDiskSize = v
                        }
                        SliderRow {
                            enabled: root.todModel.moonEnabled
                            opacity: root.todModel.moonEnabled ? 1.0 : 0.4
                            label: "Brilho do disco"
                            valueText: root.fmt(root.todModel.moonDiskBrightness)
                            from: 0.1; to: 3
                            boundValue: root.todModel.moonDiskBrightness
                            onMoved: v => root.todModel.moonDiskBrightness = v
                        }
                    }

                    Card {
                        title: "Estrelas"
                        iconName: "sparkles"

                        SliderRow {
                            label: "Intensidade"
                            valueText: root.fmt(root.todModel.starIntensity)
                            from: 0; to: 3
                            boundValue: root.todModel.starIntensity
                            onMoved: v => root.todModel.starIntensity = v
                        }

                        Item {
                            width: parent.width
                            height: 22

                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Hora sideral local"
                                color: root.textSecondary
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 10
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.timeLabel(root.todModel.siderealTimeHours)
                                color: root.textNormal
                                font.family: C.Theme.fontFamily
                                font.pixelSize: 10
                                font.weight: Font.Medium
                            }
                        }
                    }

                }
            }
        }
    }
}
