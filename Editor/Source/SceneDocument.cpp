#include "SmileEditor/SceneDocument.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/Logger.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace SmileEditor {
    namespace {
        constexpr int kMapVersion = 1;

        QJsonArray Vec3ToJson(const Smile::Vec3& V) {
            return QJsonArray{ V.X, V.Y, V.Z };
        }
        Smile::Vec3 JsonToVec3(const QJsonArray& A, const Smile::Vec3& Def) {
            if (A.size() != 3) return Def;
            return Smile::Vec3{ (float)A.at(0).toDouble(Def.X),
                                (float)A.at(1).toDouble(Def.Y),
                                (float)A.at(2).toDouble(Def.Z) };
        }
        bool NearlyEqual(const Smile::Vec3& A, const Smile::Vec3& B) {
            // Tolerancia frouxa de proposito: o transform vem de uma decomposicao em float, e
            // gravar um "override" por causa de 1e-7 de ruido encheria o .smap de entradas que
            // nao descrevem edicao nenhuma.
            constexpr float kEps = 1e-5f;
            return std::fabs(A.X - B.X) < kEps && std::fabs(A.Y - B.Y) < kEps &&
                   std::fabs(A.Z - B.Z) < kEps;
        }
    }

    SceneDocument::SceneDocument(QObject* _Parent) : QObject(_Parent) {}

    void SceneDocument::SetRenderer(RendererHandle _R) { Renderer = _R; }

    void SceneDocument::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        // Rastro do caminho da camada autorada em toda carga. Barato (uma linha por cena) e paga
        // caro: quando o save nao acontecia, a ausencia de "Mapa salvo" nao dizia se o problema
        // era o arquivo, o renderer ou o botao — e era o botao (a propriedade sceneDoc nao estava
        // declarada no QML, entao a chamada virava ReferenceError e morria calada).
        Smile::LogDebug("Mapa: carga de cena '" + _ScenePath.toStdString() +
                        "' aditiva=" + (_Additive ? "sim" : "nao") +
                        " renderer=" + (Renderer ? "pronto" : "ausente"));
        if (!Renderer) return;
        // Carga aditiva empilha outro asset na mesma cena; o .smap acompanha a cena PRINCIPAL,
        // igual ao .lights.json e ao .visibility.json.
        if (_Additive) return;

        const QFileInfo Info(_ScenePath);
        MapPath = Info.path() + "/" + Info.completeBaseName() + ".smap";
        if (IsDirty) { IsDirty = false; emit DirtyChanged(); }

        // Baseline ANTES de aplicar o .smap — e a foto do que o asset trouxe.
        {
            auto Access = Renderer.Lock();
            if (!Access) return;
            const auto& List = Access->GetScene().Renderables();
            CookedCount = 0;
            Baseline.clear();
            for (const Smile::FRenderable& R : List) {
                if (R.CookedIndex < 0) continue;
                if (R.CookedIndex >= CookedCount) CookedCount = R.CookedIndex + 1;
                if (Baseline.size() <= R.CookedIndex) Baseline.resize(R.CookedIndex + 1);
                Baseline[R.CookedIndex] = { R.Transform.Position, R.Transform.RotationEuler,
                                            R.Transform.Scale };
            }
        }

        QFile File(MapPath);
        if (!File.exists()) { emit DirtyChanged(); return; }
        if (!File.open(QIODevice::ReadOnly | QIODevice::Text)) {
            Smile::LogWarning("Mapa: nao foi possivel abrir " + MapPath.toStdString());
            emit DirtyChanged();
            return;
        }
        const QString Json = QString::fromUtf8(File.readAll());
        File.close();
        if (Apply(Json)) emit Applied();
        emit DirtyChanged();
    }

    bool SceneDocument::Apply(const QString& _Json) {
        QJsonParseError Err{};
        const QJsonDocument Doc = QJsonDocument::fromJson(_Json.toUtf8(), &Err);
        if (Doc.isNull() || !Doc.isObject()) {
            Smile::LogError("Mapa: JSON invalido em " + MapPath.toStdString() + " (" +
                            Err.errorString().toStdString() + ")");
            return false;
        }
        const QJsonObject Root = Doc.object();
        if (Root.value(QStringLiteral("version")).toInt(0) != kMapVersion) {
            Smile::LogWarning("Mapa: versao diferente da suportada; ignorado");
            return false;
        }

        auto Access = Renderer.Lock();
        if (!Access) return false;
        auto& Scene = Access->GetScene();

        // Indice do cozido -> posicao atual na lista. Montado UMA vez: aplicar N edicoes com
        // busca linear seria O(N*M) numa cena de milhares de objetos.
        QHash<int, int> ByCooked;
        {
            const auto& List = Scene.Renderables();
            for (int i = 0; i < (int)List.size(); ++i)
                if (!List[i].Spawned && List[i].CookedIndex >= 0)
                    ByCooked.insert(List[i].CookedIndex, i);
        }

        int Hidden = 0, Moved = 0, Spawned = 0, Removed = 0, Dynamic = 0;

        // 1) Overrides sobre objetos que vieram do asset.
        for (const QJsonValue& V : Root.value(QStringLiteral("overrides")).toArray()) {
            const QJsonObject O = V.toObject();
            const int Cooked = O.value(QStringLiteral("cooked")).toInt(-1);
            const auto It = ByCooked.constFind(Cooked);
            if (It == ByCooked.constEnd()) continue; // o asset mudou; ignora em silencio
            Smile::FRenderable& R = Scene.Renderables()[(size_t)It.value()];
            if (O.contains(QStringLiteral("visible"))) {
                R.Visible = O.value(QStringLiteral("visible")).toBool(true);
                if (!R.Visible) ++Hidden;
            }
            if (O.contains(QStringLiteral("pos"))) {
                R.Transform.Position = JsonToVec3(O.value(QStringLiteral("pos")).toArray(),
                                                  R.Transform.Position);
                R.Transform.RotationEuler = JsonToVec3(O.value(QStringLiteral("rot")).toArray(),
                                                       R.Transform.RotationEuler);
                R.Transform.Scale = JsonToVec3(O.value(QStringLiteral("scale")).toArray(),
                                               R.Transform.Scale);
                R.RefreshWorldBounds();
                ++Moved;
            }
            // Ausente = estatico (o default). So aparece no arquivo quem foi marcado.
            if (O.value(QStringLiteral("dynamic")).toBool(false)) {
                R.Mobility = Smile::EMobility::Dynamic;
                ++Dynamic;
            }
        }

        // 2) Objetos criados no editor: recria duplicando a fonte, que e o que a copia e.
        for (const QJsonValue& V : Root.value(QStringLiteral("spawned")).toArray()) {
            const QJsonObject O = V.toObject();
            const auto It = ByCooked.constFind(O.value(QStringLiteral("from")).toInt(-1));
            if (It == ByCooked.constEnd()) continue;
            const Smile::u64 SourceId = Scene.Renderables()[(size_t)It.value()].Id;
            Smile::FRenderable* Copy = Scene.DuplicateRenderable(SourceId);
            if (!Copy) continue;
            Copy->Name = O.value(QStringLiteral("name")).toString(
                             QString::fromStdString(Copy->Name)).toStdString();
            Copy->Visible = O.value(QStringLiteral("visible")).toBool(true);
            Copy->Transform.Position = JsonToVec3(O.value(QStringLiteral("pos")).toArray(),
                                                  Copy->Transform.Position);
            Copy->Transform.RotationEuler = JsonToVec3(O.value(QStringLiteral("rot")).toArray(),
                                                       Copy->Transform.RotationEuler);
            Copy->Transform.Scale = JsonToVec3(O.value(QStringLiteral("scale")).toArray(),
                                               Copy->Transform.Scale);
            Copy->RefreshWorldBounds();
            if (O.value(QStringLiteral("dynamic")).toBool(false))
                Copy->Mobility = Smile::EMobility::Dynamic;
            ++Spawned;
        }

        // 3) Apagados. POR ULTIMO de proposito: remover embaralha os indices vivos, e o mapa
        // ByCooked acima ficaria podre para os passos 1 e 2.
        {
            QVector<Smile::u64> ToRemove;
            const QJsonArray Deleted = Root.value(QStringLiteral("deleted")).toArray();
            for (const QJsonValue& V : Deleted) {
                const auto It = ByCooked.constFind(V.toInt(-1));
                if (It != ByCooked.constEnd())
                    ToRemove.push_back(Scene.Renderables()[(size_t)It.value()].Id);
            }
            // Coleta os Id ANTES de remover qualquer um: a remocao invalida os indices, mas nao
            // as identidades.
            for (Smile::u64 Id : ToRemove)
                if (Access->RemoveRenderable(Id)) ++Removed;
        }

        Smile::LogInfo("Mapa aplicado: " + std::to_string(Moved) + " movidos, " +
                       std::to_string(Hidden) + " ocultos, " + std::to_string(Spawned) +
                       " criados, " + std::to_string(Removed) + " apagados (" +
                       QFileInfo(MapPath).fileName().toStdString() + ")");
        return true;
    }

    void SceneDocument::markDirty() {
        if (IsDirty) return;
        IsDirty = true;
        emit DirtyChanged();
    }

    bool SceneDocument::save() {
        // Nenhum caminho de saida daqui pode ser mudo: isto e uma acao do USUARIO. A primeira
        // versao retornava false em silencio, e quando o save nao aconteceu (a propriedade
        // sceneDoc nao estava declarada no QML) o log nao tinha uma linha sequer para mostrar
        // onde parou — o unico sinal era a ausencia do "Mapa salvo".
        if (!Renderer) {
            Smile::LogError("Mapa: sem renderer; nada salvo");
            return false;
        }
        if (MapPath.isEmpty()) {
            Smile::LogError("Mapa: nenhuma cena carregada; nada salvo");
            return false;
        }
        auto Access = Renderer.Lock();
        if (!Access) {
            Smile::LogError("Mapa: renderer ocupado; nada salvo");
            return false;
        }
        const auto& List = Access->GetScene().Renderables();

        QJsonArray Overrides, Spawned;
        // Todo indice do cozido comeca "presente"; o que sobrar sem dono foi apagado.
        QSet<int> Present;

        for (const Smile::FRenderable& R : List) {
            if (R.CookedIndex < 0) continue; // proxy do terreno e afins: nao sao do asset

            const bool IsDynamic = R.Mobility == Smile::EMobility::Dynamic;

            QJsonObject O;
            O[QStringLiteral("pos")]   = Vec3ToJson(R.Transform.Position);
            O[QStringLiteral("rot")]   = Vec3ToJson(R.Transform.RotationEuler);
            O[QStringLiteral("scale")] = Vec3ToJson(R.Transform.Scale);
            if (!R.Visible) O[QStringLiteral("visible")] = false;
            // Só o que diverge do default vai para o arquivo, como a visibilidade: estático é
            // o default e a ausência da chave já o descreve.
            if (IsDynamic) O[QStringLiteral("dynamic")] = true;

            if (R.Spawned) {
                O[QStringLiteral("from")] = R.CookedIndex;
                O[QStringLiteral("name")] = QString::fromStdString(R.Name);
                Spawned.append(O);
                continue;
            }

            Present.insert(R.CookedIndex);
            // So grava override se algo REALMENTE mudou em relacao ao que o asset trouxe.
            const bool HasBaseline = R.CookedIndex < Baseline.size();
            const bool MovedByUser =
                !HasBaseline ||
                !(NearlyEqual(R.Transform.Position, Baseline[R.CookedIndex].Position) &&
                  NearlyEqual(R.Transform.RotationEuler, Baseline[R.CookedIndex].Rotation) &&
                  NearlyEqual(R.Transform.Scale, Baseline[R.CookedIndex].Scale));
            // Mobilidade entra na condicao junto com transform e visibilidade: um objeto
            // marcado como dinamico e um override legitimo mesmo parado e visivel, e sem isto
            // a marcacao seria perdida no save.
            if (!MovedByUser && R.Visible && !IsDynamic) continue;
            if (!MovedByUser) {
                O.remove(QStringLiteral("pos"));
                O.remove(QStringLiteral("rot"));
                O.remove(QStringLiteral("scale"));
            }
            O[QStringLiteral("cooked")] = R.CookedIndex;
            Overrides.append(O);
        }

        QJsonArray Deleted;
        for (int i = 0; i < CookedCount; ++i)
            if (!Present.contains(i)) Deleted.append(i);

        QJsonObject Root;
        Root[QStringLiteral("version")]   = kMapVersion;
        Root[QStringLiteral("overrides")] = Overrides;
        Root[QStringLiteral("spawned")]   = Spawned;
        Root[QStringLiteral("deleted")]   = Deleted;

        QFile File(MapPath);
        if (!File.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            Smile::LogError("Mapa: nao foi possivel gravar " + MapPath.toStdString());
            return false;
        }
        File.write(QJsonDocument(Root).toJson(QJsonDocument::Indented));
        File.close();

        Smile::LogInfo("Mapa salvo: " + std::to_string(Overrides.size()) + " overrides, " +
                       std::to_string(Spawned.size()) + " criados, " +
                       std::to_string(Deleted.size()) + " apagados -> " +
                       QFileInfo(MapPath).fileName().toStdString());
        if (IsDirty) { IsDirty = false; emit DirtyChanged(); }
        return true;
    }
}
