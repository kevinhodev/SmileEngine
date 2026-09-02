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
    // Schema, faixas plausiveis e validacao vivem em BookmarkSchema.h — inclusive o porque de
    // cada limite (pitch em 89 e nao 90 por causa do clamp da FCamera, etc.). Ficam la, e nao
    // aqui, para o gate poder validar o sidecar com o MESMO codigo que o editor usa ao carregar.

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
        const QByteArray Bytes = F.readAll();
        F.close();

        // Regras no BookmarkSchema: o gate valida o sidecar com ESTE parser, e nao com uma
        // reimplementacao que divergiria em silencio.
        QString Msg;
        State = BookmarkSchema::Parse(Bytes, EngineFovYDeg(), Slots, &Msg);
        if (State == ESidecarState::Corrupt) {
            emit Message(tr("Bookmarks: %1 — o arquivo será preservado como .bak").arg(Msg));
            return false;
        }
        if (State == ESidecarState::Incompatible) {
            emit Message(tr("Bookmarks: %1 — arquivo preservado").arg(Msg));
            // Schema desconhecido nao entrega poses; slot rejeitado individualmente sim.
            // Laco sobre kSlotCount e nao os quatro escritos a mao: com a contagem enumerada,
            // aumentar o schema ignoraria os slots novos e diminui-lo leria fora do array.
            for (int I = 0; I < kSlotCount; ++I)
                if (Slots[I].Filled) return true;
            return false;
        }
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
        Root[QStringLiteral("version")] = BookmarkSchema::kSchemaVersion;
        Root[QStringLiteral("slots")]   = Arr;

        // Escrita atomica compartilhada (ver JsonSidecar.h). Esta era a UNICA das cinco copias
        // que estava correta; virou a funcao para que a sexta nasca certa por construcao.
        if (!WriteJsonSidecar(JsonPath, Root, "Bookmarks")) return false;
        State = ESidecarState::Ok;
        return true;
    }
}
