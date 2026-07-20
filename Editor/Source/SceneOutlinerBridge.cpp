#include "SmileEditor/SceneOutlinerBridge.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Scene/Scene.h"

#include <QFileInfo>

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

    void SceneOutlinerBridge::SetRenderer(Smile::Renderer* _Renderer) {
        Renderer = _Renderer;
        if (Renderer) {
            CachedSelMesh  = Renderer->GetSelectedObject();
            CachedSelLight = Renderer->GetSelectedLight();
            CachedClouds    = Renderer->GetUseClouds();
            CachedOcean     = Renderer->GetUseWater();
            CachedTerrain   = Renderer->GetTerrain().IsLoaded();
            CachedTerrainOn = Renderer->GetUseTerrain();
        }
        Rebuild();
        emit AvailableChanged();
        emit EnvChanged();
        emit SelectionChanged();
    }

    void SceneOutlinerBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (!Renderer) return;
        const int Count = (int)Renderer->GetScene().Renderables().size();
        const QString Base = QFileInfo(_ScenePath).completeBaseName();
        if (!_Additive) {
            Assets.clear();
            AssetExpanded.clear();
            Assets.push_back({ Base, 0, Count });
            AssetExpanded.push_back(true);
        } else {
            const int Begin = Assets.isEmpty() ? 0 : Assets.last().End;
            Assets.push_back({ Base, Begin, Count });
            AssetExpanded.push_back(true);
        }
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
        const auto& Scene = const_cast<Smile::Renderer*>(Renderer)->GetScene();
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
                R.Enabled  = (E.Id == EnvNuvens)  ? Renderer->GetUseClouds()
                           : (E.Id == EnvOceano)  ? Renderer->GetUseWater()
                           : (E.Id == EnvTerreno) ? Renderer->GetUseTerrain() : true;
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
        const auto& Scene = const_cast<Smile::Renderer*>(Renderer)->GetScene();
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
        const auto& Scene = const_cast<Smile::Renderer*>(Renderer)->GetScene();
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
        return Renderer ? Renderer->GetUseWater() : false;
    }

    void SceneOutlinerBridge::SetOceanVisible(bool _V) {
        if (!Renderer || Renderer->GetUseWater() == _V) return;
        Renderer->SetUseWater(_V);
        CachedOcean = _V;
        Rebuild(); // REnabled da linha do oceano
        emit EnvChanged();
    }

    bool SceneOutlinerBridge::TerrainVisible() const {
        return Renderer ? Renderer->GetUseTerrain() : true;
    }

    void SceneOutlinerBridge::SetTerrainVisible(bool _V) {
        if (!Renderer || Renderer->GetUseTerrain() == _V) return;
        Renderer->SetUseTerrain(_V); // gates de prepass/G-buffer/CSM do FTerrain
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
        Rebuild();
        emit EnvChanged();
    }

    bool SceneOutlinerBridge::MeshSelected() const {
        return Renderer && Renderer->GetSelectedObject() >= 0;
    }

    QString SceneOutlinerBridge::MeshName() const {
        if (!Renderer) return {};
        const auto& Renderables = Renderer->GetScene().Renderables();
        const int I = Renderer->GetSelectedObject();
        if (I < 0 || I >= (int)Renderables.size()) return {};
        return QString::fromStdString(Renderables[(size_t)I].Name);
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

    void SceneOutlinerBridge::toggleEye(int _Row) {
        if (!Renderer || _Row < 0 || _Row >= Rows.size()) return;
        const FRow& Row = Rows[_Row];
        auto& Renderables = Renderer->GetScene().Renderables();

        if (Row.Kind == KMesh) {
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Renderables.size()) return;
            Renderables[(size_t)Row.SceneIdx].Visible =
                !Renderables[(size_t)Row.SceneIdx].Visible;
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
                if (!Renderables[(size_t)I].RaytracingOnly)
                    Renderables[(size_t)I].Visible = !AnyVisible;
        } else if (Row.Kind == KEnv && Row.SceneIdx == EnvTerreno) {
            SetTerrainVisible(!Renderer->GetUseTerrain());
            return;
        } else {
            return;
        }

        // TLAS re-coleta as instancias no rebuild leve do proximo frame (pula !Visible),
        // entao GI/reflexos/ReSTIR respeitam o olho, nao so o raster.
        Renderer->GetScene().BumpTransformsVersion();
        Rebuild();
    }

    void SceneOutlinerBridge::focusRow(int _Row) {
        if (!Renderer || _Row < 0 || _Row >= Rows.size()) return;
        const FRow& Row = Rows[_Row];

        Smile::Vec3 Center;
        float Radius = 1.0f;
        if (Row.Kind == KMesh) {
            const auto& Renderables = Renderer->GetScene().Renderables();
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Renderables.size()) return;
            const auto& R = Renderables[(size_t)Row.SceneIdx];
            // AABB ja e world-space nas cenas cozidas (o cooker baka a transform no
            // vertice); pra renderables com transform propria, soma a posicao.
            const Smile::Vec3 Local = (R.AABBMin + R.AABBMax) * 0.5f;
            Center = Local + R.Transform.Position;
            Radius = std::max(((R.AABBMax - R.AABBMin) * 0.5f).Length(), 0.25f);
        } else if (Row.Kind == KLight) {
            const auto& Lights = Renderer->GetScene().Lights();
            if (Row.SceneIdx < 0 || Row.SceneIdx >= (int)Lights.size()) return;
            Center = Lights[(size_t)Row.SceneIdx].Position;
            Radius = 1.5f;
        } else {
            return;
        }

        // Enquadra mantendo a orientacao atual: recua do alvo ao longo do forward.
        const double Pitch = Renderer->GetPitch() * kToRad;
        const double Yaw   = Renderer->GetYaw()   * kToRad;
        const Smile::Vec3 Fwd = { (float)(std::cos(Pitch) * std::sin(Yaw)),
                                  (float)std::sin(Pitch),
                                  (float)(std::cos(Pitch) * std::cos(Yaw)) };
        const float Dist = std::clamp(Radius * 2.2f, 1.5f, 220.0f);
        Renderer->SetCameraPose(Center - Fwd * Dist,
                                Renderer->GetPitch(), Renderer->GetYaw());
    }

    int SceneOutlinerBridge::selectedRowIndex() const {
        for (int I = 0; I < Rows.size(); ++I)
            if (RowSelected(Rows[I])) return I;
        return -1;
    }

    // ---- Sincronizacao por frame ----
    void SceneOutlinerBridge::Refresh() {
        if (!Renderer) return;
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
            const int Row = selectedRowIndex();
            if (Row >= 0) emit ScrollToRequested(Row);
        }

        // Toggles de ambiente mudaram por fora (SettingsWindow etc.).
        const bool Clouds    = Renderer->GetUseClouds();
        const bool Ocean     = Renderer->GetUseWater();
        const bool TerrainOn = Renderer->GetUseTerrain();
        if (Clouds != CachedClouds || Ocean != CachedOcean || TerrainOn != CachedTerrainOn) {
            CachedClouds    = Clouds;
            CachedOcean     = Ocean;
            CachedTerrainOn = TerrainOn;
            Rebuild(); // REnabled das linhas de ambiente
            emit EnvChanged();
        }
    }
}
