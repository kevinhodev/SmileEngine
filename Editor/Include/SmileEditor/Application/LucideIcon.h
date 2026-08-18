#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace SmileEditor {
    QIcon MakeLucideIcon(const QString& Name,
                         const QColor& Color = QColor(221, 216, 202),
                         int Size = 24);
}
