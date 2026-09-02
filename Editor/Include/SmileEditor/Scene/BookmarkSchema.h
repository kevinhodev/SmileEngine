#pragma once

// Schema e VALIDACAO do sidecar de bookmarks de camera (<cena>.cameras.json).
//
// Extraido do CameraBookmarksBridge para haver UM parser so. O bridge e um QObject que depende
// do Renderer, entao um teste que quisesse validar o sidecar teria de arrastar a engine inteira
// — ou reimplementar as regras, que e pior: duas fontes de verdade divergem em silencio, e o
// arquivo que o gate aprova deixa de ser o arquivo que o editor aceita.
//
// Aqui so entra Qt Core (JSON). Nada de QObject, nada de Renderer.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cmath>

namespace SmileEditor::BookmarkSchema {
    // Versao do SCHEMA do sidecar, independente do kCookedVersion.
    constexpr int kSchemaVersion = 1;
    constexpr int kSlotCount     = 4;

    // Faixas plausiveis. Nao sao gosto: o sidecar e um arquivo de texto que entra no
    // repositorio e vai ser editado a mao, e um pitch de 3000 graus ou um NaN vindo dali
    // colocaria a camera num estado que nenhuma captura reproduz.
    constexpr double kMaxAbsPosition = 1.0e6;
    constexpr double kMaxAbsPitch    = 89.0;
    constexpr double kMaxAbsYaw      = 3600.0; // yaw acumula voltas; so barra o absurdo

    struct FSlot {
        bool  Filled = false;
        float PosX = 0.0f, PosY = 0.0f, PosZ = 0.0f;
        float Pitch = 0.0f, Yaw = 0.0f;
        // Gravado para manter o enquadramento compativel quando o FOV virar editavel.
        float FovYDeg = 60.0f;
    };

    // Distingue sidecar ausente de um arquivo que nao pode ser sobrescrito com seguranca.
    enum class EState {
        None,         // nao existe em disco ainda: gravar e seguro
        Ok,           // lido e entendido
        Corrupt,      // JSON invalido
        Incompatible  // schema de outra versao, ou pose que esta build nao sabe restaurar
    };

    inline bool ValidSlot(int _Slot) { return _Slot >= 0 && _Slot < kSlotCount; }

    inline bool ReadNum(const QJsonObject& _O, const char* _Key, double& _Out) {
        const QJsonValue V = _O.value(QLatin1String(_Key));
        if (!V.isDouble()) return false;
        const double N = V.toDouble();
        if (!std::isfinite(N)) return false;
        _Out = N;
        return true;
    }

    // Valida os BYTES do sidecar. _OutSlots recebe kSlotCount entradas. _OutMessage, quando nao
    // nulo, recebe o texto a mostrar ao usuario (vazio quando nao ha o que dizer).
    //
    // Um slot recusado nao invalida o arquivo inteiro, mas tambem nao vira Ok em silencio:
    // gravar por cima apagaria a entrada que esta build nao entendeu.
    inline EState Parse(const QByteArray& _Bytes, float _FovNow,
                        FSlot* _OutSlots, QString* _OutMessage = nullptr) {
        if (_OutMessage) _OutMessage->clear();

        QJsonParseError Err{};
        const QJsonDocument Doc = QJsonDocument::fromJson(_Bytes, &Err);
        if (Err.error != QJsonParseError::NoError || !Doc.isObject()) {
            if (_OutMessage) *_OutMessage = QStringLiteral("JSON invalido");
            return EState::Corrupt;
        }

        const QJsonObject Root = Doc.object();
        if (Root.value(QStringLiteral("version")).toInt(0) != kSchemaVersion) {
            if (_OutMessage) *_OutMessage = QStringLiteral("versao de schema desconhecida");
            return EState::Incompatible;
        }

        // `slots` TEM de ser array. Sem esta checagem, uma chave ausente (ou de outro tipo) virava
        // array vazio pelo toArray(), o estado ia para Ok e a gravacao seguinte substituia o
        // arquivo SEM backup — exatamente o caminho de perda que o estado Corrupt existe para
        // fechar, aberto pelo unico caso que nao passava por ele.
        const QJsonValue SlotsV = Root.value(QStringLiteral("slots"));
        if (!SlotsV.isArray()) {
            if (_OutMessage) *_OutMessage = QStringLiteral("arquivo sem lista de cameras");
            return EState::Corrupt;
        }

        bool  AnyRejected = false;
        FSlot Parsed[kSlotCount];

        for (const QJsonValue& V : SlotsV.toArray()) {
            if (!V.isObject()) { AnyRejected = true; continue; }
            const QJsonObject O = V.toObject();
            const QJsonValue IdxV = O.value(QStringLiteral("index"));
            if (!IdxV.isDouble()) { AnyRejected = true; continue; }
            const int Idx = IdxV.toInt(-1);
            if (!ValidSlot(Idx)) { AnyRejected = true; continue; }

            // Parseia em TEMPORARIO e so publica se TUDO passar. A versao anterior marcava
            // Filled = true antes de ler, entao uma chave ausente virava 0.0 e o slot nascia
            // valido apontando para a origem do mundo.
            double PX, PY, PZ, Pitch, Yaw, Fov;
            if (!ReadNum(O, "posX", PX) || !ReadNum(O, "posY", PY) || !ReadNum(O, "posZ", PZ) ||
                !ReadNum(O, "pitch", Pitch) || !ReadNum(O, "yaw", Yaw) ||
                !ReadNum(O, "fovYDeg", Fov)) {
                AnyRejected = true;
                continue;
            }
            if (std::abs(PX) > kMaxAbsPosition || std::abs(PY) > kMaxAbsPosition ||
                std::abs(PZ) > kMaxAbsPosition ||
                std::abs(Pitch) > kMaxAbsPitch || std::abs(Yaw) > kMaxAbsYaw) {
                AnyRejected = true;
                continue;
            }
            // FOV que esta build nao reproduz: a pose sairia certa e o enquadramento errado, e
            // duas capturas com enquadramentos diferentes nao comparam nada. Rejeitar e o unico
            // desfecho honesto enquanto o FOV nao for restauravel.
            if (std::abs(Fov - static_cast<double>(_FovNow)) > 0.01) {
                AnyRejected = true;
                continue;
            }

            FSlot& S = Parsed[Idx];
            S.Filled  = true;
            S.PosX    = static_cast<float>(PX);
            S.PosY    = static_cast<float>(PY);
            S.PosZ    = static_cast<float>(PZ);
            S.Pitch   = static_cast<float>(Pitch);
            S.Yaw     = static_cast<float>(Yaw);
            S.FovYDeg = static_cast<float>(Fov);
        }

        for (int I = 0; I < kSlotCount; ++I) _OutSlots[I] = Parsed[I];
        if (AnyRejected) {
            if (_OutMessage)
                *_OutMessage = QStringLiteral("uma ou mais cameras nao puderam ser lidas");
            return EState::Incompatible;
        }
        return EState::Ok;
    }
}
