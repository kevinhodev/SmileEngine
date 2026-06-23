#include "SmileEditor/SmileLogo.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>

namespace SmileEditor {
    namespace {
        void DrawSmileMark(QPainter& Painter, const QRectF& Bounds) {
            Painter.save();
            Painter.translate(Bounds.topLeft());
            Painter.scale(Bounds.width() / 96.0, Bounds.height() / 96.0);

            QRadialGradient Glow(QPointF(50.0, 50.0), 48.0);
            Glow.setColorAt(0.0, QColor(255, 191, 24, 72));
            Glow.setColorAt(0.55, QColor(255, 180, 0, 20));
            Glow.setColorAt(1.0, QColor(255, 180, 0, 0));
            Painter.setPen(Qt::NoPen);
            Painter.setBrush(Glow);
            Painter.drawEllipse(QRectF(1.0, 0.0, 94.0, 94.0));

            QPainterPath MainRibbon;
            MainRibbon.moveTo(78.0, 18.0);
            MainRibbon.lineTo(43.0, 18.0);
            MainRibbon.cubicTo(24.0, 18.0, 14.0, 31.0, 18.0, 45.0);
            MainRibbon.cubicTo(20.5, 54.0, 30.0, 58.0, 43.0, 58.0);
            MainRibbon.lineTo(58.0, 58.0);
            MainRibbon.cubicTo(73.0, 58.0, 79.0, 68.0, 70.0, 77.0);
            MainRibbon.cubicTo(61.0, 86.0, 39.0, 83.0, 18.0, 70.0);

            QPainterPath CenterRibbon;
            CenterRibbon.moveTo(42.0, 39.0);
            CenterRibbon.lineTo(72.0, 39.0);

            QPen ShadowPen(QColor(0, 0, 0, 170), 16.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            Painter.strokePath(MainRibbon, ShadowPen);
            Painter.strokePath(CenterRibbon, ShadowPen);

            QLinearGradient Gold(0.0, 14.0, 0.0, 84.0);
            Gold.setColorAt(0.0, QColor(255, 235, 98));
            Gold.setColorAt(0.32, QColor(255, 194, 24));
            Gold.setColorAt(0.68, QColor(230, 151, 0));
            Gold.setColorAt(1.0, QColor(255, 211, 52));

            QPen GoldPen(QBrush(Gold), 12.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            Painter.strokePath(MainRibbon, GoldPen);
            Painter.strokePath(CenterRibbon, GoldPen);

            QPen HighlightPen(QColor(255, 246, 170, 210), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            Painter.strokePath(MainRibbon, HighlightPen);
            Painter.strokePath(CenterRibbon, HighlightPen);

            QPen LowerEdgePen(QColor(130, 74, 0, 150), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            Painter.translate(0.0, 3.0);
            Painter.strokePath(MainRibbon, LowerEdgePen);
            Painter.strokePath(CenterRibbon, LowerEdgePen);
            Painter.translate(0.0, -3.0);

            QPen EyeShadow(QColor(0, 0, 0, 135), 5.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            QPen EyePen(QColor(255, 213, 39), 4.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

            auto DrawEye = [&](const QRectF& Rect) {
                Painter.setPen(EyeShadow);
                Painter.drawArc(Rect.translated(0.0, 1.5), 0, 180 * 16);
                Painter.setPen(EyePen);
                Painter.drawArc(Rect, 0, 180 * 16);
            };

            DrawEye(QRectF(36.0, 60.0, 12.0, 13.0));
            DrawEye(QRectF(55.0, 60.0, 12.0, 13.0));

            Painter.restore();
        }
    }

    QPixmap MakeSmileLogoPixmap(int Size, bool IncludeBadgeBackground) {
        const int ClampedSize = qMax(16, Size);
        QPixmap Pixmap(ClampedSize, ClampedSize);
        Pixmap.fill(Qt::transparent);

        QPainter Painter(&Pixmap);
        Painter.setRenderHint(QPainter::Antialiasing, true);
        Painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF FullRect(0.5, 0.5, ClampedSize - 1.0, ClampedSize - 1.0);
        if (IncludeBadgeBackground) {
            QPainterPath Badge;
            const qreal Radius = ClampedSize * 0.22;
            Badge.addRoundedRect(FullRect, Radius, Radius);

            QLinearGradient BadgeGradient(FullRect.topLeft(), FullRect.bottomRight());
            BadgeGradient.setColorAt(0.0, QColor(36, 37, 35));
            BadgeGradient.setColorAt(0.5, QColor(18, 18, 17));
            BadgeGradient.setColorAt(1.0, QColor(6, 7, 7));
            Painter.fillPath(Badge, BadgeGradient);

            Painter.setPen(QPen(QColor(255, 217, 93, 42), 1.0));
            Painter.drawPath(Badge);
        }

        const qreal Margin = IncludeBadgeBackground ? ClampedSize * 0.12 : ClampedSize * 0.04;
        DrawSmileMark(Painter, FullRect.adjusted(Margin, Margin, -Margin, -Margin));

        return Pixmap;
    }
}
