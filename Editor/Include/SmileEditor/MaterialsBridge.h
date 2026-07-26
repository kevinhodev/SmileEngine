#pragma once

#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/MaterialPreview.h"
#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

namespace Smile { class Renderer; class FTexture; }

namespace SmileEditor {
    // Ponte C++<->QML do Editor de Materiais (MaterialsWindow.qml). Modelo flat dos materiais
    // importados (Renderer::GetMaterials()), com busca/filtro reconstruindo a lista no C++
    // (racional do SceneOutlinerBridge). Edicao e AO VIVO: os setters mutam
    // FMaterial::Constants e chamam UpdateConstants() (CBV upload mapeado — a cena inteira
    // reage no frame seguinte, estilo Material Instance da Flax). TwoSided/Blend tambem sao
    // ao vivo: o Renderer escolhe PSO/passe por draw, por frame.
    //
    // Persistencia: <cena>.materials.json (sidecar, mesma convencao do .visibility.json) —
    // por NOME de material, so os que divergem do default cozido (snapshot no Rebuild).
    // Cargas aditivas reaplicam os overrides por nome.
    class MaterialsBridge : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(int totalCount READ TotalCount NOTIFY StructureChanged)
        Q_PROPERTY(QString search READ Search WRITE SetSearch NOTIFY FiltersChanged)
        Q_PROPERTY(int filter READ Filter WRITE SetFilter NOTIFY FiltersChanged)
        Q_PROPERTY(bool dirty READ Dirty NOTIFY DirtyChanged)

        // ---- Material selecionado (cards do painel direito) ----
        Q_PROPERTY(bool hasSelection READ HasSelection NOTIFY SelectionChanged)
        Q_PROPERTY(QString name READ Name NOTIFY SelectionChanged)
        Q_PROPERTY(int meshCount READ MeshCount NOTIFY SelectionChanged)
        Q_PROPERTY(QString vramText READ VramText NOTIFY SelectionChanged)
        Q_PROPERTY(QColor baseColor READ BaseColor WRITE SetBaseColor NOTIFY SelectionChanged)
        Q_PROPERTY(qreal metallic READ Metallic WRITE SetMetallic NOTIFY SelectionChanged)
        Q_PROPERTY(qreal roughness READ Roughness WRITE SetRoughness NOTIFY SelectionChanged)
        Q_PROPERTY(qreal aoStrength READ AOStrength WRITE SetAOStrength NOTIFY SelectionChanged)
        Q_PROPERTY(QColor emissiveColor READ EmissiveColor WRITE SetEmissiveColor NOTIFY SelectionChanged)
        Q_PROPERTY(qreal emissiveStrength READ EmissiveStrength WRITE SetEmissiveStrength NOTIFY SelectionChanged)
        Q_PROPERTY(qreal normalStrength READ NormalStrength WRITE SetNormalStrength NOTIFY SelectionChanged)
        Q_PROPERTY(bool normalFlipY READ NormalFlipY WRITE SetNormalFlipY NOTIFY SelectionChanged)
        Q_PROPERTY(bool normalReconstructZ READ NormalReconstructZ WRITE SetNormalReconstructZ NOTIFY SelectionChanged)
        Q_PROPERTY(bool hasNormalMap READ HasNormalMap NOTIFY SelectionChanged)
        Q_PROPERTY(bool hasHeightMap READ HasHeightMap NOTIFY SelectionChanged)
        Q_PROPERTY(qreal heightScale READ HeightScale WRITE SetHeightScale NOTIFY SelectionChanged)
        Q_PROPERTY(int parallaxMinSteps READ ParallaxMinSteps WRITE SetParallaxMinSteps NOTIFY SelectionChanged)
        Q_PROPERTY(int parallaxMaxSteps READ ParallaxMaxSteps WRITE SetParallaxMaxSteps NOTIFY SelectionChanged)
        Q_PROPERTY(bool parallaxSelfShadow READ ParallaxSelfShadow WRITE SetParallaxSelfShadow NOTIFY SelectionChanged)
        Q_PROPERTY(bool parallaxRefine READ ParallaxRefine WRITE SetParallaxRefine NOTIFY SelectionChanged)
        Q_PROPERTY(bool alphaTest READ AlphaTest WRITE SetAlphaTest NOTIFY SelectionChanged)
        Q_PROPERTY(qreal alphaCutoff READ AlphaCutoff WRITE SetAlphaCutoff NOTIFY SelectionChanged)
        Q_PROPERTY(bool blend READ BlendFlag WRITE SetBlendFlag NOTIFY SelectionChanged)
        Q_PROPERTY(bool twoSided READ TwoSided WRITE SetTwoSided NOTIFY SelectionChanged)
        Q_PROPERTY(int shadingModel READ ShadingModel WRITE SetShadingModel NOTIFY SelectionChanged)
        Q_PROPERTY(QColor subsurfaceColor READ SubsurfaceColor WRITE SetSubsurfaceColor NOTIFY SelectionChanged)
        Q_PROPERTY(qreal subsurfaceIntensity READ SubsurfaceIntensity WRITE SetSubsurfaceIntensity NOTIFY SelectionChanged)
        // Slots de textura: [{ label, has, info, overridden }] × 8 (F2: clique troca o mapa).
        Q_PROPERTY(QVariantList textureSlots READ TextureSlots NOTIFY SelectionChanged)
        Q_PROPERTY(bool selectedModified READ SelectedModified NOTIFY SelectionChanged)
        // Isolar na cena: só as meshes do material selecionado ficam visíveis (TLAS segue).
        Q_PROPERTY(bool isolating READ Isolating NOTIFY IsolatingChanged)
        // ---- Preview offscreen (F3): imagem via image://materialpreview/p<seq> ----
        Q_PROPERTY(int previewSeq READ PreviewSeq NOTIFY PreviewUpdated)
        Q_PROPERTY(bool previewReady READ PreviewReady NOTIFY PreviewUpdated)
        Q_PROPERTY(int previewPrimitive READ PreviewPrimitive WRITE SetPreviewPrimitive NOTIFY PreviewParamsChanged)

    public:
        enum ERoles {
            RName = Qt::UserRole + 1,
            RMeshCount,
            RVramText,
            RBadges,     // QStringList: POM/FOL/MASK/BLEND/2S/EMIS
            RSwatch,     // QColor do BaseColorFactor
            RSelected,
            RModified,   // diverge do default cozido (dot no browser)
            RThumb       // url image://materialthumb/<idx>_v<n> ("" = ainda sem thumbnail)
        };

        // Chips de filtro do browser.
        enum EFilter { FTodos = 0, FOpacos = 1, FBlend = 2, FFoliage = 3, FPom = 4 };
        Q_ENUM(EFilter)

        explicit MaterialsBridge(QObject* parent = nullptr);

        void SetRenderer(Smile::Renderer* R);            // MainWindow, no RendererInitialized
        void OnSceneLoaded(const QString& ScenePath, bool Additive);

        // QAbstractListModel
        int rowCount(const QModelIndex& Parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& Index, int Role) const override;
        QHash<int, QByteArray> roleNames() const override;

        bool    Available() const { return Renderer != nullptr; }
        int     TotalCount() const;
        QString Search() const { return SearchText; }
        void    SetSearch(const QString& V);
        int     Filter() const { return FilterKind; }
        void    SetFilter(int V);
        bool    Dirty() const { return JsonDirty; }

        bool    HasSelection() const;
        QString Name() const;
        int     MeshCount() const;
        QString VramText() const;
        QColor  BaseColor() const;
        void    SetBaseColor(const QColor& V);
        qreal   Metallic() const;
        void    SetMetallic(qreal V);
        qreal   Roughness() const;
        void    SetRoughness(qreal V);
        qreal   AOStrength() const;
        void    SetAOStrength(qreal V);
        QColor  EmissiveColor() const;
        void    SetEmissiveColor(const QColor& V);
        qreal   EmissiveStrength() const;
        void    SetEmissiveStrength(qreal V);
        qreal   NormalStrength() const;
        void    SetNormalStrength(qreal V);
        bool    NormalFlipY() const;
        void    SetNormalFlipY(bool V);
        bool    NormalReconstructZ() const;
        void    SetNormalReconstructZ(bool V);
        bool    HasNormalMap() const;
        bool    HasHeightMap() const;
        qreal   HeightScale() const;
        void    SetHeightScale(qreal V);
        int     ParallaxMinSteps() const;
        void    SetParallaxMinSteps(int V);
        int     ParallaxMaxSteps() const;
        void    SetParallaxMaxSteps(int V);
        bool    ParallaxSelfShadow() const;
        void    SetParallaxSelfShadow(bool V);
        bool    ParallaxRefine() const;
        void    SetParallaxRefine(bool V);
        bool    AlphaTest() const;
        void    SetAlphaTest(bool V);
        qreal   AlphaCutoff() const;
        void    SetAlphaCutoff(qreal V);
        bool    BlendFlag() const;
        void    SetBlendFlag(bool V);
        bool    TwoSided() const;
        void    SetTwoSided(bool V);
        int     ShadingModel() const;
        void    SetShadingModel(int V);
        QColor  SubsurfaceColor() const;
        void    SetSubsurfaceColor(const QColor& V);
        qreal   SubsurfaceIntensity() const;
        void    SetSubsurfaceIntensity(qreal V);
        QVariantList TextureSlots() const;
        bool    SelectedModified() const;

        bool Isolating() const { return IsolatingOn; }

        int  PreviewSeq() const { return PreviewSeqN; }
        bool PreviewReady() const;
        int  PreviewPrimitive() const { return PreviewParams.Primitive; }
        void SetPreviewPrimitive(int V);
        void SetPreviewEnabled(bool On);       // MainWindow (show/hide da janela)
        QImage PreviewImageCopy() const;       // provider (pode vir da render thread do QML)
        QImage ThumbImageCopy(int MatIdx) const; // provider dos thumbnails do browser

        Q_INVOKABLE void selectRow(int row);
        Q_INVOKABLE int  selectedRowIndex() const;      // linha na lista filtrada (-1 = fora)
        Q_INVOKABLE void resetSelected();               // volta ao default cozido
        Q_INVOKABLE bool saveMaterials();               // grava o sidecar; limpa dirty
        Q_INVOKABLE void closeWindow() { emit CloseRequested(); }
        // F2: troca/limpeza de mapa num slot (0..7). browse abre QFileDialog; o path vira
        // override persistido no sidecar. clear zera o Has*Map (SRV fica, shader ignora).
        Q_INVOKABLE void browseSlotTexture(int slot);
        Q_INVOKABLE void clearSlot(int slot);
        // F2: picking reverso — seleciona a primeira mesh do material no renderer e pede
        // pro MainWindow revelar o Outliner (que segue a selecao sozinho).
        Q_INVOKABLE void selectInScene();
        // F2: isolar liga/desliga; guarda o Visible anterior e restaura ao desligar.
        Q_INVOKABLE void setIsolate(bool on);
        // F3: interacao do preview (arraste orbita, scroll aproxima, HDRI proprio).
        Q_INVOKABLE void orbitPreview(qreal dx, qreal dy);
        Q_INVOKABLE void zoomPreview(qreal steps);
        Q_INVOKABLE void rotatePreviewEnv(qreal dx);
        Q_INVOKABLE void browsePreviewHdri();

    public slots:
        void Rebuild();  // cena/materiais mudaram — refaz a lista (snapshot + overrides)
        void Refresh();  // FrameReady: segue o picking do viewport (material da mesh clicada)

    signals:
        void AvailableChanged();
        void StructureChanged();
        void SelectionChanged();
        void FiltersChanged();
        void DirtyChanged();
        void CloseRequested();
        void ScrollToRequested(int row); // selecao veio do viewport: QML rola ate a linha
        void IsolatingChanged();
        void VisibilityChanged();        // isolar mexeu em FRenderable::Visible -> Outliner refaz
        void RevealInOutlinerRequested(); // MainWindow mostra o dock do Outliner
        void PreviewUpdated();           // nova imagem (previewSeq) / previewReady mudou
        void PreviewParamsChanged();

    private:
        struct FRow {
            int         MatIdx    = -1;   // indice em Renderer->GetMaterials()
            QString     Name;
            int         MeshCount = 0;
            quint64     VramBytes = 0;
            QStringList Badges;
            QColor      Swatch;
        };
        struct FSnapshot {
            Smile::MaterialConstants C;
            bool TwoSided = false;
            bool Blend    = false;
            Smile::FTexture* Slots[Smile::kMaterialTextureSlots] = {}; // ponteiros cozidos
        };

        Smile::FMaterial* SelMat() const;
        void   RebuildRows();
        // Slot helpers (0..7 na ordem Albedo/Normal/MR/AO/Emissivo/Height/Metal/Rough).
        Smile::FTexture** SlotPointer(Smile::FMaterial& M, int Slot) const;
        Smile::u32*       SlotHasFlag(Smile::FMaterial& M, int Slot) const;
        void   ApplySlotTexture(Smile::FMaterial& M, int Slot, Smile::FTexture* T);
        Smile::FTexture* LoadTextureCached(const QString& Path, int Slot);
        void   ApplyIsolation();         // Visible = (Material == selecionado)
        // Desliga o isolar RESTAURANDO o Visible salvo (prefixo comum, p/ o caso de carga
        // aditiva ter crescido o vetor de renderables). Usar sempre que a cena continuar viva —
        // largar SavedVisibility sem restaurar deixa a cena escondida sem volta pela UI.
        void   StopIsolation();
        void   RenderPreviewIfNeeded();  // Refresh: renderiza quando dirty + janela aberta
        void   MarkPreviewDirty() { PreviewDirty = true; }
        void   EnsureDefaultEnv();       // HDRI padrao (uma tentativa por sessao/cena)
        void   ProcessThumbQueue();      // Refresh: ate 2 thumbnails por frame
        void   InvalidateThumb(int MatIdx);
        bool   MatchesFilter(const Smile::FMaterial& M) const;
        void   MarkDirty();
        // Notifica edicao do selecionado: SelectionChanged + dataChanged da linha
        // (badges/swatch/dot de modificado podem mudar).
        void   TouchSelected();
        // Igual ao TouchSelected, mais o pedido de refresh do snapshot InstanceGeo. Use nos
        // setters cujo campo o RAY TRACING le (ver FDDGI::FillInstanceGeo): cor base, metallic,
        // roughness, emissivo, cutoff, shading model, alpha-test/blend/two-sided e troca de
        // textura. Campos so-raster (parallax, NormalStrength, AOStrength, subsurface) devem usar
        // o TouchSelected puro — marcar a toa reseta o historico do GI/denoiser sem motivo.
        void   TouchSelectedRT();
        bool   IsModified(const Smile::FMaterial* M) const;
        // Aplica OverrideCache por nome, SO nos materiais recem-vistos (_Fresh). Reaplicar em
        // material ja carregado descartaria a edicao nao salva dele — o Rebuild dispara tambem
        // em carga aditiva, nao so na abertura da cena.
        void   ApplyOverrides(const QSet<const Smile::FMaterial*>& _Fresh);
        QJsonObject MaterialToJson(const Smile::FMaterial& M) const;
        void   JsonToMaterial(const QJsonObject& O, Smile::FMaterial& M) const;

        Smile::Renderer* Renderer = nullptr;
        QVector<FRow>    Rows;
        int              SelectedMat = -1;             // indice em GetMaterials()
        QString          SearchText;
        int              FilterKind = FTodos;
        QString          JsonPath;                     // <cena>.materials.json (nao-aditiva)
        bool             JsonDirty = false;
        QHash<const Smile::FMaterial*, FSnapshot> Defaults;   // default cozido por material
        QHash<QString, QJsonObject>               OverrideCache; // sidecar carregado, por nome
        // Texturas trocadas no editor: material -> (slot -> path; "" = slot limpo).
        QHash<const Smile::FMaterial*, QHash<int, QString>> TexOverrides;
        QHash<QString, Smile::FTexture*> PathTexCache; // mesma imagem em 2 slots = 1 upload
        QString LastTexDir;                            // pasta do ultimo browse

        bool          IsolatingOn = false;
        QVector<bool> SavedVisibility;                 // Visible antes do isolar (por indice)

        // ---- Preview (F3) ----
        Smile::FMaterialPreview::FParams PreviewParams;
        QImage         PreviewImg;
        mutable QMutex PreviewMutex;       // requestImage pode vir da render thread do QML
        int            PreviewSeqN     = 0;
        bool           PreviewDirty   = true;
        bool           PreviewEnabledFlag = false;     // janela Materiais visivel
        bool           DefaultEnvTried   = false;      // HDRI padrao ja tentado

        // Thumbnails do browser: esfera renderizada por material, 96x96, sob demanda
        // (data() enfileira, Refresh drena 2/frame). Chaves por INDICE de material — o
        // provider roda na render thread do QML e nao pode tocar no Renderer.
        mutable QMutex        ThumbMutex;
        mutable QHash<int, QImage> ThumbByIdx;
        mutable QHash<int, int>    ThumbVersion;
        mutable QVector<int>       ThumbPending;

        // Espelhos p/ o Refresh detectar mudanca externa (load de cena, picking).
        int CachedMatCount = -1;
        int CachedSelMesh  = -1;
    };

    // Serve a imagem do preview pro QML (Image { source: "image://materialpreview/..." }).
    // O id carrega o previewSeq so pra furar o cache do QML; o conteudo vem do bridge.
    class MaterialPreviewImageProvider : public QQuickImageProvider {
    public:
        explicit MaterialPreviewImageProvider(MaterialsBridge* Bridge)
            : QQuickImageProvider(QQuickImageProvider::Image), Bridge(Bridge) {}

        QImage requestImage(const QString&, QSize* Size, const QSize&) override {
            QImage Img = Bridge ? Bridge->PreviewImageCopy() : QImage();
            if (Size) *Size = Img.size();
            return Img;
        }

    private:
        MaterialsBridge* Bridge = nullptr;
    };

    // Thumbnails do browser (image://materialthumb/<idx>_v<n>). O sufixo _v so fura o
    // cache do QML quando o material e editado.
    class MaterialThumbImageProvider : public QQuickImageProvider {
    public:
        explicit MaterialThumbImageProvider(MaterialsBridge* Bridge)
            : QQuickImageProvider(QQuickImageProvider::Image), Bridge(Bridge) {}

        QImage requestImage(const QString& Id, QSize* Size, const QSize&) override {
            const int MatIdx = Id.left(Id.indexOf(QLatin1Char('_'))).toInt();
            QImage Img = Bridge ? Bridge->ThumbImageCopy(MatIdx) : QImage();
            if (Size) *Size = Img.size();
            return Img;
        }

    private:
        MaterialsBridge* Bridge = nullptr;
    };
}
