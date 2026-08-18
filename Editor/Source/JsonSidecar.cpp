#include "SmileEditor/JsonSidecar.h"
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
            // Sair sem commit() JA descarta o temporario — o destrutor do QSaveFile "discards the
            // saved contents unless commit() was called", e o commit() sozinho ja recusaria
            // ("commits the changes to disk, IF all previous writes were successful"). Ou seja:
            // este ramo esta seguro mesmo sem a linha abaixo.
            //
            // O cancelWriting() fica como declaracao de INTENCAO, nao como o que segura o
            // arquivo: ele arma o erro para que qualquer commit() futuro descarte, o que protege
            // de alguem inserir um commit() aqui embaixo mais tarde. A checagem explicita do short
            // write tambem nao e o que garante a atomicidade (o Qt ja rastreia erro de escrita) —
            // ela existe para LOGAR o motivo preciso, que o Qt nao faz por nos.
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
