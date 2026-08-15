#include "SmileEditor/SceneOutlinerBridge.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/Logger.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace SmileEditor {
    namespace {
        // Ambiente e uma lista fixa; o Terreno so entra quando ha um carregado.
        struct FEnvDef { int Id; const char* Name; const char* Icon; };
        constexpr FEnvDef kEnvDefs[] = {
            { SceneOutlinerBridge::EnvSol,     "Sol & Atmosfera",     "sun"      },
            { SceneOutlinerBridge::EnvNuvens,  "Nuvens Volumetricas", "cloud"    },
            { SceneOutlinerBridge::EnvOceano,  "Oceano FFT",          "waves"    },
            { SceneOutlinerBridge::EnvTerreno, "Terreno",             "mountain" },
        };

        constexpr double kToRad = 3.14159265358979 / 180.0;
    }

    SceneOutlinerBridge::SceneOutlinerBridge(QObject* _Parent)
        : QAbstractListModel(_Parent) {}

    void SceneOutlinerBridge::SetRenderer(RendererHandle _Renderer) {
        Renderer = _Renderer;
        if (Renderer) {
            CachedSelMesh  = Renderer->GetSelectedObject();
            CachedSelLight = Renderer->GetSelectedLight();
            CachedClouds    = Renderer->Settings().GetUseClouds();
            CachedOcean     = Renderer->Settings().GetUseWater();
            CachedTerrain   = Renderer->GetTerrain().IsLoaded();
            CachedTerrainOn = Renderer->Settings().GetUseTerrain();
        }
        Rebuild();
        emit AvailableChanged();
        emit EnvChanged();
        emit SelectionChanged();
    }

    void SceneOutlinerBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (!Renderer) return;
        const int Count = (int)Renderer->GetScene().Renderables().size();
        const QFileInfo Info(_ScenePath);
        if (!_Additive) {
            Assets.clear();
            AssetExpanded.clear();
            Assets.push_back({ Info.completeBaseName(), 0, Count });
            AssetExpanded.push_back(true);
            // Persistencia acompanha a cena principal (mesma convencao do .lights.json).
            JsonPath = Info.path() + "/" + Info.completeBaseName() + ".visibility.json";
            if (VisDirty) { VisDirty = false; emit DirtyChanged(); }
        } else {
            const int Begin = Assets.isEmpty() ? 0 : Assets.last().End;
            Assets.push_back({ Info.completeBaseName(), Begin, Count });
            AssetExpanded.push_back(true);
        }
        LoadVisibility(); // aplica ocultas/terreno do json da cena principal (idempotente)
        Rebuild();
    }

    // ---- Modelo ----
    int SceneOutlinerBridge::rowCount(const QModelIndex& _Parent) const {
        return _Parent.isValid() ? 0 : (int)Rows.size();
    }

    QVariant SceneOutlinerBridge::data(const QModelIndex& _Index, int _Role) const {
        const int I = _Index.row();
        if (I < 0 || I >= Rows.size()) return {};
        const FRow& R = Rows[I];
        switch (_Role) {
            case RKind:     return R.Kind;
            case RDepth:    return R.Depth;
            case RName:     return R.Name;
            case RIcon:     return R.Icon;
            case RCount:    return R.Count;
            case RExpanded: return R.Expanded;
            case REnabled:  return R.Enabled;
            case RHasEye:   return R.HasEye;
            case RDotColor: return R.Dot.isValid() ? QVariant(R.Dot) : QVariant(QColor(Qt::white));
            case RHasDot:   return R.Dot.isValid();
            case RSelected: return RowSelected(R);
            case RSceneIdx: return R.SceneIdx;
        }
        return {};
    }

    QHash<int, QByteArray> SceneOutlinerBridge::roleNames() const {
        return {
            { RKind,     "kind"     }, { RDepth,    "depth"    }, { RName,   "name"   },
            { RIcon,     "icon"     }, { RCount,    "count"    }, { RExpanded, "expanded" },
            { REnabled,  "rowEnabled" }, { RHasEye, "hasEye"   }, { RDotColor, "dotColor" },
            { RHasDot,   "hasDot"   }, { RSelected, "selected" }, { RSceneIdx, "sceneIdx" },
        };
    }

    bool SceneOutlinerBridge::RowSelected(const FRow& _Row) const {
        if (!Renderer) return false;
        if (_Row.Kind == KLight) return Renderer->GetSelectedLight() == _Row.SceneIdx;
        if (_Row.Kind == KMesh)  return Renderer->GetSelectedObject() == _Row.SceneIdx;
        return false;
    }

    bool SceneOutlinerBridge::MatchesSearch(const QString& _Name) const {
        return SearchText.isEmpty() || _Name.contains(SearchText, Qt::CaseInsensitive);
    }

    // Monta a lista flat respeitando filtro (chips), busca e expand/colapso. Durante a busca
    // tudo fica "expandido" e so entram folhas que casam (com seus pais).
    void SceneOutlinerBridge::RebuildRows(QVector<FRow>& _Out) const {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        const auto& Scene = RendererAccess->GetScene();
        const bool Searching = !SearchText.isEmpty();
        const auto GroupOn = [this](int G) {
            return FilterGroup == 0 || FilterGroup == G + 1;
        };

        // ---- Ambiente ----
        if (GroupOn(GAmbiente)) {
            const bool TerrainLoaded = Renderer->GetTerrain().IsLoaded();
            QVector<FRow> Children;
            for (const FEnvDef& E : kEnvDefs) {
                if (E.Id == EnvTerreno && !TerrainLoaded) continue;
                if (!MatchesSearch(QLatin1String(E.Name))) continue;
                FRow R;
                R.Kind     = KEnv;
                R.Depth    = 1;
                R.Name     = QLatin1String(E.Name);
                R.Icon     = QLatin1String(E.Icon);
                R.SceneIdx = E.Id;
                // Olho: o estado vem por binding direto no QML (viewportModel.cloudsEnabled /
                // oceanVisible / terrainVisible), REnabled aqui e so fallback.
                R.HasEye   = (E.Id != EnvSol);
                R.Enabled  = (E.Id == EnvNuvens)  ? Renderer->Settings().GetUseClouds()
                           : (E.Id == EnvOceano)  ? Renderer->Settings().GetUseWater()
                           : (E.Id == EnvTerreno) ? Renderer->Settings().GetUseTerrain() : true;
                Children.push_back(R);
            }
            if (!Searching || !Children.isEmpty()) {
                FRow G;
                G.Kind     = KGroup;
                G.Name     = QStringLiteral("Ambiente");
                G.Icon     = QStringLiteral("globe");
                G.Count    = Children.size();
                G.Expanded = Searching || GroupExpanded[GAmbiente];
                G.SceneIdx = GAmbiente;
                _Out.push_back(G);
                if (G.Expanded) _Out += Children;
            }
        }

        // ---- Luzes ----
        if (GroupOn(GLuzes)) {
            const auto& Lights = Scene.Lights();
            QVector<FRow> Children;
            for (int I = 0; I < (int)Lights.size(); ++I) {
                const auto& L = Lights[(size_t)I];
                const QString Name = QString::fromStdString(L.Name);
                if (!MatchesSearch(Name)) continue;
                FRow R;
                R.Kind     = KLight;
                R.Depth    = 1;
                R.Name     = Name;
                R.Icon     = (L.Type == Smile::ELightType::Spot) ? QStringLiteral("cone")
                                                                 : QStringLiteral("lightbulb");
                R.SceneIdx = I;
                R.HasEye   = true;
                R.Enabled  = L.Enabled;
                R.Dot      = QColor::fromRgbF(std::clamp(L.Color.X, 0.0f, 1.0f),
                                              std::clamp(L.Color.Y, 0.0f, 1.0f),
                                              std::clamp(L.Color.Z, 0.0f, 1.0f));
                Children.push_back(R);
            }
            if (!Searching || !Children.isEmpty()) {
                FRow G;
                G.Kind     = KGroup;
                G.Name     = QStringLiteral("Luzes");
                G.Icon     = QStringLiteral("lightbulb");
                G.Count    = (int)Lights.size();
                G.Expanded = Searching || GroupExpanded[GLuzes];
                G.SceneIdx = GLuzes;
                _Out.push_back(G);
                if (G.Expanded) _Out += Children;
            }
        }

        // ---- Meshes (pastas por asset importado) ----
        if (GroupOn(GMeshes)) {
            const auto& Renderables = Scene.Renderables();
            const int Total = (int)Renderables.size();

            // Ranges por asset + range residual (meshes fora de qualquer carga conhecida).
            QVector<FAssetRange> Ranges = Assets;
            const int Known = Ranges.isEmpty() ? 0 : Ranges.last().End;
            if (Known < Total) Ranges.push_back({ QStringLiteral("Outros"), Known, Total });

            int MeshCount = 0;
            QVector<FRow> Folders; // pastas com filhos ja embutidos
            for (int A = 0; A < Ranges.size(); ++A) {
                const FAssetRange& Range = Ranges[A];
                QVector<FRow> Children;
                int RangeCount = 0;
                bool AnyVisible = false;
                for (int I = Range.Begin; I < std::min(Range.End, Total); ++I) {
                    const auto& R = Renderables[(size_t)I];
                    if (R.RaytracingOnly) continue;
                    ++RangeCount;
                    AnyVisible = AnyVisible || R.Visible;
                    const QString Name = QString::fromStdString(R.Name);
                    if (!MatchesSearch(Name)) continue;
                    FRow M;
                    M.Kind     = KMesh;
                    M.Depth    = 2;
                    M.Name     = Name;
                    M.Icon     = (R.Material && R.Material->Constants.ShadingModel == 1)
                               ? QStringLiteral("leaf") : QStringLiteral("box");
                    M.SceneIdx = I;
                    M.HasEye   = true;
                    M.Enabled  = R.Visible;
                    Children.push_back(M);
                }
                MeshCount += RangeCount;
                if (RangeCount == 0 || (Searching && Children.isEmpty())) continue;

                const bool Expanded = Searching || (A < AssetExpanded.size() ? AssetExpanded[A]
                                                                             : true);
                FRow F;
                F.Kind     = KAsset;
                F.Depth    = 1;
                F.Name     = Range.Name;
                F.Icon     = QStringLiteral("folder");
                F.Count    = RangeCount;
                F.Expanded = Expanded;
                F.SceneIdx = A; // indice em Assets (ou o residual, que nao colapsa persistente)
                F.HasEye   = true;
                F.Enabled  = AnyVisible;
                Folders.push_back(F);
                if (Expanded) Folders += Children;
            }

            if (!Searching || !Folders.isEmpty()) {
                FRow G;
                G.Kind     = KGroup;
                G.Name     = QStringLiteral("Meshes");
                G.Icon     = QStringLiteral("box");
                G.Count    = MeshCount;
                G.Expanded = Searching || GroupExpanded[GMeshes];
                G.SceneIdx = GMeshes;
                _Out.push_back(G);
                if (G.Expanded) _Out += Folders;
            }
        }
    }

    // Estrutura igual (kind/depth/sceneIdx linha a linha) -> so dataChanged, preservando o
    // scroll da ListView; qualquer diferenca -> reset completo.
    void SceneOutlinerBridge::ApplyRows(QVector<FRow>&& _NewRows) {
        bool SameShape = _NewRows.size() == Rows.size();
        if (SameShape) {
            for (int I = 0; I < Rows.size(); ++I) {
                const FRow& A = Rows[I];
                const FRow& B = _NewRows[I];
                if (A.Kind != B.Kind || A.Depth != B.Depth || A.SceneIdx != B.SceneIdx) {
                    SameShape = false;
                    break;
                }
            }
        }
        if (SameShape) {
            Rows = std::move(_NewRows);
            if (!Rows.isEmpty())
                emit dataChanged(index(0), index((int)Rows.size() - 1));
        } else {
            beginResetModel();
            Rows = std::move(_NewRows);
            endResetModel();
        }
    }

    void SceneOutlinerBridge::Rebuild() {
        QVector<FRow> NewRows;
        RebuildRows(NewRows);
        ApplyRows(std::move(NewRows));
        if (Renderer) {
            CachedRenderableCount = (int)Renderer->GetScene().Renderables().size();
            CachedLightCount      = (int)Renderer->GetScene().Lights().size();
            CachedTerrain         = Renderer->GetTerrain().IsLoaded();
        }
        emit StructureChanged();
    }

    // ---- Propriedades ----
    int SceneOutlinerBridge::TotalCount() const {
        if (!Renderer) return 0;
        auto RendererAccess = Renderer.Lock();
        const auto& Scene = RendererAccess->GetScene();
        int Meshes = 0;
        for (const auto& R : Scene.Renderables())
            if (!R.RaytracingOnly) ++Meshes;
        int Env = 3 + (Renderer->GetTerrain().IsLoaded() ? 1 : 0);
        return Meshes + (int)Scene.Lights().size() + Env;
    }

    int SceneOutlinerBridge::SelectedCount() const {
        if (!Renderer) return 0;
        return (Renderer->GetSelectedObject() >= 0 ? 1 : 0) +
               (Renderer->GetSelectedLight()  >= 0 ? 1 : 0);
    }

    int SceneOutlinerBridge::HiddenCount() const {
        if (!Renderer) return 0;
        auto RendererAccess = Renderer.Lock();
        const auto& Scene = RendererAccess->GetScene();
        int N = 0;
        for (const auto& L : Scene.Lights())
            if (!L.Enabled) ++N;
        for (const auto& R : Scene.Renderables())
            if (!R.RaytracingOnly && !R.Visible) ++N;
        return N;
    }

    void SceneOutlinerBridge::SetSearch(const QString& _V) {
        if (SearchText == _V) return;
        SearchText = _V;
        Rebuild();
        emit FiltersChanged();
    }

    void SceneOutlinerBridge::SetFilter(int _V) {
        const int Clamped = std::clamp(_V, 0, 3);
        if (FilterGroup == Clamped) return;
        FilterGroup = Clamped;
        Rebuild();
        emit FiltersChanged();
    }

    bool SceneOutlinerBridge::OceanVisible() const {
        return Renderer ? Renderer->Settings().GetUseWater() : false;
    }

    void SceneOutlinerBridge::SetOceanVisible(bool _V) {
        if (!Renderer || Renderer->Settings().GetUseWater() == _V) return;
        Renderer->Settings().SetUseWater(_V);
        CachedOcean = _V;
        Rebuild(); // REnabled da linha do oceano
        emit EnvChanged();
    }

    bool SceneOutlinerBridge::TerrainVisible() const {
        return Renderer ? Renderer->Settings().GetUseTerrain() : true;
    }

    void SceneOutlinerBridge::SetTerrainVisible(bool _V) {
        if (!Renderer || Renderer->Settings().GetUseTerrain() == _V) return;
        auto RendererAccess = Renderer.Lock();
        Renderer->Settings().SetUseTerrain(_V); // gates de prepass/G-buffer/CSM do FTerrain
        // O chao do GI e o proxy RaytracingOnly na TLAS — esconde junto, senao o DDGI
        // continua quicando luz num terreno invisivel.
        bool Bumped = false;
        for (auto& R : Renderer->GetScene().Renderables()) {
            if (R.RaytracingOnly && R.Visible != _V) {
                R.Visible = _V;
                Bumped = true;
            }
        }
        if (Bumped) Renderer->GetScene().BumpTransformsVersion();
        CachedTerrainOn = _V;
        MarkDirty();
        Rebuild();
        emit EnvChanged();
    }

    bool SceneOutlinerBridge::MeshSelected() const {
        return Renderer && Renderer->GetSelectedObject() >= 0;
    }

    class LockedSelectedMesh {
    public:
        explicit LockedSelectedMesh(const RendererHandle& _Renderer)
            : Access(_Renderer ? _Renderer.Lock() : RendererHandle::Access{}) {
            if (!Access) return;
            const auto& List = Access->GetScene().Renderables();
            const int Index = Access->GetSelectedObject();
            if (Index >= 0 && Index < static_cast<int>(List.size()))
                Value = &List[static_cast<size_t>(Index)];
        }

        explicit operator bool() const { return Value != nullptr; }
        const Smile::FRenderable* operator->() const { return Value; }

    private:
        RendererHandle::Access    Access;
        const Smile::FRenderable* Value = nullptr;
    };

    static LockedSelectedMesh SelMesh(const RendererHandle& _R) {
        return LockedSelectedMesh(_R);
    }

    QString SceneOutlinerBridge::MeshName() const {
        const auto R = SelMesh(Renderer);
        return R ? QString::fromStdString(R->Name) : QString();
    }

    QString SceneOutlinerBridge::MeshMaterial() const {
        const auto R = SelMesh(Renderer);
        if (!R || !R->Material) return {};
        return QString::fromStdString(R->Material->Name);
    }

    int SceneOutlinerBridge::MeshTris() const {
        const auto R = SelMesh(Renderer);
        return (R && R->Mesh) ? (int)(R->Mesh->GetIndexCount() / 3) : 0;
    }

    int SceneOutlinerBridge::MeshVerts() const {
        const auto R = SelMesh(Renderer);
        return (R && R->Mesh) ? (int)R->Mesh->VertexCount() : 0;
    }

    QString SceneOutlinerBridge::MeshVramText() const {
        const auto R = SelMesh(Renderer);
        if (!R || !R->Mesh) return {};
        const double Bytes = (double)R->Mesh->VertexCount() * R->Mesh->VertexStride() +
                             (double)R->Mesh->GetIndexCount() * sizeof(Smile::u32);
        const double KB = Bytes / 1024.0;
        if (KB < 1024.0)
            return QString::number(KB, 'f', 1).replace('.', ',') + QStringLiteral(" KB");
        return QString::number(KB / 1024.0, 'f', 2).replace('.', ',') + QStringLiteral(" MB");
    }

    QStringList SceneOutlinerBridge::MeshFlags() const {
        const auto R = SelMesh(Renderer);
        QStringList Flags;
        if (!R || !R->Material) return Flags;
        const auto* M = R->Material;
        if (M->Constants.ShadingModel == 1) Flags << QStringLiteral("Folhagem");
        if (M->TwoSided)                    Flags << QStringLiteral("Two-sided");
        if (M->Constants.AlphaTest)         Flags << QStringLiteral("Masked");
        if (M->Blend)                       Flags << QStringLiteral("Translúcido");
        return Flags;
    }

    bool SceneOutlinerBridge::MeshVisible() const {
        const auto R = SelMesh(Renderer);
        return R ? R->Visible : true;
    }

    bool SceneOutlinerBridge::MeshDynamic() const {
        const auto R = SelMesh(Renderer);
        return R && R->Mobility == Smile::EMobility::Dynamic;
    }

    void SceneOutlinerBridge::SetMeshDynamic(bool _V) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        const int Sel = Renderer->GetSelectedObject();
        auto& List = Renderer->GetScene().Renderables();
        if (Sel < 0 || Sel >= static_cast<int>(List.size())) return;

        const auto Want = _V ? Smile::EMobility::Dynamic : Smile::EMobility::Static;
        Smile::FRenderable& R = List[static_cast<size_t>(Sel)];
        if (R.Mobility == Want) return;
        R.Mobility = Want;
        // Nos DOIS sentidos: virar dinamico tira o objeto do mapa estatico, virar estatico o
        // coloca la. Qualquer um dos dois muda o conteudo cacheado.
        Renderer->GetScene().BumpStaticCastersVersion();
        MarkDirty();
        emit SelectionChanged();
    }

    // ---- Acoes ----
    void SceneOutlinerBridge::toggleExpand(int _Row) {
        if (_Row < 0 || _Row >= Rows.size()) return;
        if (!SearchText.isEmpty()) return; // busca forca tudo expandido
        const FRow& R = Rows[_Row];
        if (R.Kind == KGroup && R.SceneIdx >= 0 && R.SceneIdx < 3) {
            GroupExpanded[R.SceneIdx] = !GroupExpanded[R.SceneIdx];
        } else if (R.Kind == KAsset) {
            if (R.SceneIdx >= 0 && R.SceneIdx < AssetExpanded.size())
                AssetExpanded[R.SceneIdx] = !AssetExpanded[R.SceneIdx];
            else
                return; // pasta residual "Outros": sem estado persistente
        } else {
            return;
        }
        Rebuild();
    }

    void SceneOutlinerBridge::selectRow(int _Row) {
        if (!Renderer || _Row < 0 || _Row >= Rows.size()) return;
        const FRow& R = Rows[_Row];
        if (R.Kind == KLight) {
            if (Renderer->GetSelectedLight() == R.SceneIdx) return;
            Renderer->SetSelectedLight(R.SceneIdx);
            Renderer->ClearSelection(); // selecao de luz e de mesh sao exclusivas
        } else if (R.Kind == KMesh) {
            if (Renderer->GetSelectedObject() == R.SceneIdx) return;
            Renderer->SetSelectedObject(R.SceneIdx);
            Renderer->ClearLightSelection();
        } else {
            return;
        }
        CachedSelMesh  = Renderer->GetSelectedObject();
        CachedSelLight = Renderer->GetSelectedLight();
        if (!Rows.isEmpty())
            emit dataChanged(index(0), index((int)Rows.size() - 1), { RSelected });
        emit SelectionChanged();
    }

    void SceneOutlinerBridge::ShiftAssetRangesAfterRemoval(int _RemovedIndex) {
        for (FAssetRange& Range : Assets) {
            if (Range.Begin > _RemovedIndex) --Range.Begin;
            if (Range.End   > _RemovedIndex) --Range.End;
        }
    }

    bool SceneOutlinerBridge::deleteSelectedMesh() {
        if (!Renderer) return false;
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess) return false;

        const int              Index = RendererAccess->GetSelectedObject();
        const Smile::u64       Id    = RendererAccess->GetSelectedObjectId();
        if (Index < 0 || Id == 0) return false;

        const QString Name = QString::fromStdString(
            RendererAccess->GetScene().Renderables()[(size_t)Index].Name);
        if (!RendererAccess->RemoveRenderable(Id)) return false;
        RendererAccess->ClearSelection();

        ShiftAssetRangesAfterRemoval(Index);
        CachedSelMesh = -1;
        // A visibilidade persistida e por indice dentro do asset; com a lista deslocada o json
        // salvo antes da remocao ja nao descreve esta cena.
        MarkDirty();
        Rebuild();
        emit SelectionChanged();
        Smile::LogInfo("Objeto removido da cena: " + Name.toStdString());
        return true;
    }

    bool SceneOutlinerBridge::duplicateSelectedMesh() {
        if (!Renderer) return false;
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess) return false;

        const Smile::u64 Id = RendererAccess->GetSelectedObjectId();
        if (Id == 0) return false;

        const Smile::u64 NewId = RendererAccess->DuplicateRenderable(Id);
        if (NewId == 0) return false;

        // Seleciona a copia: e o que o usuario vai querer arrastar em seguida, e serve de
        // confirmacao visual de que ela existe (a copia nasce exatamente sobre o original).
        const int NewIndex = RendererAccess->GetScene().IndexOfRenderable(NewId);
        RendererAccess->SetSelectedObject(NewIndex);
        RendererAccess->ClearLightSelection();
        CachedSelMesh = NewIndex;

        MarkDirty();
        Rebuild();
        RevealSelection();
        emit SelectionChanged();
        return true;
    }

    void SceneOutlinerBridge::toggleEye(int _Row) {
        if (!Renderer || _Row < 0 || _Row >= Rows.size()) return;
        auto RendererAccess = Renderer.Lock();
        const FRow& Row = Rows[_Row];
        auto& Renderables = Renderer->GetScene().Renderables();

        // Uniao das caixas de mundo do que for realmente alternado. O objeto nao MOVE, entao
        // antes e depois sao a mesma caixa e basta uma; o que muda e ele entrar ou sair da TLAS
        // (RaytracingScene pula !Visible), ou seja, o GI daquela regiao passa a ver outra cena.
        Smile::Vec3 RegionMin{}, RegionMax{};
        bool        HasRegion = false;
        auto GrowRegion = [&](const Smile::FRenderable& _R) {
            if (!HasRegion) { RegionMin = _R.AABBMin; RegionMax = _R.AABBMax; HasRegion = true; return; }
            RegionMin = { std::min(RegionMin.X, _R.AABBMin.X), std::min(RegionMin.Y, _R.AABBMin.Y),
                          std::min(RegionMin.Z, _R.AABBMin.Z) };
            RegionMax = { std::max(RegionMax.X, _R.AABBMax.X), std::max(RegionMax.Y, _R.AABBMax.Y),
                          std::max(RegionMax.Z, _R.AABBMax.Z) };
        };

        if (Row.Kind == KMesh) {
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Renderables.size()) return;
            Renderables[(size_t)Row.SceneIdx].Visible =
                !Renderables[(size_t)Row.SceneIdx].Visible;
            GrowRegion(Renderables[(size_t)Row.SceneIdx]);
        } else if (Row.Kind == KAsset) {
            // Pasta: range do asset (ou o residual "Outros"). Se ha alguma visivel ->
            // esconde todas; senao mostra todas.
            const int Total = (int)Renderables.size();
            int Begin = 0, End = Total;
            if (Row.SceneIdx >= 0 && Row.SceneIdx < Assets.size()) {
                Begin = Assets[Row.SceneIdx].Begin;
                End   = std::min(Assets[Row.SceneIdx].End, Total);
            } else if (!Assets.isEmpty()) {
                Begin = Assets.last().End; // residual
            }
            bool AnyVisible = false;
            for (int I = Begin; I < End; ++I)
                if (!Renderables[(size_t)I].RaytracingOnly && Renderables[(size_t)I].Visible)
                    { AnyVisible = true; break; }
            for (int I = Begin; I < End; ++I)
                if (!Renderables[(size_t)I].RaytracingOnly) {
                    Renderables[(size_t)I].Visible = !AnyVisible;
                    GrowRegion(Renderables[(size_t)I]);
                }
        } else if (Row.Kind == KEnv && Row.SceneIdx == EnvTerreno) {
            SetTerrainVisible(!Renderer->Settings().GetUseTerrain());
            return;
        } else {
            return;
        }

        // TLAS re-coleta as instancias no rebuild leve do proximo frame (pula !Visible),
        // entao GI/reflexos/ReSTIR respeitam o olho, nao so o raster.
        Renderer->GetScene().BumpTransformsVersion();
        // Ocultar/mostrar muda o que o mapa estatico contem, tanto quanto criar ou remover.
        Renderer->GetScene().BumpStaticCastersVersion();
        // ...e muda o que os CACHES ja acumularam. O atlas do DDGI cai so na regiao afetada;
        // reservoirs, reflexoes, ReGIR e cache de radiancia nao tem granularidade espacial e
        // caem inteiros (dominio SceneContent). Diferente do arraste de gizmo, aqui e um clique
        // discreto: nao ha gesto continuo para o reset atrapalhar.
        // Geometria: a instancia sai da TLAS (RaytracingScene pula !Visible), entao as sondas
        // dali passam a enxergar coisa diferente — e a classificacao delas envelheceu junto.
        if (HasRegion)
            Renderer->NotifyGIRegionChanged(RegionMin, RegionMax, Smile::EGIRegionChange::Geometry);
        Renderer->Settings().MarkSceneContentDirty();
        MarkDirty();
        Rebuild();
    }

    void SceneOutlinerBridge::focusRow(int _Row) {
        if (!Renderer || _Row < 0 || _Row >= Rows.size()) return;
        auto RendererAccess = Renderer.Lock();
        const FRow& Row = Rows[_Row];

        Smile::Vec3 Center;
        float Radius = 1.0f;
        if (Row.Kind == KMesh) {
            const auto& Renderables = Renderer->GetScene().Renderables();
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Renderables.size()) return;
            const auto& R = Renderables[(size_t)Row.SceneIdx];
            // FRenderable::AABBMin/Max ja e a caixa de MUNDO (derivada da local pelo Transform
            // em RefreshWorldBounds), entao o centro sai direto dela. Somar Transform.Position
            // aqui contava a translacao DUAS vezes: era no-op enquanto o cooker bakeava a
            // transform no vertice e todo transform era identidade, e passou a jogar a camera
            // longe do objeto quando a v7 tirou o bake.
            Center = (R.AABBMin + R.AABBMax) * 0.5f;
            Radius = std::max(((R.AABBMax - R.AABBMin) * 0.5f).Length(), 0.25f);
        } else if (Row.Kind == KLight) {
            const auto& Lights = Renderer->GetScene().Lights();
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Lights.size()) return;
            Center = Lights[(size_t)Row.SceneIdx].Position;
            Radius = 1.5f;
        } else {
            return;
        }

        // Enquadra aproximando PELO LADO onde a camera ja esta (dolly no eixo alvo ->
        // camera) e aponta pro alvo — recuar pelo forward atual jogava a camera pra
        // TRAS do objeto quando ele estava fora do olhar (ex.: dentro do predio atras
        // da arvore). Mesmo comportamento da tecla F da UE.
        const float Dist = std::clamp(Radius * 2.2f, 1.5f, 220.0f);
        Smile::Vec3 Dir = (Renderer->GetCameraPos() - Center)
                              .NormalizedSafe(Smile::Vec3{ 0.0f, 0.35f, -1.0f });
        const Smile::Vec3 Pos = Center + Dir * Dist;
        const Smile::Vec3 Look = (Dir * -1.0f); // olhar pro centro
        const float PitchDeg = (float)(std::asin(std::clamp((double)Look.Y, -1.0, 1.0)) / kToRad);
        const float YawDeg   = (float)(std::atan2((double)Look.X, (double)Look.Z) / kToRad);
        Renderer->SetCameraPose(Pos, PitchDeg, YawDeg);
    }

    int SceneOutlinerBridge::selectedRowIndex() const {
        for (int I = 0; I < Rows.size(); ++I)
            if (RowSelected(Rows[I])) return I;
        return -1;
    }

    int SceneOutlinerBridge::RevealSelection() {
        int Row = selectedRowIndex();
        if (Row >= 0 || !Renderer) return Row;

        const int SelLight = Renderer->GetSelectedLight();
        const int SelMesh  = Renderer->GetSelectedObject();
        if (SelLight < 0 && SelMesh < 0) return -1;

        // A linha nao esta na lista flat: grupo/pasta colapsados ou chip de filtro
        // escondendo o tipo — abre o caminho ate ela.
        bool Changed = false;
        if (SelLight >= 0 && !GroupExpanded[GLuzes]) {
            GroupExpanded[GLuzes] = true;
            Changed = true;
        }
        if (SelMesh >= 0) {
            if (!GroupExpanded[GMeshes]) { GroupExpanded[GMeshes] = true; Changed = true; }
            for (int A = 0; A < Assets.size() && A < AssetExpanded.size(); ++A) {
                if (SelMesh >= Assets[A].Begin && SelMesh < Assets[A].End &&
                    !AssetExpanded[A]) {
                    AssetExpanded[A] = true;
                    Changed = true;
                }
            }
        }
        const int Need = SelLight >= 0 ? GLuzes : GMeshes;
        if (FilterGroup != 0 && FilterGroup != Need + 1) {
            FilterGroup = 0;
            Changed = true;
            emit FiltersChanged();
        }
        if (Changed) {
            Rebuild();
            Row = selectedRowIndex();
        }
        return Row; // -1 restante = busca ativa filtrando o nome da selecao
    }

    void SceneOutlinerBridge::selectStep(int _Delta) {
        if (Rows.isEmpty()) return;
        const int Dir = _Delta >= 0 ? 1 : -1;
        const int Cur = selectedRowIndex();
        int I = Cur < 0 ? (Dir > 0 ? 0 : (int)Rows.size() - 1) : Cur + Dir;
        for (; I >= 0 && I < Rows.size(); I += Dir) {
            if (Rows[I].Kind == KLight || Rows[I].Kind == KMesh) {
                selectRow(I);
                emit ScrollToRequested(I);
                return;
            }
        }
    }

    // ---- Persistencia (<cena>.visibility.json) ----
    void SceneOutlinerBridge::MarkDirty() {
        if (!VisDirty) { VisDirty = true; emit DirtyChanged(); }
    }

    bool SceneOutlinerBridge::LoadVisibility() {
        if (!Renderer || JsonPath.isEmpty()) return false;
        QFile File(JsonPath);
        if (!File.exists()) return false;
        if (!File.open(QIODevice::ReadOnly)) {
            Smile::LogWarning("Outliner: falha ao abrir " + JsonPath.toStdString());
            return false;
        }
        QJsonParseError Err{};
        const QJsonDocument Doc = QJsonDocument::fromJson(File.readAll(), &Err);
        if (Err.error != QJsonParseError::NoError || !Doc.isObject()) {
            Smile::LogWarning("Outliner: JSON invalido em " + JsonPath.toStdString());
            return false;
        }
        const QJsonObject Root = Doc.object();
        auto RendererAccess = Renderer.Lock();
        auto& Renderables = Renderer->GetScene().Renderables();
        const int Total = (int)Renderables.size();
        bool Applied = false;

        if (Root.contains(QStringLiteral("terrainVisible"))) {
            const bool V = Root.value(QStringLiteral("terrainVisible")).toBool(true);
            if (Renderer->Settings().GetUseTerrain() != V) {
                Renderer->Settings().SetUseTerrain(V);
                for (auto& R : Renderables)
                    if (R.RaytracingOnly) R.Visible = V;
                CachedTerrainOn = V;
                Applied = true;
            }
        }

        // Ocultas por asset: indices RELATIVOS ao Begin do range — estaveis enquanto os
        // mesmos cozidos carregarem na mesma ordem (a ordem do .sscene e deterministica).
        int HiddenApplied = 0;
        for (const QJsonValue& V : Root.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject O = V.toObject();
            const QString Name = O.value(QStringLiteral("name")).toString();
            for (const FAssetRange& A : Assets) {
                if (A.Name != Name) continue;
                const int End = std::min(A.End, Total);
                for (const QJsonValue& H : O.value(QStringLiteral("hidden")).toArray()) {
                    const int I = A.Begin + H.toInt(-1);
                    if (I >= A.Begin && I < End && Renderables[(size_t)I].Visible) {
                        Renderables[(size_t)I].Visible = false;
                        Applied = true;
                        ++HiddenApplied;
                    }
                }
                break;
            }
        }

        if (Applied) {
            Renderer->GetScene().BumpTransformsVersion(); // TLAS acompanha as ocultas
            Renderer->GetScene().BumpStaticCastersVersion();
            if (HiddenApplied > 0)
                Smile::LogInfo("Outliner: " + std::to_string(HiddenApplied) +
                               " mesh(es) ocultas de " +
                               QFileInfo(JsonPath).fileName().toStdString());
        }
        return Applied;
    }

    bool SceneOutlinerBridge::saveVisibility() {
        if (!Renderer || JsonPath.isEmpty()) return false;
        auto RendererAccess = Renderer.Lock();
        const auto& Renderables = Renderer->GetScene().Renderables();
        const int Total = (int)Renderables.size();

        QJsonArray AssetsArr;
        for (const FAssetRange& A : Assets) {
            QJsonArray Hidden;
            const int End = std::min(A.End, Total);
            for (int I = A.Begin; I < End; ++I) {
                const auto& R = Renderables[(size_t)I];
                if (!R.RaytracingOnly && !R.Visible) Hidden.append(I - A.Begin);
            }
            if (Hidden.isEmpty()) continue;
            QJsonObject O;
            O[QStringLiteral("name")]   = A.Name;
            O[QStringLiteral("hidden")] = Hidden;
            AssetsArr.append(O);
        }

        QJsonObject Root;
        Root[QStringLiteral("version")]        = 1;
        Root[QStringLiteral("terrainVisible")] = Renderer->Settings().GetUseTerrain();
        Root[QStringLiteral("assets")]         = AssetsArr;

        QFile File(JsonPath);
        if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Smile::LogError("Outliner: falha ao salvar " + JsonPath.toStdString());
            return false;
        }
        File.write(QJsonDocument(Root).toJson(QJsonDocument::Indented));
        Smile::LogInfo("Outliner: visibilidade salva em " +
                       QFileInfo(JsonPath).fileName().toStdString());
        if (VisDirty) { VisDirty = false; emit DirtyChanged(); }
        return true;
    }

    // ---- Sincronizacao por frame ----
    void SceneOutlinerBridge::Refresh() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        const auto& Scene = Renderer->GetScene();

        // Estrutura mudou por fora (load aditivo sem aviso, remocao de luz pelo painel).
        if ((int)Scene.Renderables().size() != CachedRenderableCount ||
            (int)Scene.Lights().size()      != CachedLightCount ||
            Renderer->GetTerrain().IsLoaded() != CachedTerrain) {
            Rebuild();
        }

        // Selecao mudou por fora (picking no viewport, acoes do lightsModel).
        const int SelMesh  = Renderer->GetSelectedObject();
        const int SelLight = Renderer->GetSelectedLight();
        if (SelMesh != CachedSelMesh || SelLight != CachedSelLight) {
            CachedSelMesh  = SelMesh;
            CachedSelLight = SelLight;
            if (!Rows.isEmpty())
                emit dataChanged(index(0), index((int)Rows.size() - 1), { RSelected });
            emit SelectionChanged();
            const int Row = RevealSelection(); // expande o caminho se estiver colapsado
            if (Row >= 0) emit ScrollToRequested(Row);
        }

        // Toggles de ambiente mudaram por fora (SettingsWindow etc.).
        const bool Clouds    = Renderer->Settings().GetUseClouds();
        const bool Ocean     = Renderer->Settings().GetUseWater();
        const bool TerrainOn = Renderer->Settings().GetUseTerrain();
        if (Clouds != CachedClouds || Ocean != CachedOcean || TerrainOn != CachedTerrainOn) {
            CachedClouds    = Clouds;
            CachedOcean     = Ocean;
            CachedTerrainOn = TerrainOn;
            Rebuild(); // REnabled das linhas de ambiente
            emit EnvChanged();
        }
    }
}
