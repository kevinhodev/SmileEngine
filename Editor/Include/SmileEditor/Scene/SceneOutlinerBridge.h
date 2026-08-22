#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QString>
#include <QVector>
#include "SmileEditor/Viewport/RenderThread.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Modelo flat e virtualizavel da hierarquia da cena. Edicao de luzes e ambiente permanece nas
    // bridges de dominio; este tipo cuida de hierarquia, selecao e visibilidade de meshes.
    class SceneOutlinerBridge : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(int totalCount READ TotalCount NOTIFY StructureChanged)
        Q_PROPERTY(int selectedCount READ SelectedCount NOTIFY SelectionChanged)
        Q_PROPERTY(int hiddenCount READ HiddenCount NOTIFY StructureChanged)
        Q_PROPERTY(QString search READ Search WRITE SetSearch NOTIFY FiltersChanged)
        Q_PROPERTY(int filter READ Filter WRITE SetFilter NOTIFY FiltersChanged)
        Q_PROPERTY(bool oceanVisible READ OceanVisible WRITE SetOceanVisible NOTIFY EnvChanged)
        Q_PROPERTY(bool terrainVisible READ TerrainVisible WRITE SetTerrainVisible NOTIFY EnvChanged)
        Q_PROPERTY(int selectedEnvironment READ SelectedEnvironment NOTIFY SelectionChanged)
        Q_PROPERTY(bool meshSelected READ MeshSelected NOTIFY SelectionChanged)
        Q_PROPERTY(QString meshName READ MeshName NOTIFY SelectionChanged)
        // Propriedades da mesh SELECIONADA (card do painel; defaults vazios sem selecao).
        Q_PROPERTY(QString meshMaterial READ MeshMaterial NOTIFY SelectionChanged)
        Q_PROPERTY(int meshTris READ MeshTris NOTIFY SelectionChanged)
        Q_PROPERTY(int meshVerts READ MeshVerts NOTIFY SelectionChanged)
        Q_PROPERTY(QString meshVramText READ MeshVramText NOTIFY SelectionChanged)
        Q_PROPERTY(QStringList meshFlags READ MeshFlags NOTIFY SelectionChanged)
        // Transform da mesh selecionada. O gizmo ja escreve direto em Renderables()[i].Transform;
        // estas propriedades sao a mesma edicao pelo teclado, para o Inspector. Nove escalares
        // em vez de tres Vec3 porque o QML edita um campo por vez e um setter de vetor faria
        // cada digito reescrever os outros dois eixos.
        Q_PROPERTY(double meshPosX READ MeshPosX WRITE SetMeshPosX NOTIFY TransformChanged)
        Q_PROPERTY(double meshPosY READ MeshPosY WRITE SetMeshPosY NOTIFY TransformChanged)
        Q_PROPERTY(double meshPosZ READ MeshPosZ WRITE SetMeshPosZ NOTIFY TransformChanged)
        Q_PROPERTY(double meshRotX READ MeshRotX WRITE SetMeshRotX NOTIFY TransformChanged)
        Q_PROPERTY(double meshRotY READ MeshRotY WRITE SetMeshRotY NOTIFY TransformChanged)
        Q_PROPERTY(double meshRotZ READ MeshRotZ WRITE SetMeshRotZ NOTIFY TransformChanged)
        Q_PROPERTY(double meshScaleX READ MeshScaleX WRITE SetMeshScaleX NOTIFY TransformChanged)
        Q_PROPERTY(double meshScaleY READ MeshScaleY WRITE SetMeshScaleY NOTIFY TransformChanged)
        Q_PROPERTY(double meshScaleZ READ MeshScaleZ WRITE SetMeshScaleZ NOTIFY TransformChanged)
        // Escala uniforme: o elo do Inspector escreve os tres eixos de uma vez.
        Q_PROPERTY(bool meshScaleLocked READ MeshScaleLocked WRITE SetMeshScaleLocked NOTIFY ScaleLockChanged)
        Q_PROPERTY(bool meshVisible READ MeshVisible NOTIFY SelectionChanged)
        // Mobilidade (ver Smile::EMobility). Gravavel: e a unica propriedade de mesh que o
        // painel edita, e ela vai para o .smap junto com transform e visibilidade.
        Q_PROPERTY(bool meshDynamic READ MeshDynamic WRITE SetMeshDynamic NOTIFY SelectionChanged)
        // Visibilidade tem persistencia propria (<cena>.visibility.json, botao salvar).
        Q_PROPERTY(bool dirty READ Dirty NOTIFY DirtyChanged)

    public:
        // Tipos de linha (role `kind` no delegate).
        enum ERowKind {
            KGroup = 0,
            KEnv = 1,
            KLight = 2,
            KAsset = 3,
            KMesh = 4,
            KMore = 5
        };
        Q_ENUM(ERowKind)

        // Grupos (id em `sceneIdx` quando kind == KGroup) / filtro dos chips (0 = Tudo).
        enum EGroup { GAmbiente = 0, GLuzes = 1, GMeshes = 2 };
        Q_ENUM(EGroup)

        // Itens de ambiente (id em `sceneIdx` quando kind == KEnv).
        enum EEnvItem {
            EnvSol = 0,
            EnvNuvens = 1,
            EnvOceano = 2,
            EnvTerreno = 3,
            EnvClima = 4
        };
        Q_ENUM(EEnvItem)

        enum ERoles {
            RKind = Qt::UserRole + 1,
            RDepth,
            RName,
            RIcon,
            RCount,      // badge de contagem (grupos/pastas; -1 = sem badge)
            RExpanded,
            REnabled,    // estado do olho (luz Enabled / nuvens / oceano)
            RHasEye,
            RDotColor,   // cor da luz (dot); invalida = sem dot
            RHasDot,
            RSelected,
            RSceneIdx    // indice real: luz em Lights(), mesh em Renderables(), id de env/grupo
        };

        explicit SceneOutlinerBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle R);             // MainWindow, no RendererInitialized
        void OnSceneLoaded(const QString& ScenePath, bool Additive); // apos LoadCookedScene OK

        // QAbstractListModel
        int rowCount(const QModelIndex& Parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& Index, int Role) const override;
        QHash<int, QByteArray> roleNames() const override;

        bool    Available() const { return static_cast<bool>(Renderer); }
        int     TotalCount() const;
        int     SelectedCount() const;
        int     HiddenCount() const;
        QString Search() const { return SearchText; }
        void    SetSearch(const QString& V);
        int     Filter() const { return FilterGroup; }
        void    SetFilter(int V);
        bool    OceanVisible() const;
        void    SetOceanVisible(bool V);
        bool    TerrainVisible() const;
        void    SetTerrainVisible(bool V);
        int     SelectedEnvironment() const { return SelectedEnv; }
        bool    MeshSelected() const;
        QString MeshName() const;
        QString MeshMaterial() const;
        int     MeshTris() const;
        int     MeshVerts() const;
        QString MeshVramText() const;
        QStringList MeshFlags() const;
        bool    MeshVisible() const;
        bool    MeshDynamic() const;
        void    SetMeshDynamic(bool V);
        double  MeshPosX() const;   void SetMeshPosX(double V);
        double  MeshPosY() const;   void SetMeshPosY(double V);
        double  MeshPosZ() const;   void SetMeshPosZ(double V);
        double  MeshRotX() const;   void SetMeshRotX(double V);
        double  MeshRotY() const;   void SetMeshRotY(double V);
        double  MeshRotZ() const;   void SetMeshRotZ(double V);
        double  MeshScaleX() const; void SetMeshScaleX(double V);
        double  MeshScaleY() const; void SetMeshScaleY(double V);
        double  MeshScaleZ() const; void SetMeshScaleZ(double V);
        bool    MeshScaleLocked() const { return ScaleLocked; }
        void    SetMeshScaleLocked(bool V);
        bool    Dirty() const { return VisDirty; }

        Q_INVOKABLE void toggleExpand(int row);
        Q_INVOKABLE void showMore(int row);      // amplia o lote da pasta indicado por KMore
        Q_INVOKABLE void selectRow(int row);    // ambiente/luz/mesh: selecao exclusiva
        // Pasta aplica visibilidade ao range inteiro do asset.
        Q_INVOKABLE void toggleEye(int row);
        // Enquadra mesh ou luz mantendo a orientacao da camera.
        Q_INVOKABLE void focusRow(int row);
        Q_INVOKABLE int  selectedRowIndex() const; // linha selecionada na lista flat (-1 = fora)
        // Setas cima/baixo: move a selecao pra proxima linha selecionavel.
        Q_INVOKABLE void selectStep(int delta);
        // Salva o estado de visibilidade (meshes ocultas por asset + terreno) no
        // <cena>.visibility.json — chamado pelo botao salvar junto do saveLights.
        Q_INVOKABLE bool saveVisibility();
        Q_INVOKABLE void closePanel() { emit CloseRequested(); }

        // Mutacoes usam Id porque indices deixam de ser validos depois da operacao.
        Q_INVOKABLE bool deleteSelectedMesh();
        Q_INVOKABLE bool duplicateSelectedMesh();

    public slots:
        void Rebuild();  // estrutura mudou (cena/luzes) — reconstroi a lista flat
        void Refresh();  // FrameReady: detecta selecao por picking e toggles externos

    signals:
        void AvailableChanged();
        void StructureChanged();
        void SelectionChanged();
        void TransformChanged();
        void ScaleLockChanged();
        void FiltersChanged();
        void EnvChanged();
        void DirtyChanged();
        void SceneChanged();
        void CloseRequested();
        void ScrollToRequested(int row); // selecao veio do viewport: QML rola ate a linha

    private:
        struct FRow {
            int     Kind      = KMesh;
            int     Depth     = 0;
            QString Name;
            QString Icon;
            int     Count     = -1;
            bool    Expanded  = false;
            bool    Enabled   = true;
            bool    HasEye    = false;
            QColor  Dot;                 // invalida = sem dot
            int     SceneIdx  = -1;
        };
        struct FAssetRange {
            QString Name;
            int     Begin = 0;
            int     End   = 0;   // exclusivo, em Scene.Renderables()
        };

        void RebuildRows(QVector<FRow>& Out) const;
        bool RowSelected(const FRow& Row) const;
        // Corrige ranges [Begin, End) depois da remocao de um renderavel.
        void ShiftAssetRangesAfterRemoval(int RemovedIndex);
        // Troca Rows preservando o scroll: mesma estrutura -> dataChanged; senao reset.
        void ApplyRows(QVector<FRow>&& NewRows);
        bool MatchesSearch(const QString& Name) const;
        void MarkDirty();
        // Um acessor por componente (0-2 posicao, 3-5 rotacao, 6-8 escala) em vez de nove
        // pares de metodos com o mesmo corpo.
        void   WriteMeshTransform(int Component, double Value);
        double ReadMeshTransform(int Component) const;
        bool LoadVisibility(); // le o JsonPath e aplica Visible/terreno nos assets presentes
        // Garante que a linha da selecao exista na lista flat (expande grupo/pasta e
        // reseta o chip de filtro se preciso). Retorna a linha, ou -1 se a busca a esconde.
        int  RevealSelection();

        RendererHandle Renderer;
        QVector<FRow>        Rows;
        QVector<FAssetRange> Assets;
        QHash<int, int>       AssetVisibleLimits; // indice do asset -> meshes reveladas
        QString JsonPath;          // <cena>.visibility.json da cena principal (nao-aditiva)
        bool    VisDirty = false;
        bool    ScaleLocked = true;   // elo da escala uniforme (Inspector)
        Smile::u64 CachedTransformsVersion = 0;
        QVector<bool>        AssetExpanded;      // paralelo a Assets
        bool    GroupExpanded[3] = { true, true, true };
        QString SearchText;
        int     FilterGroup = 0;                 // 0=Tudo, senao EGroup+1

        // Espelhos p/ o Refresh detectar mudanca externa (picking, settings, load).
        int  CachedSelMesh   = -1;
        int  CachedSelLight  = -1;
        int  SelectedEnv     = -1;
        int  CachedRenderableCount = -1;
        int  CachedLightCount      = -1;
        bool CachedClouds    = true;
        bool CachedOcean     = true;
        bool CachedTerrain   = false; // Terrain.IsLoaded()
        bool CachedTerrainOn = true;  // GetUseTerrain() (olho)
    };
}
