#include "SmileEditor/Scene/CameraBookmarksBridge.h"
#include "SmileEditor/Scene/JsonSidecar.h"

#include "Smile/Graphics/Renderer/Renderer.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace SmileEditor {
    namespace {
        // Versao do SCHEMA do sidecar, independente do kCookedVersion.
        constexpr int kSchemaVersion = 1;

        // Faixas plausiveis. Nao sao gosto: o sidecar e um arquivo de texto que entra no
        // repositorio e vai ser editado a mao, e um pitch de 3000 graus ou um NaN vindo dali
        // viraria uma matriz de view degenerada — que aparece como tela preta ou lixo, longe
        // daqui e sem nada apontando para o arquivo.
        constexpr double kMaxAbsPosition = 1.0e6; // muito alem de qualquer cena da engine
        // 89, e nao 90: e o clamp da propria FCamera (Camera.cpp, Clamp(..., -89, 89)). Em 90 o
        // forward fica paralelo ao up e a matriz de view degenera — ou seja, aceitar 90 aqui seria
        // aceitar uma pose que a camera nao consegue produzir e nao consegue reproduzir.
        constexpr double kMaxAbsPitch    = 89.0;
        constexpr double kMaxAbsYaw      = 3600.0; // yaw acumula voltas; so barra o absurdo

        // Le um numero OBRIGATORIO. Chave ausente e erro, e nao zero: o toDouble() sem esta
        // checagem transformava sidecar truncado em pose (0,0,0) — um valor plausivel, gravavel e
        // errado, que e o pior resultado possivel para um arquivo de referencia.
        bool ReadNum(const QJsonObject& _O, const char* _Key, double& _Out) {
            const QJsonValue V = _O.value(QLatin1String(_Key));
            if (!V.isDouble()) return false;
            _Out = V.toDouble();
            return std::isfinite(_Out);
        }
    }

    CameraBookmarksBridge::CameraBookmarksBridge(QObject* _Parent) : QObject(_Parent) {}

    float CameraBookmarksBridge::EngineFovYDeg() const {
        // Fonte unica: o proprio Renderer. Repetir "60.0f" aqui faria o dia em que o FOV virar
        // knob passar despercebido justamente nesta comparacao.
        if (auto Access = Renderer.Lock())
            return Access->GetFovY() * (180.0f / 3.14159265358979323846f);
        return 60.0f;
    }

    void CameraBookmarksBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (_Additive) return; // bookmarks pertencem a cena PRINCIPAL (regra do MaterialsBridge)

        const QFileInfo Info(_ScenePath);
        JsonPath = Info.path() + "/" + Info.completeBaseName() + ".cameras.json";
        for (FSlot& S : Slots) S = FSlot{};
        State = ESidecarState::None;
        LoadFromDisk();
        emit SlotsChanged();
    }

    QVariantList CameraBookmarksBridge::SlotsFilled() const {
        QVariantList Out;
        for (const FSlot& S : Slots) Out.append(S.Filled);
        return Out;
    }

    QVariantList CameraBookmarksBridge::SlotLabels() const {
        QVariantList Out;
        for (int i = 0; i < kSlotCount; ++i) Out.append(SlotLabel(i));
        return Out;
    }

    QString CameraBookmarksBridge::SidecarWarning() const {
        switch (State) {
            case ESidecarState::Corrupt:
                return tr("O arquivo de câmeras não é JSON válido. Gravar cria um .bak antes de "
                          "substituí-lo.");
            case ESidecarState::Incompatible:
                return tr("O arquivo de câmeras é de outra versão, ou tem FOV que esta build não "
                          "reproduz. Gravar cria um .bak antes de substituí-lo.");
            default:
                return {};
        }
    }

    bool CameraBookmarksBridge::HasSlot(int _Slot) const {
        return ValidSlot(_Slot) && Slots[_Slot].Filled;
    }

    QString CameraBookmarksBridge::SlotLabel(int _Slot) const {
        if (!HasSlot(_Slot)) return {};
        const FSlot& S = Slots[_Slot];
        return QStringLiteral("x=%1 y=%2 z=%3 · p=%4 y=%5")
            .arg(S.PosX, 0, 'f', 2).arg(S.PosY, 0, 'f', 2).arg(S.PosZ, 0, 'f', 2)
            .arg(S.Pitch, 0, 'f', 1).arg(S.Yaw, 0, 'f', 1);
    }

    bool CameraBookmarksBridge::Save(int _Slot) {
        if (!ValidSlot(_Slot)) return false;
        // Estes dois guards eram um `return false` mudo, e mudo aqui e o pior caso: o botao
        // simplesmente nao fazia nada, sem nada em lugar nenhum dizendo por que. E o mesmo defeito
        // que motivou ligar o Message a barra de status — so que na porta de entrada, onde a
        // funcao desiste antes de chegar la.
        if (JsonPath.isEmpty()) {
            emit Message(tr("Nenhuma cena carregada — não há onde gravar a câmera"));
            return false;
        }
        if (!Renderer) {
            emit Message(tr("Renderizador indisponível — câmera não gravada"));
            return false;
        }

        // Um único lock garante que posição, orientação e FOV pertençam ao mesmo frame.
        FSlot Staged{};
        {
            auto Access = Renderer.Lock();
            if (!Access) return false;
            const Smile::Vec3 P = Access->GetCameraPos();
            Staged.Filled  = true;
            Staged.PosX    = P.X; Staged.PosY = P.Y; Staged.PosZ = P.Z;
            Staged.Pitch   = Access->GetPitch();
            Staged.Yaw     = Access->GetYaw();
            Staged.FovYDeg = Access->GetFovY() * (180.0f / 3.14159265358979323846f);
        }

        // STAGING: so publica na memoria depois que o disco aceitou. Publicar antes deixaria a UI
        // afirmando uma referencia que o proximo load nao teria — e como bookmark so e conferido
        // dias depois, a divergencia so apareceria quando ja nao houvesse como recuperar a pose.
        const FSlot Previous = Slots[_Slot];
        Slots[_Slot] = Staged;
        if (!SaveToDisk()) {
            Slots[_Slot] = Previous; // rollback
            emit Message(tr("Falha ao gravar a câmera %1 — nada foi alterado").arg(_Slot + 1));
            emit SlotsChanged();
            return false;
        }

        emit SlotsChanged();
        emit Message(tr("Câmera %1 gravada").arg(_Slot + 1));
        return true;
    }

    bool CameraBookmarksBridge::Restore(int _Slot) {
        if (!HasSlot(_Slot) || !Renderer) return false;
        const FSlot& S = Slots[_Slot];
        // SetCameraPose ja emite o corte de camera (Renderer.cpp). Nao invalidar de novo aqui:
        // duplicar o reset dos filtros de tela custaria um frame aliasado a mais sem ganho.
        Renderer->SetCameraPose(Smile::Vec3{ S.PosX, S.PosY, S.PosZ }, S.Pitch, S.Yaw);
        emit Message(tr("Câmera %1 restaurada").arg(_Slot + 1));
        return true;
    }

    bool CameraBookmarksBridge::Clear(int _Slot) {
        if (!ValidSlot(_Slot) || JsonPath.isEmpty()) return false;
        const FSlot Previous = Slots[_Slot];
        Slots[_Slot] = FSlot{};
        if (!SaveToDisk()) {
            Slots[_Slot] = Previous; // mesmo staging/rollback do Save
            emit Message(tr("Falha ao gravar — a câmera %1 continua no lugar").arg(_Slot + 1));
            emit SlotsChanged();
            return false;
        }
        emit SlotsChanged();
        return true;
    }

    // ---- Persistencia (<cena>.cameras.json) ----------------------------------------------
    bool CameraBookmarksBridge::LoadFromDisk() {
        QFile F(JsonPath);
        if (!F.exists()) { State = ESidecarState::None; return false; } // cena nova: normal
        if (!F.open(QIODevice::ReadOnly)) {
            State = ESidecarState::Corrupt; // existe e nao abriu: NAO tratar como "vazio"
            return false;
        }

        QJsonParseError Err{};
        const QJsonDocument Doc = QJsonDocument::fromJson(F.readAll(), &Err);
        F.close();
        if (Err.error != QJsonParseError::NoError || !Doc.isObject()) {
            State = ESidecarState::Corrupt;
            emit Message(tr("Bookmarks: JSON inválido — o arquivo será preservado como .bak"));
            return false;
        }

        const QJsonObject Root = Doc.object();
        if (Root.value(QStringLiteral("version")).toInt(0) != kSchemaVersion) {
            State = ESidecarState::Incompatible;
            emit Message(tr("Bookmarks: versão de schema desconhecida — arquivo preservado"));
            return false;
        }

        // `slots` TEM de ser array. Sem esta checagem, uma chave ausente (ou de outro tipo) virava
        // array vazio pelo toArray(), o estado ia para Ok e a gravacao seguinte substituia o
        // arquivo SEM backup — exatamente o caminho de perda que o estado Corrupt existe para
        // fechar, aberto pelo unico caso que nao passava por ele.
        const QJsonValue SlotsV = Root.value(QStringLiteral("slots"));
        if (!SlotsV.isArray()) {
            State = ESidecarState::Corrupt;
            emit Message(tr("Bookmarks: arquivo sem lista de câmeras — será preservado como .bak"));
            return false;
        }

        const float FovNow = EngineFovYDeg();
        bool AnyRejected = false;
        FSlot Parsed[kSlotCount];

        const QJsonArray Arr = SlotsV.toArray();
        for (const QJsonValue& V : Arr) {
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
            if (std::abs(Fov - static_cast<double>(FovNow)) > 0.01) {
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

        for (int i = 0; i < kSlotCount; ++i) Slots[i] = Parsed[i];
        // Um slot recusado nao invalida o arquivo inteiro, mas tambem nao pode virar Ok em
        // silencio: gravar por cima apagaria a entrada que esta build nao entendeu.
        State = AnyRejected ? ESidecarState::Incompatible : ESidecarState::Ok;
        if (AnyRejected)
            emit Message(tr("Bookmarks: uma ou mais câmeras não puderam ser lidas — arquivo preservado"));
        return true;
    }

    bool CameraBookmarksBridge::BackupUnreadableSidecar() {
        const QString BakPath = JsonPath + QStringLiteral(".bak");
        QFile::remove(BakPath);               // um .bak so; o interessante e o ULTIMO nao entendido
        // COPIA, e nao rename. Renomear tira o original do caminho canonico ANTES de o novo
        // existir: se o open ou o commit falharem depois, a cena fica sem sidecar nenhum — o
        // backup teria criado a perda que ele existe para evitar. Copiando, o original so
        // desaparece no rename atomico do QSaveFile::commit(), quando o novo ja esta completo.
        if (!QFile::copy(JsonPath, BakPath)) return false;
        emit Message(tr("Arquivo de câmeras anterior preservado em %1")
                         .arg(QFileInfo(BakPath).fileName()));
        return true;
    }

    bool CameraBookmarksBridge::SaveToDisk() {
        if (JsonPath.isEmpty()) return false;

        // Preserve sidecars desconhecidos; State só vira Ok depois de um commit bem-sucedido.
        if (State == ESidecarState::Corrupt || State == ESidecarState::Incompatible) {
            if (!BackupUnreadableSidecar()) return false;
        }

        QJsonArray Arr;
        for (int i = 0; i < kSlotCount; ++i) {
            const FSlot& S = Slots[i];
            if (!S.Filled) continue;             // slot vazio simplesmente nao aparece
            QJsonObject O;
            O[QStringLiteral("index")]   = i;
            O[QStringLiteral("posX")]    = S.PosX;
            O[QStringLiteral("posY")]    = S.PosY;
            O[QStringLiteral("posZ")]    = S.PosZ;
            O[QStringLiteral("pitch")]   = S.Pitch;
            O[QStringLiteral("yaw")]     = S.Yaw;
            O[QStringLiteral("fovYDeg")] = S.FovYDeg;
            Arr.append(O);
        }

        QJsonObject Root;
        Root[QStringLiteral("version")] = kSchemaVersion;
        Root[QStringLiteral("slots")]   = Arr;

        // Escrita atomica compartilhada (ver JsonSidecar.h). Esta era a UNICA das cinco copias
        // que estava correta; virou a funcao para que a sexta nasca certa por construcao.
        if (!WriteJsonSidecar(JsonPath, Root, "Bookmarks")) return false;
        State = ESidecarState::Ok;
        return true;
    }
}
