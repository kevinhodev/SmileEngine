#include "SmileEditor/LucideIcon.h"

#include <QByteArray>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace SmileEditor {
    namespace {
        QString IconBody(const QString& Name) {
            if (Name == QStringLiteral("sun")) {
                return QStringLiteral(
                    R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>)");
            }
            if (Name == QStringLiteral("moon")) {
                return QStringLiteral(
                    R"(<path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/>)");
            }
            if (Name == QStringLiteral("cloud-sun")) {
                return QStringLiteral(
                    R"(<path d="M12 2v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="M2 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="M12 8a4 4 0 0 1 3.84 5.14"/><path d="M20 17.58A5 5 0 0 0 18 8h-1.26A8 8 0 1 0 4 16.25"/><path d="M8 20h11a3 3 0 0 0 .2-6"/>)");
            }
            if (Name == QStringLiteral("cloud")) {
                return QStringLiteral(
                    R"(<path d="M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z"/>)");
            }
            if (Name == QStringLiteral("orbit")) {
                return QStringLiteral(
                    R"(<circle cx="12" cy="12" r="3"/><circle cx="19" cy="5" r="2"/><circle cx="5" cy="19" r="2"/><path d="M10.4 21.6a10 10 0 0 1-8-8"/><path d="M13.6 2.4a10 10 0 0 1 8 8"/><path d="M16.9 7.1 14.1 9.9"/><path d="M9.9 14.1l-2.8 2.8"/>)");
            }
            if (Name == QStringLiteral("waves")) {
                return QStringLiteral(
                    R"(<path d="M2 6c.6.5 1.2 1 2.5 1C7 7 7 5 9.5 5S12 7 14.5 7 17 5 19.5 5c1.3 0 1.9.5 2.5 1"/><path d="M2 12c.6.5 1.2 1 2.5 1C7 13 7 11 9.5 11s2.5 2 5 2 2.5-2 5-2c1.3 0 1.9.5 2.5 1"/><path d="M2 18c.6.5 1.2 1 2.5 1C7 19 7 17 9.5 17s2.5 2 5 2 2.5-2 5-2c1.3 0 1.9.5 2.5 1"/>)");
            }
            if (Name == QStringLiteral("globe")) {
                return QStringLiteral(
                    R"(<circle cx="12" cy="12" r="10"/><path d="M2 12h20"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10Z"/>)");
            }
            if (Name == QStringLiteral("terminal")) {
                return QStringLiteral(
                    R"(<path d="m7 11 2-2-2-2"/><path d="M11 13h4"/><rect x="3" y="4" width="18" height="16" rx="2"/>)");
            }
            if (Name == QStringLiteral("settings")) {
                return QStringLiteral(
                    R"(<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.51a2 2 0 0 1 1-1.72l.15-.1a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2Z"/><circle cx="12" cy="12" r="3"/>)");
            }
            if (Name == QStringLiteral("info")) {
                return QStringLiteral(
                    R"(<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>)");
            }
            if (Name == QStringLiteral("rotate-ccw")) {
                return QStringLiteral(
                    R"(<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>)");
            }
            if (Name == QStringLiteral("chevron-up")) {
                return QStringLiteral(R"(<path d="m18 15-6-6-6 6"/>)");
            }
            if (Name == QStringLiteral("x")) {
                return QStringLiteral(R"(<path d="M18 6 6 18"/><path d="m6 6 12 12"/>)");
            }
            if (Name == QStringLiteral("minus")) {
                return QStringLiteral(R"(<path d="M5 12h14"/>)");
            }
            if (Name == QStringLiteral("square")) {
                return QStringLiteral(R"(<rect x="5" y="5" width="14" height="14" rx="2"/>)");
            }
            return QStringLiteral(R"(<circle cx="12" cy="12" r="9"/>)");
        }
    }

    QIcon MakeLucideIcon(const QString& Name, const QColor& Color, int Size) {
        const QString Svg = QStringLiteral(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="%1" height="%1" viewBox="0 0 24 24" fill="none" stroke="%2" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">%3</svg>)")
            .arg(Size)
            .arg(Color.name(QColor::HexRgb))
            .arg(IconBody(Name));

        QSvgRenderer Renderer(Svg.toUtf8());
        QPixmap Pixmap(Size, Size);
        Pixmap.fill(Qt::transparent);

        QPainter Painter(&Pixmap);
        Painter.setRenderHint(QPainter::Antialiasing, true);
        Renderer.render(&Painter);

        return QIcon(Pixmap);
    }
}
