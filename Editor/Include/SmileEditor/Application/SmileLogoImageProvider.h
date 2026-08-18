#pragma once

#include <QQuickImageProvider>
#include <QPixmap>
#include <QSize>
#include "SmileEditor/Application/SmileLogo.h"

namespace SmileEditor {
    // Expoe o logo procedural da engine ao QML: `source: "image://smilelogo/<tamanho>"`.
    // Reusa MakeSmileLogoPixmap (SmileLogo.h). Sem badge de fundo (a barra ja tem o seu).
    class SmileLogoImageProvider : public QQuickImageProvider {
    public:
        SmileLogoImageProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

        QPixmap requestPixmap(const QString& _Id, QSize* _Size, const QSize& _Requested) override {
            Q_UNUSED(_Requested);
            bool Ok = false;
            int Px = _Id.toInt(&Ok);
            if (!Ok || Px <= 0) Px = 64;
            QPixmap Pm = MakeSmileLogoPixmap(Px, /*IncludeBadgeBackground=*/false);
            if (_Size) *_Size = Pm.size();
            return Pm;
        }
    };
}
