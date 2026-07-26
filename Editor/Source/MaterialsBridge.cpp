#include "SmileEditor/MaterialsBridge.h"

#include "Smile/Graphics/Renderer.h"
#include "Smile/Core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace SmileEditor {
    namespace {
        // Estimativa de VRAM de uma textura (mip chain completa). Bytes por bloco 4x4 nos
        // BC*, bytes por pixel no resto — bate com o VramTracker o suficiente pro browser.
        quint64 TextureBytes(const Smile::FTexture& T) {
            if (!T.IsValid()) return 0;
            const DXGI_FORMAT F = T.Format();
            const bool Block8 = (F >= DXGI_FORMAT_BC1_TYPELESS && F <= DXGI_FORMAT_BC1_UNORM_SRGB) ||
                                (F >= DXGI_FORMAT_BC4_TYPELESS && F <= DXGI_FORMAT_BC4_SNORM);
            const bool Block16 = (F >= DXGI_FORMAT_BC2_TYPELESS && F <= DXGI_FORMAT_BC3_UNORM_SRGB) ||
                                 (F >= DXGI_FORMAT_BC5_TYPELESS && F <= DXGI_FORMAT_BC5_SNORM) ||
                                 (F >= DXGI_FORMAT_BC6H_TYPELESS && F <= DXGI_FORMAT_BC7_UNORM_SRGB);
            quint64 Total = 0;
            Smile::u32 W = T.Width(), H = T.Height();
            for (Smile::u32 Mip = 0; Mip < T.MipCount(); ++Mip) {
                if (Block8 || Block16) {
                    const quint64 Blocks = quint64((W + 3) / 4) * quint64((H + 3) / 4);
                    Total += Blocks * (Block8 ? 8 : 16);
                } else {
                    Total += quint64(W) * quint64(H) * 4; // RGBA8 e afins
                }
                W = W > 1 ? W / 2 : 1;
                H = H > 1 ? H / 2 : 1;
            }
            return Total;
        }

        QString FormatName(DXGI_FORMAT F) {
            if (F >= DXGI_FORMAT_BC1_TYPELESS && F <= DXGI_FORMAT_BC1_UNORM_SRGB) return QStringLiteral("BC1");
            if (F >= DXGI_FORMAT_BC2_TYPELESS && F <= DXGI_FORMAT_BC2_UNORM_SRGB) return QStringLiteral("BC2");
            if (F >= DXGI_FORMAT_BC3_TYPELESS && F <= DXGI_FORMAT_BC3_UNORM_SRGB) return QStringLiteral("BC3");
            if (F >= DXGI_FORMAT_BC4_TYPELESS && F <= DXGI_FORMAT_BC4_SNORM)      return QStringLiteral("BC4");
            if (F >= DXGI_FORMAT_BC5_TYPELESS && F <= DXGI_FORMAT_BC5_SNORM)      return QStringLiteral("BC5");
            if (F >= DXGI_FORMAT_BC6H_TYPELESS && F <= DXGI_FORMAT_BC6H_SF16)     return QStringLiteral("BC6H");
            if (F >= DXGI_FORMAT_BC7_TYPELESS && F <= DXGI_FORMAT_BC7_UNORM_SRGB) return QStringLiteral("BC7");
            if (F == DXGI_FORMAT_R8G8B8A8_UNORM || F == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                return QStringLiteral("RGBA8");
            return QStringLiteral("?");
        }

        QString BytesText(quint64 Bytes) {
            const double KB = double(Bytes) / 1024.0;
            if (KB < 1024.0)
                return QString::number(KB, 'f', 1).replace('.', ',') + QStringLiteral(" KB");
            return QString::number(KB / 1024.0, 'f', 1).replace('.', ',') + QStringLiteral(" MB");
        }

        // Badges do browser, na ordem de "peso" visual.
        QStringList BadgesOf(const Smile::FMaterial& M) {
            QStringList B;
            const auto& C = M.Constants;
            if (C.ShadingModel == 1)      B << QStringLiteral("FOL");
            else if (C.ShadingModel == 2) B << QStringLiteral("SSS");
            if (C.HasHeightMap)      B << QStringLiteral("POM");
            if (M.Blend)             B << QStringLiteral("BLEND");
            else if (C.AlphaTest)    B << QStringLiteral("MASK");
            if (M.TwoSided)          B << QStringLiteral("2S");
            const auto& E = C.EmissiveFactor;
            if (C.HasEmissiveMap || (E.X + E.Y + E.Z) * C.EmissiveStrength > 0.001f)
                B << QStringLiteral("EMIS");
            return B;
        }

        QJsonArray ColorToJson(float R, float G, float B) {
            return QJsonArray{ double(R), double(G), double(B) };
        }
    }

    MaterialsBridge::MaterialsBridge(QObject* _Parent) : QAbstractListModel(_Parent) {}

    void MaterialsBridge::SetRenderer(Smile::Renderer* _R) {
        Renderer = _R;
        emit AvailableChanged();
        Rebuild();
    }

    void MaterialsBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (!_Additive) {
            const QFileInfo Info(_ScenePath);
            // Persistencia acompanha a cena principal (mesma convencao do .visibility.json).
            JsonPath = Info.path() + "/" + Info.completeBaseName() + ".materials.json";
            OverrideCache.clear();
            LegacyOverrideCache.clear();
            // O load nao-aditivo LIBEROU os FMaterial/FTexture antigos: todo cache por
            // ponteiro esta stale (um material novo no mesmo endereco pegaria snapshot
            // errado). Zera tudo antes do Rebuild re-snapshotar.
            Defaults.clear();
            TexOverrides.clear();
            PathTexCache.clear();
            SelectedMat = -1;
            IsolatingOn = false;
            SavedVisibility.clear();
            emit IsolatingChanged();
            {
                QMutexLocker Lock(&ThumbMutex);
                ThumbByIdx.clear();
                ThumbVersion.clear();
                ThumbPending.clear();
            }
            DefaultEnvTried = false; // cena nova pode ter vindo de outro diretorio

            QFile File(JsonPath);
            if (File.exists() && File.open(QIODevice::ReadOnly)) {
                QJsonParseError Err{};
                const QJsonDocument Doc = QJsonDocument::fromJson(File.readAll(), &Err);
                if (Err.error == QJsonParseError::NoError && Doc.isObject()) {
                    const QJsonObject Root = Doc.object();
                    // v2 chaveia por id (hex do FMaterial::Id); v1 chaveava por nome. Sidecar
                    // antigo continua sendo lido — aplicado por nome uma vez e migrado no save.
                    const int SidecarVer = Root.value(QStringLiteral("version")).toInt(1);
                    for (const QJsonValue& V : Root.value(QStringLiteral("materials")).toArray()) {
                        const QJsonObject O  = V.toObject();
                        const QString Name   = O.value(QStringLiteral("name")).toString();
                        const QString IdText = O.value(QStringLiteral("id")).toString();
                        bool IdOk = false;
                        const quint64 Id = IdText.toULongLong(&IdOk, 16);
                        if (SidecarVer >= 2 && IdOk && Id != 0) OverrideCache.insert(Id, O);
                        else if (!Name.isEmpty())               LegacyOverrideCache.insert(Name, O);
                    }
                    const size_t Total = OverrideCache.size() + LegacyOverrideCache.size();
                    Smile::LogInfo("Materiais: " + std::to_string(Total) + " override(s) de " +
                                   JsonPath.toStdString() +
                                   (LegacyOverrideCache.isEmpty()
                                        ? ""
                                        : " (formato antigo por nome; sera migrado no proximo save)"));
                } else {
                    Smile::LogWarning("Materiais: sidecar invalido, ignorando: " +
                                      JsonPath.toStdString());
                }
            }
        }
        Rebuild(); // snapshot dos defaults ANTES de aplicar overrides (Rebuild cuida)
        if (JsonDirty) { JsonDirty = false; emit DirtyChanged(); }
    }

    // ---- Modelo ----
    int MaterialsBridge::rowCount(const QModelIndex& _Parent) const {
        return _Parent.isValid() ? 0 : Rows.size();
    }

    QVariant MaterialsBridge::data(const QModelIndex& _Index, int _Role) const {
        if (!_Index.isValid() || _Index.row() < 0 || _Index.row() >= Rows.size()) return {};
        const FRow& R = Rows[_Index.row()];
        switch (_Role) {
        case RName:      return R.Name;
        case RMeshCount: return R.MeshCount;
        case RVramText:  return BytesText(R.VramBytes);
        case RBadges:    return R.Badges;
        case RSwatch:    return R.Swatch;
        case RSelected:  return R.MatIdx == SelectedMat;
        case RModified: {
            if (!Renderer) return false;
            const auto& Mats = Renderer->GetMaterials();
            return R.MatIdx >= 0 && R.MatIdx < (int)Mats.size() && IsModified(Mats[R.MatIdx].get());
        }
        case RThumb: {
            QMutexLocker Lock(&ThumbMutex);
            if (ThumbByIdx.contains(R.MatIdx))
                return QStringLiteral("image://materialthumb/%1_v%2")
                    .arg(R.MatIdx).arg(ThumbVersion.value(R.MatIdx, 0));
            if (!ThumbPending.contains(R.MatIdx)) ThumbPending.push_back(R.MatIdx);
            return QString();
        }
        }
        return {};
    }

    QHash<int, QByteArray> MaterialsBridge::roleNames() const {
        return {
            { RName,      "name" },
            { RMeshCount, "meshCount" },
            { RVramText,  "vramText" },
            { RBadges,    "badges" },
            { RSwatch,    "swatch" },
            { RSelected,  "selected" },
            { RModified,  "modified" },
            { RThumb,     "thumbUrl" },
        };
    }

    int MaterialsBridge::TotalCount() const {
        if (!Renderer) return 0;
        int N = 0;
        for (const auto& M : Renderer->GetMaterials())
            if (!M->Name.empty()) ++N; // proxies internos (terreno) ficam fora
        return N;
    }

    void MaterialsBridge::SetSearch(const QString& _V) {
        if (SearchText == _V) return;
        SearchText = _V;
        emit FiltersChanged();
        RebuildRows();
    }

    void MaterialsBridge::SetFilter(int _V) {
        if (FilterKind == _V) return;
        FilterKind = _V;
        emit FiltersChanged();
        RebuildRows();
    }

    bool MaterialsBridge::MatchesFilter(const Smile::FMaterial& _M) const {
        if (!SearchText.isEmpty() &&
            !QString::fromStdString(_M.Name).contains(SearchText, Qt::CaseInsensitive))
            return false;
        const auto& C = _M.Constants;
        switch (FilterKind) {
        case FOpacos:  return !_M.Blend && !C.AlphaTest && C.ShadingModel == 0;
        case FBlend:   return _M.Blend;
        case FFoliage: return C.ShadingModel == 1;
        case FPom:     return C.HasHeightMap != 0;
        default:       return true;
        }
    }

    void MaterialsBridge::RebuildRows() {
        beginResetModel();
        Rows.clear();
        if (Renderer) {
            const auto& Mats = Renderer->GetMaterials();
            const auto& Rnds = Renderer->GetScene().Renderables();

            // Contagem de meshes por material numa passada so (2k+ renderables).
            QHash<const Smile::FMaterial*, int> Counts;
            Counts.reserve(int(Mats.size()));
            for (const auto& R : Rnds)
                if (R.Material && !R.RaytracingOnly) ++Counts[R.Material];

            Rows.reserve(int(Mats.size()));
            for (int i = 0; i < (int)Mats.size(); ++i) {
                const Smile::FMaterial& M = *Mats[i];
                if (M.Name.empty()) continue;          // material interno sem nome
                if (!MatchesFilter(M)) continue;

                FRow Row;
                Row.MatIdx    = i;
                Row.Name      = QString::fromStdString(M.Name);
                Row.MeshCount = Counts.value(&M, 0);
                const Smile::FTexture* Slots[Smile::kMaterialTextureSlots] = {
                    M.Albedo, M.Normal, M.MetallicRoughness, M.AO,
                    M.Emissive, M.Height, M.Metalness, M.Roughness };
                for (const auto* T : Slots)
                    if (T) Row.VramBytes += TextureBytes(*T);
                Row.Badges = BadgesOf(M);
                const auto& B = M.Constants.BaseColorFactor;
                Row.Swatch = QColor::fromRgbF(std::clamp(B.X, 0.0f, 1.0f),
                                              std::clamp(B.Y, 0.0f, 1.0f),
                                              std::clamp(B.Z, 0.0f, 1.0f));
                Rows.push_back(std::move(Row));
            }
        }
        endResetModel();
        emit StructureChanged();
        emit SelectionChanged();
    }

    void MaterialsBridge::Rebuild() {
        if (Renderer) {
            // Snapshot do default cozido de quem ainda nao tem (cargas novas), depois
            // overrides do sidecar por nome — a ordem garante que "Reverter" volta ao cooker.
            // "Fresh" = material que ainda nao tinha snapshot, ou seja, entrou nesta carga. Numa
            // carga nao-aditiva o OnSceneLoaded limpou Defaults, entao sao todos; numa aditiva,
            // so os novos — e os antigos preservam o que o usuario editou e ainda nao salvou.
            QSet<const Smile::FMaterial*> Fresh;
            for (const auto& M : Renderer->GetMaterials()) {
                if (Defaults.contains(M.get())) continue;
                FSnapshot S{ M->Constants, M->TwoSided, M->Blend, {} };
                S.Slots[0] = M->Albedo;   S.Slots[1] = M->Normal;
                S.Slots[2] = M->MetallicRoughness; S.Slots[3] = M->AO;
                S.Slots[4] = M->Emissive; S.Slots[5] = M->Height;
                S.Slots[6] = M->Metalness; S.Slots[7] = M->Roughness;
                Defaults.insert(M.get(), S);
                Fresh.insert(M.get());
            }
            ApplyOverrides(Fresh);
            CachedMatCount = int(Renderer->GetMaterials().size());
            if (SelectedMat >= CachedMatCount) SelectedMat = -1;
            MarkPreviewDirty();
            // Estrutura da cena mudou (carga aditiva): o isolar nao consegue mais mapear indice
            // -> renderable com seguranca, entao desliga. RESTAURANDO o Visible do prefixo comum:
            // limpar SavedVisibility direto deixava toda mesh de outro material invisivel p/
            // sempre, e o chip da UI ja voltava p/ "off" — sem caminho de volta pela interface.
            if (IsolatingOn &&
                SavedVisibility.size() != (qsizetype)Renderer->GetScene().Renderables().size()) {
                StopIsolation();
                emit IsolatingChanged();
            }
        } else {
            Defaults.clear();
            SelectedMat = -1;
            CachedMatCount = -1;
            IsolatingOn = false;
            SavedVisibility.clear();
        }
        RebuildRows();
    }

    void MaterialsBridge::ApplyOverrides(const QSet<const Smile::FMaterial*>& _Fresh) {
        if (!Renderer || (OverrideCache.isEmpty() && LegacyOverrideCache.isEmpty())) return;
        for (const auto& M : Renderer->GetMaterials()) {
            if (!_Fresh.contains(M.get())) continue; // ja carregado: nao pisa em edicao pendente
            // Id primeiro; nome so como ponte p/ sidecar v1 ainda nao migrado.
            const QJsonObject* Ovr = nullptr;
            if (const auto It = OverrideCache.constFind(M->Id); It != OverrideCache.constEnd()) {
                Ovr = &It.value();
            } else if (const auto L =
                           LegacyOverrideCache.constFind(QString::fromStdString(M->Name));
                       L != LegacyOverrideCache.constEnd()) {
                Ovr = &L.value();
            }
            if (!Ovr) continue;
            JsonToMaterial(*Ovr, *M);
            M->UpdateConstants();
            // O sidecar reescreve cor base, emissivo, rough/metal, cutoff e shading model — tudo
            // que o InstanceGeo copia. Sem isto a persistencia so valia p/ o raster: a cena reabria
            // com o material editado na tela e o GI iluminando pelos valores cozidos.
            if (Renderer) Renderer->MarkMaterialRTStateDirty();

            // Texturas trocadas: recarrega do path e reaplica no slot (falha -> slot segue
            // no mapa cozido, so loga). Cargas aditivas pulam quem ja tem override aplicado.
            const QJsonObject Tex = Ovr->value(QStringLiteral("textures")).toObject();
            if (Tex.isEmpty()) continue;
            for (auto T = Tex.constBegin(); T != Tex.constEnd(); ++T) {
                bool Ok = false;
                const int Slot = T.key().toInt(&Ok);
                if (!Ok || Slot < 0 || Slot >= int(Smile::kMaterialTextureSlots)) continue;
                if (TexOverrides.value(M.get()).contains(Slot)) continue; // ja aplicado
                const QString Path = T.value().toString();
                if (Path.isEmpty()) {
                    ApplySlotTexture(*M, Slot, nullptr);
                    TexOverrides[M.get()][Slot] = QString();
                    continue;
                }
                Smile::FTexture* Loaded = LoadTextureCached(Path, Slot);
                if (!Loaded) {
                    Smile::LogWarning("Materiais: textura do sidecar sumiu, mantendo a cozida: "
                                      + Path.toStdString());
                    continue;
                }
                ApplySlotTexture(*M, Slot, Loaded);
                TexOverrides[M.get()][Slot] = Path;
            }
        }
    }

    void MaterialsBridge::Refresh() {
        if (!Renderer) return;

        const int MatCount = int(Renderer->GetMaterials().size());
        if (MatCount != CachedMatCount) {
            Rebuild();
            return;
        }

        // Picking do viewport: clicar numa mesh seleciona o material dela no browser
        // (o "Pegar da cena" da Cry, de graca — o picking GPU ja existe).
        const int SelMesh = Renderer->GetSelectedObject();
        if (SelMesh != CachedSelMesh) {
            CachedSelMesh = SelMesh;
            // "Mesh da cena" segue a selecao mesmo quando o material e o mesmo.
            if (PreviewParams.Primitive == Smile::FMaterialPreview::PrimSceneMesh)
                MarkPreviewDirty();
            const auto& Rnds = Renderer->GetScene().Renderables();
            if (SelMesh >= 0 && SelMesh < (int)Rnds.size() && Rnds[SelMesh].Material) {
                const auto& Mats = Renderer->GetMaterials();
                for (int i = 0; i < (int)Mats.size(); ++i) {
                    if (Mats[i].get() != Rnds[SelMesh].Material) continue;
                    if (SelectedMat != i) {
                        SelectedMat = i;
                        MarkPreviewDirty();
                        emit SelectionChanged();
                        if (!Rows.isEmpty())
                            emit dataChanged(index(0), index(Rows.size() - 1), { RSelected });
                        const int Row = selectedRowIndex();
                        if (Row >= 0) emit ScrollToRequested(Row);
                        if (IsolatingOn) ApplyIsolation();
                    }
                    break;
                }
            }
        }

        // Preview: renderiza no maximo uma vez por frame do viewport, e so quando algo
        // mudou (parametro/selecao/orbita) com a janela aberta. Thumbnails drenam na fila.
        RenderPreviewIfNeeded();
        ProcessThumbQueue();
    }

    // ---- Selecao ----
    Smile::FMaterial* MaterialsBridge::SelMat() const {
        if (!Renderer || SelectedMat < 0) return nullptr;
        auto& Mats = Renderer->GetMaterials();
        return SelectedMat < (int)Mats.size() ? Mats[SelectedMat].get() : nullptr;
    }

    void MaterialsBridge::selectRow(int _Row) {
        if (_Row < 0 || _Row >= Rows.size()) return;
        if (SelectedMat == Rows[_Row].MatIdx) return;
        SelectedMat = Rows[_Row].MatIdx;
        MarkPreviewDirty();
        emit SelectionChanged();
        emit dataChanged(index(0), index(Rows.size() - 1), { RSelected });
        if (IsolatingOn) ApplyIsolation(); // isolar segue o material selecionado
    }

    int MaterialsBridge::selectedRowIndex() const {
        for (int i = 0; i < Rows.size(); ++i)
            if (Rows[i].MatIdx == SelectedMat) return i;
        return -1;
    }

    bool MaterialsBridge::HasSelection() const { return SelMat() != nullptr; }

    QString MaterialsBridge::Name() const {
        const auto* M = SelMat();
        return M ? QString::fromStdString(M->Name) : QString();
    }

    int MaterialsBridge::MeshCount() const {
        const auto* M = SelMat();
        if (!M || !Renderer) return 0;
        int N = 0;
        for (const auto& R : Renderer->GetScene().Renderables())
            if (R.Material == M && !R.RaytracingOnly) ++N;
        return N;
    }

    QString MaterialsBridge::VramText() const {
        const int Row = selectedRowIndex();
        if (Row < 0) {
            // Selecao filtrada pra fora da lista: recalcula direto.
            const auto* M = SelMat();
            if (!M) return {};
            quint64 Bytes = 0;
            const Smile::FTexture* Slots[Smile::kMaterialTextureSlots] = {
                M->Albedo, M->Normal, M->MetallicRoughness, M->AO,
                M->Emissive, M->Height, M->Metalness, M->Roughness };
            for (const auto* T : Slots)
                if (T) Bytes += TextureBytes(*T);
            return BytesText(Bytes);
        }
        return BytesText(Rows[Row].VramBytes);
    }

    // ---- Getters/Setters do selecionado (edicao ao vivo) ----
    // Todos os setters seguem o mesmo esquema: muta Constants (ou flag), UpdateConstants()
    // (memcpy no CBV upload mapeado) e TouchSelected() — o proximo frame ja desenha o novo.

    void MaterialsBridge::TouchSelected() {
        MarkDirty();
        MarkPreviewDirty();
        InvalidateThumb(SelectedMat); // thumbnail do browser acompanha a edicao
        emit SelectionChanged();
        const int Row = selectedRowIndex();
        if (Row >= 0) {
            // Badges/swatch/VRAM/dot podem mudar com a edicao.
            auto& R = Rows[Row];
            if (const auto* M = SelMat()) {
                R.Badges = BadgesOf(*M);
                const auto& B = M->Constants.BaseColorFactor;
                R.Swatch = QColor::fromRgbF(std::clamp(B.X, 0.0f, 1.0f),
                                            std::clamp(B.Y, 0.0f, 1.0f),
                                            std::clamp(B.Z, 0.0f, 1.0f));
                R.VramBytes = 0;
                const Smile::FTexture* Slots[Smile::kMaterialTextureSlots] = {
                    M->Albedo, M->Normal, M->MetallicRoughness, M->AO,
                    M->Emissive, M->Height, M->Metalness, M->Roughness };
                for (const auto* T : Slots)
                    if (T) R.VramBytes += TextureBytes(*T);
            }
            emit dataChanged(index(Row), index(Row),
                             { RBadges, RSwatch, RVramText, RModified });
        }
    }

    void MaterialsBridge::TouchSelectedRT() {
        TouchSelected();
        // Coalescido: o Renderer aplica uma vez no proximo frame. Chamar o Notify direto aqui
        // custaria um Flush da fila + reset de todos os historicos POR TICK de slider.
        if (Renderer) Renderer->MarkMaterialRTStateDirty();
    }

    void MaterialsBridge::MarkDirty() {
        if (!JsonDirty) { JsonDirty = true; emit DirtyChanged(); }
    }

    QColor MaterialsBridge::BaseColor() const {
        const auto* M = SelMat();
        if (!M) return QColor(255, 255, 255);
        const auto& B = M->Constants.BaseColorFactor;
        return QColor::fromRgbF(std::clamp(B.X, 0.0f, 1.0f), std::clamp(B.Y, 0.0f, 1.0f),
                                std::clamp(B.Z, 0.0f, 1.0f));
    }

    void MaterialsBridge::SetBaseColor(const QColor& _V) {
        auto* M = SelMat(); if (!M) return;
        auto& B = M->Constants.BaseColorFactor;
        B.X = float(_V.redF()); B.Y = float(_V.greenF()); B.Z = float(_V.blueF());
        M->UpdateConstants();
        TouchSelectedRT(); // InstanceGeo.BaseColor = albedo do GI/reflexo
    }

    qreal MaterialsBridge::Metallic() const {
        const auto* M = SelMat();
        return M ? M->Constants.MetallicFactor : 0.0;
    }

    void MaterialsBridge::SetMetallic(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.MetallicFactor = float(_V);
        M->UpdateConstants();
        TouchSelectedRT(); // InstanceGeo.EmissiveFactor.w
    }

    qreal MaterialsBridge::Roughness() const {
        const auto* M = SelMat();
        return M ? M->Constants.RoughnessFactor : 0.5;
    }

    void MaterialsBridge::SetRoughness(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.RoughnessFactor = float(_V);
        M->UpdateConstants();
        TouchSelectedRT(); // InstanceGeo.RoughnessFactor
    }

    qreal MaterialsBridge::AOStrength() const {
        const auto* M = SelMat();
        return M ? M->Constants.AOStrength : 1.0;
    }

    void MaterialsBridge::SetAOStrength(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.AOStrength = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    QColor MaterialsBridge::EmissiveColor() const {
        const auto* M = SelMat();
        if (!M) return QColor(0, 0, 0);
        const auto& E = M->Constants.EmissiveFactor;
        return QColor::fromRgbF(std::clamp(E.X, 0.0f, 1.0f), std::clamp(E.Y, 0.0f, 1.0f),
                                std::clamp(E.Z, 0.0f, 1.0f));
    }

    void MaterialsBridge::SetEmissiveColor(const QColor& _V) {
        auto* M = SelMat(); if (!M) return;
        auto& E = M->Constants.EmissiveFactor;
        E.X = float(_V.redF()); E.Y = float(_V.greenF()); E.Z = float(_V.blueF());
        M->UpdateConstants();
        TouchSelectedRT(); // InstanceGeo.EmissiveFactor.rgb — o emissivo ILUMINA o GI
    }

    qreal MaterialsBridge::EmissiveStrength() const {
        const auto* M = SelMat();
        return M ? M->Constants.EmissiveStrength : 1.0;
    }

    void MaterialsBridge::SetEmissiveStrength(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.EmissiveStrength = float(_V);
        M->UpdateConstants();
        TouchSelectedRT(); // multiplica o EmissiveFactor no InstanceGeo
    }

    qreal MaterialsBridge::NormalStrength() const {
        const auto* M = SelMat();
        return M ? M->Constants.NormalStrength : 1.0;
    }

    void MaterialsBridge::SetNormalStrength(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.NormalStrength = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::NormalFlipY() const {
        const auto* M = SelMat();
        return M && M->Constants.NormalFlipY != 0;
    }

    void MaterialsBridge::SetNormalFlipY(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.NormalFlipY = _V ? 1 : 0;
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::NormalReconstructZ() const {
        const auto* M = SelMat();
        return M && M->Constants.NormalReconstructZ != 0;
    }

    void MaterialsBridge::SetNormalReconstructZ(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.NormalReconstructZ = _V ? 1 : 0;
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::HasNormalMap() const {
        const auto* M = SelMat();
        return M && M->Constants.HasNormalMap != 0;
    }

    bool MaterialsBridge::HasHeightMap() const {
        const auto* M = SelMat();
        return M && M->Constants.HasHeightMap != 0;
    }

    qreal MaterialsBridge::HeightScale() const {
        const auto* M = SelMat();
        return M ? M->Constants.HeightScale : 0.05;
    }

    void MaterialsBridge::SetHeightScale(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.HeightScale = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    int MaterialsBridge::ParallaxMinSteps() const {
        const auto* M = SelMat();
        return M ? int(M->Constants.ParallaxMinSteps) : 8;
    }

    void MaterialsBridge::SetParallaxMinSteps(int _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.ParallaxMinSteps = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    int MaterialsBridge::ParallaxMaxSteps() const {
        const auto* M = SelMat();
        return M ? int(M->Constants.ParallaxMaxSteps) : 32;
    }

    void MaterialsBridge::SetParallaxMaxSteps(int _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.ParallaxMaxSteps = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::ParallaxSelfShadow() const {
        const auto* M = SelMat();
        return M && M->Constants.ParallaxSelfShadow != 0;
    }

    void MaterialsBridge::SetParallaxSelfShadow(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.ParallaxSelfShadow = _V ? 1 : 0;
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::ParallaxRefine() const {
        const auto* M = SelMat();
        return M && M->Constants.ParallaxRefine != 0;
    }

    void MaterialsBridge::SetParallaxRefine(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.ParallaxRefine = _V ? 1 : 0;
        M->UpdateConstants();
        TouchSelected();
    }

    bool MaterialsBridge::AlphaTest() const {
        const auto* M = SelMat();
        return M && M->Constants.AlphaTest != 0;
    }

    void MaterialsBridge::SetAlphaTest(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.AlphaTest = _V ? 1 : 0;
        M->UpdateConstants();
        // AlphaTest nao e so raster: decide a InstanceMask e o FORCE_NON_OPAQUE da TLAS, entra no
        // criterio de culling (IsTwoSidedForRT) e vive no snapshot do InstanceGeo. Sem isto o RT
        // seguia com o estado do load.
        TouchSelectedRT();
    }

    qreal MaterialsBridge::AlphaCutoff() const {
        const auto* M = SelMat();
        return M ? M->Constants.AlphaCutoff : 0.5;
    }

    void MaterialsBridge::SetAlphaCutoff(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.AlphaCutoff = float(_V);
        M->UpdateConstants();
        // InstanceGeo.AlphaCutoff: e o cutoff que o alpha-test dos RAIOS usa (RTAlphaTest.hlsli).
        // O toggle AlphaTest ja sincronizava; o VALOR nao — recorte do raster e do raio divergiam.
        TouchSelectedRT();
    }

    bool MaterialsBridge::BlendFlag() const {
        const auto* M = SelMat();
        return M && M->Blend;
    }

    void MaterialsBridge::SetBlendFlag(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->Blend = _V; // por-draw no Renderer: sai do G-buffer e entra no passe forward
        // No RT, Blend escolhe a CATEGORIA da instancia na TLAS (kRTMaskTranslucent): translucido
        // sai do gather do GI. Isso mora na TLAS, nao no constant buffer do material — sem o
        // rebuild, o material vira vidro no raster e continua opaco para a iluminacao indireta.
        TouchSelectedRT();
    }

    bool MaterialsBridge::TwoSided() const {
        const auto* M = SelMat();
        return M && M->TwoSided;
    }

    void MaterialsBridge::SetTwoSided(bool _V) {
        auto* M = SelMat(); if (!M) return;
        M->TwoSided = _V; // PSO escolhido por draw, por frame
        // No RT nao e por draw: alimenta o culling da instancia na TLAS e o TwoSidedRT do
        // InstanceGeo (que decide se um hit pelo verso conta como "dentro de solido").
        TouchSelectedRT();
    }

    int MaterialsBridge::ShadingModel() const {
        const auto* M = SelMat();
        return M ? int(M->Constants.ShadingModel) : 0;
    }

    void MaterialsBridge::SetShadingModel(int _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.ShadingModel = Smile::u32(_V);
        M->UpdateConstants();
        TouchSelectedRT(); // InstanceGeo.Flags bit FOLIAGE (ShadingModel == 1)
    }

    QColor MaterialsBridge::SubsurfaceColor() const {
        const auto* M = SelMat();
        if (!M) return QColor(255, 255, 255);
        const auto& S = M->Constants.SubsurfaceColor;
        return QColor::fromRgbF(std::clamp(S.X, 0.0f, 1.0f), std::clamp(S.Y, 0.0f, 1.0f),
                                std::clamp(S.Z, 0.0f, 1.0f));
    }

    void MaterialsBridge::SetSubsurfaceColor(const QColor& _V) {
        auto* M = SelMat(); if (!M) return;
        auto& S = M->Constants.SubsurfaceColor;
        S.X = float(_V.redF()); S.Y = float(_V.greenF()); S.Z = float(_V.blueF());
        M->UpdateConstants();
        TouchSelected();
    }

    qreal MaterialsBridge::SubsurfaceIntensity() const {
        const auto* M = SelMat();
        return M ? M->Constants.SubsurfaceColor.W : 0.0;
    }

    void MaterialsBridge::SetSubsurfaceIntensity(qreal _V) {
        auto* M = SelMat(); if (!M) return;
        M->Constants.SubsurfaceColor.W = float(_V);
        M->UpdateConstants();
        TouchSelected();
    }

    QVariantList MaterialsBridge::TextureSlots() const {
        QVariantList Out;
        const auto* M = SelMat();
        static const char* Labels[Smile::kMaterialTextureSlots] = {
            "Albedo", "Normal", "MetalRough", "AO", "Emissivo", "Height", "Metal", "Rough" };
        const Smile::FTexture* Slots[Smile::kMaterialTextureSlots] = {
            M ? M->Albedo : nullptr,   M ? M->Normal : nullptr,
            M ? M->MetallicRoughness : nullptr, M ? M->AO : nullptr,
            M ? M->Emissive : nullptr, M ? M->Height : nullptr,
            M ? M->Metalness : nullptr, M ? M->Roughness : nullptr };
        const auto Overrides = M ? TexOverrides.value(M) : QHash<int, QString>{};
        for (int i = 0; i < int(Smile::kMaterialTextureSlots); ++i) {
            const auto* T = Slots[i];
            const bool Has = T && T->IsValid();
            const bool Overridden = Overrides.contains(i);
            QVariantMap Slot;
            Slot[QStringLiteral("label")] = QString::fromLatin1(Labels[i]);
            Slot[QStringLiteral("has")]   = Has;
            Slot[QStringLiteral("overridden")] = Overridden;
            Slot[QStringLiteral("info")]  = Has
                ? QStringLiteral("%1×%2 · %3 · %4").arg(T->Width()).arg(T->Height())
                      .arg(FormatName(T->Format()), BytesText(TextureBytes(*T)))
                      + (Overridden ? QStringLiteral(" · ")
                                          + QFileInfo(Overrides.value(i)).fileName()
                                    : QString())
                : (Overridden ? QStringLiteral("limpo (era do cooker)")
                              : QStringLiteral("—"));
            Out.push_back(Slot);
        }
        return Out;
    }

    bool MaterialsBridge::IsModified(const Smile::FMaterial* _M) const {
        const auto It = Defaults.constFind(_M);
        if (It == Defaults.constEnd()) return false;
        const FSnapshot& S = It.value();
        return memcmp(&S.C, &_M->Constants, sizeof(Smile::MaterialConstants)) != 0 ||
               S.TwoSided != _M->TwoSided || S.Blend != _M->Blend ||
               !TexOverrides.value(_M).isEmpty();
    }

    // ---- Slots de textura (F2) ----
    Smile::FTexture** MaterialsBridge::SlotPointer(Smile::FMaterial& _M, int _Slot) const {
        switch (_Slot) {
        case 0: return &_M.Albedo;
        case 1: return &_M.Normal;
        case 2: return &_M.MetallicRoughness;
        case 3: return &_M.AO;
        case 4: return &_M.Emissive;
        case 5: return &_M.Height;
        case 6: return &_M.Metalness;
        case 7: return &_M.Roughness;
        }
        return nullptr;
    }

    Smile::u32* MaterialsBridge::SlotHasFlag(Smile::FMaterial& _M, int _Slot) const {
        auto& C = _M.Constants;
        switch (_Slot) {
        case 0: return &C.HasAlbedoMap;
        case 1: return &C.HasNormalMap;
        case 2: return &C.HasMetallicRoughnessMap;
        case 3: return &C.HasAOMap;
        case 4: return &C.HasEmissiveMap;
        case 5: return &C.HasHeightMap;
        case 6: return &C.HasMetalnessMap;
        case 7: return &C.HasRoughnessMap;
        }
        return nullptr;
    }

    void MaterialsBridge::ApplySlotTexture(Smile::FMaterial& _M, int _Slot,
                                           Smile::FTexture* _T) {
        Smile::FTexture** Ptr = SlotPointer(_M, _Slot);
        Smile::u32*      Flag = SlotHasFlag(_M, _Slot);
        if (!Ptr || !Flag || !Renderer) return;
        const bool Valid = _T && _T->IsValid();
        *Ptr  = Valid ? _T : nullptr;
        *Flag = Valid ? 1 : 0;
        if (Valid && _M.IsFinalized())
            _M.UpdateTextureSlot(Renderer->GetDevice().Native(), Renderer->GetSRVHeap(),
                                 Smile::u32(_Slot), _T);
        _M.UpdateConstants();
        // Os slots 0/2/4 viram indices bindless no InstanceGeo (Albedo/MrMap/EmissiveMap) e o
        // HasAlbedo governa o alpha-test dos RAIOS (RTAlphaTest.hlsli desiste quando e 0). Sem
        // re-subir o snapshot: adicionar albedo num material sem textura deixava a folhagem como
        // quad solido so no RT, e limpar o slot mantinha o raio amostrando o descritor antigo.
        if (Renderer) Renderer->MarkMaterialRTStateDirty();
    }

    Smile::FTexture* MaterialsBridge::LoadTextureCached(const QString& _Path, int _Slot) {
        if (!Renderer) return nullptr;
        // Semantica por slot: normal map tem mip chain propria (Toksvig), albedo/emissivo
        // sao sRGB (a view lineariza no sample); o resto e dado linear.
        const bool IsNormal = _Slot == 1;
        const bool sRGB     = _Slot == 0 || _Slot == 4;
        const QString Key = (IsNormal ? QStringLiteral("n|") :
                             sRGB     ? QStringLiteral("s|") : QStringLiteral("l|")) + _Path;
        if (Smile::FTexture* Cached = PathTexCache.value(Key, nullptr))
            return Cached;
        Smile::FTexture* T =
            Renderer->ImportRuntimeTexture(_Path.toStdWString(), IsNormal, sRGB);
        if (T) PathTexCache.insert(Key, T);
        return T;
    }

    void MaterialsBridge::browseSlotTexture(int _Slot) {
        auto* M = SelMat();
        if (!M || _Slot < 0 || _Slot >= int(Smile::kMaterialTextureSlots)) return;

        static const char* Labels[Smile::kMaterialTextureSlots] = {
            "Albedo", "Normal", "MetalRough", "AO", "Emissivo", "Height", "Metal", "Rough" };
        const QString Start = !LastTexDir.isEmpty() ? LastTexDir
                            : !JsonPath.isEmpty()   ? QFileInfo(JsonPath).path()
                                                    : QString();
        const QString File = QFileDialog::getOpenFileName(
            nullptr,
            QStringLiteral("Textura para o slot %1 — %2")
                .arg(QString::fromLatin1(Labels[_Slot]), Name()),
            Start,
            QStringLiteral("Texturas (*.png *.jpg *.jpeg *.bmp *.dds);;Todos (*.*)"));
        if (File.isEmpty()) return;
        LastTexDir = QFileInfo(File).path();

        Smile::FTexture* T = LoadTextureCached(File, _Slot);
        if (!T) {
            Smile::LogError("Materiais: falha ao carregar textura " + File.toStdString());
            return;
        }
        ApplySlotTexture(*M, _Slot, T);
        TexOverrides[M][_Slot] = File;
        TouchSelected();
    }

    void MaterialsBridge::clearSlot(int _Slot) {
        auto* M = SelMat();
        if (!M || _Slot < 0 || _Slot >= int(Smile::kMaterialTextureSlots)) return;
        ApplySlotTexture(*M, _Slot, nullptr);
        // "" no override = slot explicitamente limpo (sobrevive ao save/load).
        TexOverrides[M][_Slot] = QString();
        TouchSelected();
    }

    // ---- Selecionar/isolar na cena (F2) ----
    void MaterialsBridge::selectInScene() {
        auto* M = SelMat();
        if (!M || !Renderer) return;
        const auto& Rnds = Renderer->GetScene().Renderables();
        for (int i = 0; i < (int)Rnds.size(); ++i) {
            if (Rnds[i].Material != M || Rnds[i].RaytracingOnly) continue;
            Renderer->SetSelectedObject(i);
            CachedSelMesh = i; // Refresh nao precisa re-selecionar o material
            emit RevealInOutlinerRequested();
            return;
        }
    }

    void MaterialsBridge::ApplyIsolation() {
        auto* M = SelMat();
        if (!Renderer || !M) return;
        auto& Rnds = Renderer->GetScene().Renderables();
        if (SavedVisibility.size() != (qsizetype)Rnds.size()) return;
        for (int i = 0; i < (int)Rnds.size(); ++i) {
            if (Rnds[i].RaytracingOnly) continue;
            // Mesh escondida pelo usuario continua escondida dentro do isolar.
            Rnds[i].Visible = SavedVisibility[i] && Rnds[i].Material == M;
        }
        Renderer->GetScene().BumpTransformsVersion(); // TLAS re-coleta -> GI/reflexos seguem
        emit VisibilityChanged();
    }

    // Nao emite IsolatingChanged: quem chama ja emite (setIsolate no fim, Rebuild no seu ramo).
    void MaterialsBridge::StopIsolation() {
        if (!IsolatingOn) return;
        IsolatingOn = false;
        if (Renderer) {
            // Prefixo comum: em carga aditiva o vetor cresceu, mas os N primeiros renderables
            // continuam sendo os mesmos objetos — restaurar o que da e melhor que largar tudo
            // escondido. Os novos ja entram com o Visible que o loader definiu.
            auto& Rnds = Renderer->GetScene().Renderables();
            const int N = int(std::min<qsizetype>(SavedVisibility.size(), (qsizetype)Rnds.size()));
            for (int i = 0; i < N; ++i) Rnds[i].Visible = SavedVisibility[i];
            if (N > 0) {
                Renderer->GetScene().BumpTransformsVersion();
                emit VisibilityChanged();
            }
        }
        SavedVisibility.clear();
    }

    // ---- Preview offscreen (F3) ----
    bool MaterialsBridge::PreviewReady() const {
        return Renderer && Renderer->MaterialPreviewReady();
    }

    void MaterialsBridge::SetPreviewPrimitive(int _V) {
        if (_V < 0 || _V > 4 || PreviewParams.Primitive == _V) return;
        PreviewParams.Primitive = _V;
        PreviewDirty = true;
        emit PreviewParamsChanged();
    }

    void MaterialsBridge::SetPreviewEnabled(bool _On) {
        PreviewEnabledFlag = _On;
        if (_On) PreviewDirty = true;
    }

    QImage MaterialsBridge::PreviewImageCopy() const {
        QMutexLocker Lock(&PreviewMutex);
        return PreviewImg;
    }

    QImage MaterialsBridge::ThumbImageCopy(int _MatIdx) const {
        QMutexLocker Lock(&ThumbMutex);
        return ThumbByIdx.value(_MatIdx);
    }

    void MaterialsBridge::InvalidateThumb(int _MatIdx) {
        QMutexLocker Lock(&ThumbMutex);
        if (!ThumbByIdx.remove(_MatIdx)) return;
        ++ThumbVersion[_MatIdx];
        if (!ThumbPending.contains(_MatIdx)) ThumbPending.push_back(_MatIdx);
    }

    void MaterialsBridge::ProcessThumbQueue() {
        if (!PreviewEnabledFlag || !Renderer) return;

        // Ate 2 por frame do viewport: cada thumb e um render sincrono de ~1-2ms; o
        // browser inteiro (200+ materiais) preenche em poucos segundos sem travar a GUI.
        for (int Budget = 0; Budget < 2; ++Budget) {
            int MatIdx = -1;
            {
                QMutexLocker Lock(&ThumbMutex);
                if (ThumbPending.isEmpty()) return;
                MatIdx = ThumbPending.takeFirst();
                if (ThumbByIdx.contains(MatIdx)) { --Budget; continue; }
            }
            auto& Mats = Renderer->GetMaterials();
            if (MatIdx < 0 || MatIdx >= (int)Mats.size()) continue;

            EnsureDefaultEnv();

            Smile::FMaterialPreview::FParams P; // esfera, angulo default
            P.TransparentBackground = true;     // estilo UE: so a esfera, sem o ceu
            P.Dist = 1.45f;                     // enquadramento apertado pro icone
            std::vector<Smile::u8> Pixels;
            if (!Renderer->RenderMaterialPreview(Mats[MatIdx].get(), P, Pixels))
                return; // infra indisponivel: para a fila (re-enfileira via data())

            // Premultiplied antes do scale: downscale em alpha reto faria franja escura
            // na borda da esfera contra o fundo transparente. Dois passos (fast 256 ->
            // smooth 96): smooth direto de 1024 custaria ~10ms por thumb na fila.
            const int Size = int(Smile::FMaterialPreview::kSize);
            QImage Thumb = QImage(Pixels.data(), Size, Size, QImage::Format_RGBA8888)
                               .convertToFormat(QImage::Format_ARGB32_Premultiplied)
                               .scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation)
                               .scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            {
                QMutexLocker Lock(&ThumbMutex);
                ThumbByIdx.insert(MatIdx, Thumb);
            }
            for (int Row = 0; Row < Rows.size(); ++Row) {
                if (Rows[Row].MatIdx != MatIdx) continue;
                emit dataChanged(index(Row), index(Row), { RThumb });
                break;
            }
        }
    }

    void MaterialsBridge::orbitPreview(qreal _Dx, qreal _Dy) {
        PreviewParams.Yaw   += float(_Dx) * 0.01f;
        PreviewParams.Pitch  = std::clamp(PreviewParams.Pitch + float(_Dy) * 0.01f,
                                          -1.45f, 1.45f);
        PreviewDirty = true;
    }

    void MaterialsBridge::zoomPreview(qreal _Steps) {
        PreviewParams.Dist = std::clamp(PreviewParams.Dist * std::pow(0.9f, float(_Steps)),
                                        0.7f, 8.0f);
        PreviewDirty = true;
    }

    void MaterialsBridge::rotatePreviewEnv(qreal _Dx) {
        PreviewParams.EnvRotation += float(_Dx) * 0.01f;
        PreviewDirty = true;
    }

    void MaterialsBridge::browsePreviewHdri() {
        if (!Renderer) return;
        QString Start = LastTexDir;
#ifdef SMILE_ASSETS_DIR
        if (Start.isEmpty()) Start = QStringLiteral(SMILE_ASSETS_DIR "/HDRi");
#endif
        const QString File = QFileDialog::getOpenFileName(
            nullptr, QStringLiteral("HDRI do preview"), Start,
            QStringLiteral("HDR (*.hdr);;Todos (*.*)"));
        if (File.isEmpty()) return;
        if (!Renderer->LoadMaterialPreviewEnvironment(File.toStdWString())) {
            Smile::LogError("Materiais: falha ao carregar HDRI do preview: " +
                            File.toStdString());
            return;
        }
        PreviewDirty = true;
        emit PreviewUpdated(); // previewReady pode ter mudado
    }

    void MaterialsBridge::EnsureDefaultEnv() {
        // HDRI padrao (uma tentativa; o botao HDRI troca depois). 4k primeiro: o equirect
        // RGBA32F fica residente no FHDREnvironment — 8k custaria ~512MB contra ~128MB.
        if (!Renderer || Renderer->MaterialPreviewReady() || DefaultEnvTried) return;
        DefaultEnvTried = true;
#ifdef SMILE_ASSETS_DIR
        const QString Candidates[] = {
            QStringLiteral(SMILE_ASSETS_DIR "/Scenes/Bistro/san_giuseppe_bridge_4k.hdr"),
            QStringLiteral(SMILE_ASSETS_DIR "/HDRi/cedar_bridge_sunset_2_8k.hdr"),
        };
        for (const QString& Path : Candidates) {
            if (!QFile::exists(Path)) continue;
            if (Renderer->LoadMaterialPreviewEnvironment(Path.toStdWString())) break;
        }
#endif
    }

    void MaterialsBridge::RenderPreviewIfNeeded() {
        if (!PreviewEnabledFlag || !PreviewDirty || !Renderer) return;
        auto* M = SelMat();
        if (!M) return;

        EnsureDefaultEnv();

        std::vector<Smile::u8> Pixels;
        if (!Renderer->RenderMaterialPreview(M, PreviewParams, Pixels)) {
            PreviewDirty = false; // infra indisponivel: nao insistir todo frame
            return;
        }
        {
            QMutexLocker Lock(&PreviewMutex);
            PreviewImg = QImage(Pixels.data(),
                                int(Smile::FMaterialPreview::kSize),
                                int(Smile::FMaterialPreview::kSize),
                                QImage::Format_RGBA8888).copy();
        }
        PreviewDirty = false;
        ++PreviewSeqN;
        emit PreviewUpdated();
    }

    void MaterialsBridge::setIsolate(bool _On) {
        if (!Renderer) return;
        auto& Rnds = Renderer->GetScene().Renderables();
        if (_On) {
            if (!SelMat()) return;
            if (!IsolatingOn) {
                SavedVisibility.resize((qsizetype)Rnds.size());
                for (int i = 0; i < (int)Rnds.size(); ++i)
                    SavedVisibility[i] = Rnds[i].Visible;
            }
            IsolatingOn = true;
            ApplyIsolation();
        } else {
            StopIsolation();
        }
        emit IsolatingChanged();
    }

    bool MaterialsBridge::SelectedModified() const {
        const auto* M = SelMat();
        return M && IsModified(M);
    }

    void MaterialsBridge::resetSelected() {
        auto* M = SelMat(); if (!M) return;
        const auto It = Defaults.constFind(M);
        if (It == Defaults.constEnd()) return;
        M->Constants = It.value().C;
        M->TwoSided  = It.value().TwoSided;
        M->Blend     = It.value().Blend;
        // Texturas trocadas voltam pros mapas cozidos (o snapshot ja tem os Has*Map certos;
        // so falta reapontar o ponteiro e reescrever o SRV do slot).
        for (int i = 0; i < int(Smile::kMaterialTextureSlots); ++i) {
            Smile::FTexture* Orig = It.value().Slots[i];
            if (Smile::FTexture** Ptr = SlotPointer(*M, i)) *Ptr = Orig;
            if (Orig && Orig->IsValid() && M->IsFinalized() && Renderer)
                M->UpdateTextureSlot(Renderer->GetDevice().Native(), Renderer->GetSRVHeap(),
                                     Smile::u32(i), Orig);
        }
        TexOverrides.remove(M);
        M->UpdateConstants();
        // O override salvo (se houver) tambem cai — senao voltaria no proximo load.
        OverrideCache.remove(M->Id);
        LegacyOverrideCache.remove(QString::fromStdString(M->Name));
        TouchSelectedRT(); // reverte TUDO, inclusive os campos que o RT le
    }

    // ---- Persistencia (<cena>.materials.json) ----
    QJsonObject MaterialsBridge::MaterialToJson(const Smile::FMaterial& _M) const {
        const auto& C = _M.Constants;
        QJsonObject O;
        O[QStringLiteral("name")]               = QString::fromStdString(_M.Name);
        O[QStringLiteral("baseColor")]          = ColorToJson(C.BaseColorFactor.X, C.BaseColorFactor.Y, C.BaseColorFactor.Z);
        O[QStringLiteral("metallic")]           = C.MetallicFactor;
        O[QStringLiteral("roughness")]          = C.RoughnessFactor;
        O[QStringLiteral("aoStrength")]         = C.AOStrength;
        O[QStringLiteral("emissive")]           = ColorToJson(C.EmissiveFactor.X, C.EmissiveFactor.Y, C.EmissiveFactor.Z);
        O[QStringLiteral("emissiveStrength")]   = C.EmissiveStrength;
        O[QStringLiteral("normalStrength")]     = C.NormalStrength;
        O[QStringLiteral("normalFlipY")]        = C.NormalFlipY != 0;
        O[QStringLiteral("normalReconstructZ")] = C.NormalReconstructZ != 0;
        O[QStringLiteral("heightScale")]        = C.HeightScale;
        O[QStringLiteral("parallaxMinSteps")]   = C.ParallaxMinSteps;
        O[QStringLiteral("parallaxMaxSteps")]   = C.ParallaxMaxSteps;
        O[QStringLiteral("parallaxSelfShadow")] = C.ParallaxSelfShadow != 0;
        O[QStringLiteral("parallaxRefine")]     = C.ParallaxRefine != 0;
        O[QStringLiteral("alphaTest")]          = C.AlphaTest != 0;
        O[QStringLiteral("alphaCutoff")]        = C.AlphaCutoff;
        O[QStringLiteral("shadingModel")]       = int(C.ShadingModel);
        O[QStringLiteral("subsurface")]         = QJsonArray{
            double(C.SubsurfaceColor.X), double(C.SubsurfaceColor.Y),
            double(C.SubsurfaceColor.Z), double(C.SubsurfaceColor.W) };
        O[QStringLiteral("twoSided")]           = _M.TwoSided;
        O[QStringLiteral("blend")]              = _M.Blend;
        // Slots trocados no editor: { "1": "D:/.../normal.png", "5": "" } ("" = limpo).
        const auto Overrides = TexOverrides.value(&_M);
        if (!Overrides.isEmpty()) {
            QJsonObject Tex;
            for (auto It = Overrides.constBegin(); It != Overrides.constEnd(); ++It)
                Tex[QString::number(It.key())] = It.value();
            O[QStringLiteral("textures")] = Tex;
        }
        return O;
    }

    void MaterialsBridge::JsonToMaterial(const QJsonObject& _O, Smile::FMaterial& _M) const {
        auto& C = _M.Constants;
        auto Color3 = [&](const char* Key, float& X, float& Y, float& Z) {
            const QJsonArray A = _O.value(QLatin1String(Key)).toArray();
            if (A.size() < 3) return;
            X = float(A[0].toDouble(X)); Y = float(A[1].toDouble(Y)); Z = float(A[2].toDouble(Z));
        };
        auto Num = [&](const char* Key, float& V) {
            if (_O.contains(QLatin1String(Key))) V = float(_O.value(QLatin1String(Key)).toDouble(V));
        };
        auto Flag = [&](const char* Key, Smile::u32& V) {
            if (_O.contains(QLatin1String(Key))) V = _O.value(QLatin1String(Key)).toBool(V != 0) ? 1 : 0;
        };
        Color3("baseColor", C.BaseColorFactor.X, C.BaseColorFactor.Y, C.BaseColorFactor.Z);
        Num("metallic", C.MetallicFactor);
        Num("roughness", C.RoughnessFactor);
        Num("aoStrength", C.AOStrength);
        Color3("emissive", C.EmissiveFactor.X, C.EmissiveFactor.Y, C.EmissiveFactor.Z);
        Num("emissiveStrength", C.EmissiveStrength);
        Num("normalStrength", C.NormalStrength);
        Flag("normalFlipY", C.NormalFlipY);
        Flag("normalReconstructZ", C.NormalReconstructZ);
        Num("heightScale", C.HeightScale);
        Num("parallaxMinSteps", C.ParallaxMinSteps);
        Num("parallaxMaxSteps", C.ParallaxMaxSteps);
        Flag("parallaxSelfShadow", C.ParallaxSelfShadow);
        Flag("parallaxRefine", C.ParallaxRefine);
        Flag("alphaTest", C.AlphaTest);
        Num("alphaCutoff", C.AlphaCutoff);
        if (_O.contains(QStringLiteral("shadingModel")))
            C.ShadingModel = Smile::u32(_O.value(QStringLiteral("shadingModel")).toInt(int(C.ShadingModel)));
        {
            const QJsonArray A = _O.value(QStringLiteral("subsurface")).toArray();
            if (A.size() >= 4) {
                C.SubsurfaceColor.X = float(A[0].toDouble());
                C.SubsurfaceColor.Y = float(A[1].toDouble());
                C.SubsurfaceColor.Z = float(A[2].toDouble());
                C.SubsurfaceColor.W = float(A[3].toDouble());
            }
        }
        if (_O.contains(QStringLiteral("twoSided"))) _M.TwoSided = _O.value(QStringLiteral("twoSided")).toBool(_M.TwoSided);
        if (_O.contains(QStringLiteral("blend")))    _M.Blend    = _O.value(QStringLiteral("blend")).toBool(_M.Blend);
    }

    bool MaterialsBridge::saveMaterials() {
        if (!Renderer || JsonPath.isEmpty()) return false;

        // Overrides = materiais que divergem do default cozido AGORA. O cache tambem e
        // atualizado: um material revertido some do arquivo.
        QJsonArray Arr;
        QHash<quint64, QJsonObject> NewCache;
        for (const auto& M : Renderer->GetMaterials()) {
            if (M->Name.empty() || !IsModified(M.get())) continue;
            QJsonObject O = MaterialToJson(*M);
            // Chave = id de conteudo em hex. O "name" continua no arquivo, mas so p/ leitura
            // humana — nao e mais usado para casar override com material.
            O[QStringLiteral("id")] = QString::number(M->Id, 16);
            // Materiais identicos compartilham id de proposito (ver FMaterial::Id): o insert
            // colapsa as copias numa entrada so, que o load reaplica em todas.
            if (NewCache.contains(M->Id)) continue;
            NewCache.insert(M->Id, O);
            Arr.push_back(O);
        }
        OverrideCache = std::move(NewCache);
        LegacyOverrideCache.clear(); // migrado: o arquivo abaixo ja sai em v2

        QJsonObject Root;
        Root[QStringLiteral("version")]   = 2; // v2 = chaveado por id; v1 era por nome
        Root[QStringLiteral("materials")] = Arr;

        QFile File(JsonPath);
        if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Smile::LogError("Materiais: falha ao salvar " + JsonPath.toStdString());
            return false;
        }
        File.write(QJsonDocument(Root).toJson(QJsonDocument::Indented));
        Smile::LogInfo("Materiais: " + std::to_string(Arr.size()) + " override(s) salvos em " +
                       JsonPath.toStdString());
        if (JsonDirty) { JsonDirty = false; emit DirtyChanged(); }
        return true;
    }
}
