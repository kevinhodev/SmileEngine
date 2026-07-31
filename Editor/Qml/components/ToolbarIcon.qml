import QtQuick

// Iconografia vetorial compacta usada pela toolbar do viewport.
Canvas {
    id: icon
    property string name: ""
    property color color: Theme.textNormal

    implicitWidth: 14
    implicitHeight: 14

    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.strokeStyle = color
        ctx.fillStyle = color
        ctx.lineWidth = 1.45
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        ctx.scale(width / 18, height / 18)

        function line(x1, y1, x2, y2) {
            ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke()
        }
        function poly(points, closePath) {
            ctx.beginPath(); ctx.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2) ctx.lineTo(points[i], points[i + 1])
            if (closePath) ctx.closePath()
            ctx.stroke()
        }
        function box(x, y, w, h) { ctx.strokeRect(x, y, w, h) }

        if (name === "add") {
            line(9, 3.5, 9, 14.5); line(3.5, 9, 14.5, 9)
        } else if (name === "undo" || name === "redo") {
            const flip = name === "redo"
            ctx.save()
            if (flip) { ctx.translate(18, 0); ctx.scale(-1, 1) }
            ctx.beginPath(); ctx.moveTo(5, 6); ctx.bezierCurveTo(8, 3, 14, 4, 15, 10)
            ctx.bezierCurveTo(15.3, 12, 14.7, 13.6, 14, 15); ctx.stroke()
            poly([5, 3.5, 5, 7.5, 9, 7.5], false)
            ctx.restore()
        } else if (name === "select") {
            poly([4, 2.5, 14.5, 10.5, 9.5, 11.2, 12.5, 16, 10, 17, 7, 12.3, 3.8, 16], true)
        } else if (name === "move") {
            line(9, 2, 9, 16); line(2, 9, 16, 9)
            poly([6.5, 4.5, 9, 2, 11.5, 4.5], false)
            poly([6.5, 13.5, 9, 16, 11.5, 13.5], false)
            poly([4.5, 6.5, 2, 9, 4.5, 11.5], false)
            poly([13.5, 6.5, 16, 9, 13.5, 11.5], false)
        } else if (name === "rotate") {
            ctx.beginPath(); ctx.arc(9, 9, 5.8, 0.45, 5.35); ctx.stroke()
            poly([11.4, 3.2, 14.4, 3.8, 13.7, 6.8], false)
        } else if (name === "scale") {
            box(2.5, 2.5, 5, 5); box(10.5, 10.5, 5, 5)
            line(7.5, 7.5, 11.5, 11.5); poly([8.8, 11.5, 11.5, 11.5, 11.5, 8.8], false)
        } else if (name === "world") {
            ctx.beginPath(); ctx.arc(9, 9, 6.3, 0, Math.PI * 2); ctx.stroke()
            line(9, 1.8, 9, 16.2); line(1.8, 9, 16.2, 9)
            ctx.beginPath(); ctx.moveTo(9, 2.7)
            ctx.bezierCurveTo(5.4, 5.2, 5.4, 12.8, 9, 15.3)
            ctx.bezierCurveTo(12.6, 12.8, 12.6, 5.2, 9, 2.7); ctx.stroke()
        } else if (name === "magnet") {
            ctx.beginPath(); ctx.moveTo(3.5, 3); ctx.lineTo(3.5, 10.2)
            ctx.bezierCurveTo(3.5, 16, 14.5, 16, 14.5, 10.2); ctx.lineTo(14.5, 3); ctx.stroke()
            line(3.5, 3, 7, 3); line(7, 3, 7, 8); line(11, 3, 14.5, 3); line(11, 3, 11, 8)
        } else if (name === "angle") {
            line(3, 14.5, 15, 14.5); line(3, 14.5, 11.3, 5)
            ctx.beginPath(); ctx.arc(3, 14.5, 5.2, -0.85, 0); ctx.stroke()
        } else if (name === "percent") {
            line(4, 14, 14, 4)
            ctx.beginPath(); ctx.arc(5, 5, 1.5, 0, Math.PI * 2); ctx.stroke()
            ctx.beginPath(); ctx.arc(13, 13, 1.5, 0, Math.PI * 2); ctx.stroke()
        } else if (name === "camera") {
            box(2, 5, 10, 8); poly([12, 7, 16, 4.5, 16, 13.5, 12, 11], true)
        } else if (name === "lit") {
            ctx.beginPath(); ctx.arc(8, 8, 5, 0, Math.PI * 2); ctx.stroke()
            poly([3.5, 11, 7, 7, 9.5, 9, 14.5, 3], false)
        } else if (name === "eye") {
            ctx.beginPath(); ctx.moveTo(1.5, 9); ctx.bezierCurveTo(5, 3.5, 13, 3.5, 16.5, 9)
            ctx.bezierCurveTo(13, 14.5, 5, 14.5, 1.5, 9); ctx.stroke()
            ctx.beginPath(); ctx.arc(9, 9, 2.3, 0, Math.PI * 2); ctx.stroke()
        } else if (name === "focus") {
            poly([6.5, 2.5, 2.5, 2.5, 2.5, 6.5], false)
            poly([11.5, 2.5, 15.5, 2.5, 15.5, 6.5], false)
            poly([15.5, 11.5, 15.5, 15.5, 11.5, 15.5], false)
            poly([6.5, 15.5, 2.5, 15.5, 2.5, 11.5], false)
            ctx.beginPath(); ctx.arc(9, 9, 2.6, 0, Math.PI * 2); ctx.stroke()
            ctx.beginPath(); ctx.arc(9, 9, 0.9, 0, Math.PI * 2); ctx.fill()
        } else if (name === "speed") {
            ctx.beginPath(); ctx.arc(9, 11, 6, Math.PI, Math.PI * 2); ctx.stroke()
            line(3, 11, 3, 14); line(15, 11, 15, 14); line(3, 14, 15, 14)
            line(9, 11, 13, 6.5)
            ctx.beginPath(); ctx.arc(9, 11, 1.2, 0, Math.PI * 2); ctx.fill()
        } else if (name === "stats") {
            line(3, 15, 3, 10); line(8, 15, 8, 4); line(13, 15, 13, 7); line(2, 15.5, 16, 15.5)
        } else if (name === "grid") {
            for (let i = 3; i <= 15; i += 4) { line(i, 2, i, 16); line(2, i, 16, i) }
        } else if (name === "maximize") {
            poly([7, 2.5, 2.5, 2.5, 2.5, 7], false); poly([11, 2.5, 15.5, 2.5, 15.5, 7], false)
            poly([15.5, 11, 15.5, 15.5, 11, 15.5], false); poly([7, 15.5, 2.5, 15.5, 2.5, 11], false)
        } else if (name === "more") {
            for (let y = 4; y <= 14; y += 5) { ctx.beginPath(); ctx.arc(9, y, 1, 0, Math.PI * 2); ctx.fill() }
        }
    }
}
