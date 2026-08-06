#include "SmileEditor/SmileLogo.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>

namespace SmileEditor {
    namespace {
        // A marca e a silhueta de uma esfera com o crescente do limbo iluminado: o terminador
        // desenha a curva do sorriso sem virar uma cara. Traco unico, UMA cor chapada, sem
        // sombra e sem gradiente — e isso que faz o desenho sobreviver a 16 px (barra de
        // titulo, taskbar) e a impressao em uma cor. Geometria em espaco 96x96, igual a
        // Editor/Design/Logo_Reference.svg (direcao 1).
        constexpr qreal kGrid         = 96.0;
        constexpr qreal kSphereRadius = 34.0;
        const QPointF   kSphereCenter(48.0, 48.0);
        // Esfera de sombra deslocada na direcao da luz (cima/esquerda). A DIFERENCA entre os
        // dois circulos e o crescente — mexer neste ponto muda a "fase" da marca, e a
        // distancia entre os centros e a espessura dela.
        const QPointF   kShadowCenter(39.0, 37.0);

        const QColor kGold(0xD9, 0xA5, 0x14);   // mesmo acento do editor e do About

        void DrawSmileMark(QPainter& Painter, const QRectF& Bounds) {
            Painter.save();
            Painter.translate(Bounds.topLeft());
            Painter.scale(Bounds.width() / kGrid, Bounds.height() / kGrid);

            QPainterPath Sphere;
            Sphere.addEllipse(kSphereCenter, kSphereRadius, kSphereRadius);
            QPainterPath Shadow;
            Shadow.addEllipse(kShadowCenter, kSphereRadius, kSphereRadius);

            Painter.setPen(Qt::NoPen);
            Painter.setBrush(kGold);
            Painter.drawPath(Sphere.subtracted(Shadow));

            // A silhueta fecha a leitura de "esfera" nos tamanhos grandes; nos pequenos ela
            // afina sozinha com a escala e some, deixando so o crescente. Nao ha nada a
            // recuperar ali — o crescente ja e a marca.
            QColor Outline = kGold;
            Outline.setAlphaF(0.4f);
            Painter.setBrush(Qt::NoBrush);
            Painter.setPen(QPen(Outline, 2.5));
            Painter.drawEllipse(kSphereCenter, kSphereRadius, kSphereRadius);

            Painter.restore();
        }
    }

    QPixmap MakeSmileLogoPixmap(int Size, bool IncludeBadgeBackground) {
        const int ClampedSize = qMax(16, Size);
        QPixmap Pixmap(ClampedSize, ClampedSize);
        Pixmap.fill(Qt::transparent);

        QPainter Painter(&Pixmap);
        Painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF FullRect(0.5, 0.5, ClampedSize - 1.0, ClampedSize - 1.0);
        if (IncludeBadgeBackground) {
            QPainterPath Badge;
            const qreal Radius = ClampedSize * 0.22;
            Badge.addRoundedRect(FullRect, Radius, Radius);
            Painter.fillPath(Badge, QColor(0x16, 0x17, 0x0F));

            QColor Border = kGold;
            Border.setAlphaF(0.22f);
            Painter.setPen(QPen(Border, 1.0));
            Painter.drawPath(Badge);
        }

        // Sem badge a marca usa a caixa inteira: a folga dela ja vem da grade de 96 (a esfera
        // tem raio 34). Com badge ela recua para nao encostar no canto arredondado.
        const qreal Margin = IncludeBadgeBackground ? ClampedSize * 0.08 : 0.0;
        DrawSmileMark(Painter, FullRect.adjusted(Margin, Margin, -Margin, -Margin));

        return Pixmap;
    }
}
