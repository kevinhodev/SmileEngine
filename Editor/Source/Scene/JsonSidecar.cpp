#include "SmileEditor/Scene/JsonSidecar.h"
#include "Smile/Core/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace SmileEditor {
    bool WriteJsonSidecar(const QString& _Path, const QJsonObject& _Root, const char* _Label) {
        const std::string Where = std::string(_Label) + ": " + _Path.toStdString();

        QSaveFile File(_Path);
        if (!File.open(QIODevice::WriteOnly)) {
            Smile::LogError(Where + " — nao foi possivel abrir para escrita (" +
                            File.errorString().toStdString() + ")");
            return false;
        }

        const QByteArray Payload = QJsonDocument(_Root).toJson(QJsonDocument::Indented);
        const qint64 Written = File.write(Payload);
        if (Written != Payload.size()) {
            // Impede que um commit futuro publique um payload incompleto.
            File.cancelWriting();
            Smile::LogError(Where + " — escrita incompleta (" + std::to_string(Written) + " de " +
                            std::to_string(Payload.size()) + " bytes); o arquivo anterior foi "
                            "preservado");
            return false;
        }
        if (!File.commit()) {
            Smile::LogError(Where + " — falha ao publicar o arquivo (" +
                            File.errorString().toStdString() + ")");
            return false;
        }
        return true;
    }
}
