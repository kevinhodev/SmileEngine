#pragma once

#include <QString>

class QJsonObject;

namespace SmileEditor {
    // Publica JSON indentado por troca atômica. O fallback de escrita direta deve permanecer
    // desativado para preservar o arquivo anterior em qualquer falha.
    bool WriteJsonSidecar(const QString& Path, const QJsonObject& Root, const char* Label);
}
