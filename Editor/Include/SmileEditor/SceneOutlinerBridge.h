#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QString>
#include <QVector>

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Ponte C++<->QML do Scene Outliner (SceneOutlinerPanel.qml). Modelo FLAT da arvore da
    // cena: os grupos (Ambiente/Luzes/Meshes), pastas por asset importado e as linhas folha
    // viram uma lista de linhas visiveis que a ListView virtualiza — expand/colapso, busca e
    // filtro reconstroem a lista aqui no C++ (Rebuild), nunca no QML (2k+ meshes).
    //
    // Le a cena direto do Renderer (mesma thread da GUI, racional do LightsBridge). As ACOES
    // de luz (add/remover/duplicar/toggle/propriedades) continuam no LightsBridge — o QML
    // chama lightsModel; o MainWindow conecta LightsChanged -> Rebuild daqui. Nuvens ligam no
    // ViewportWidget (viewportModel.cloudsEnabled); o oceano nao tinha toggle no editor, entao
    // ele mora aqui (oceanVisible -> Renderer::SetUseWater).
    //
    // Agrupamento de meshes: MainWindow avisa OnSceneLoaded apos cada LoadCookedScene; cada
    // carga vira uma "pasta" com o range [begin, end) em Scene.Renderables(). Renderables
    // RaytracingOnly (proxy do terreno) ficam fora da lista.
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
        Q_PROPERTY(bool meshSelected READ MeshSelected NOTIFY SelectionChanged)
        Q_PROPERTY(QString meshName READ MeshName NOTIFY SelectionChanged)
        // Propriedades da mesh SELECIONADA (card do painel; defaults vazios sem selecao).
        Q_PROPERTY(QString meshMaterial READ MeshMaterial NOTIFY SelectionChanged)
        Q_PROPERTY(int meshTris READ MeshTris NOTIFY SelectionChanged)
        Q_PROPERTY(int meshVerts READ MeshVerts NOTIFY SelectionChanged)
        Q_PROPERTY(QString meshVramText READ MeshVramText NOTIFY SelectionChanged)
        Q_PROPERTY(QStringList meshFlags READ MeshFlags NOTIFY SelectionChanged)
        Q_PROPERTY(bool meshVisible READ MeshVisible NOTIFY SelectionChanged)
        // Visibilidade tem persistencia propria (<cena>.visibility.json, botao salvar).
        Q_PROPERTY(bool dirty READ Dirty NOTIFY DirtyChanged)

    public:
        // Tipos de linha (role `kind` no delegate).
        enum ERowKind { KGroup = 0, KEnv = 1, KLight = 2, KAsset = 3, KMesh = 4 };
        Q_ENUM(ERowKind)

        // Grupos (id em `sceneIdx` quando kind == KGroup) / filtro dos chips (0 = Tudo).
        enum EGroup { GAmbiente = 0, GLuzes = 1, GMeshes = 2 };
        Q_ENUM(EGroup)

        // Itens de ambiente (id em `sceneIdx` quando kind == KEnv).
        enum EEnvItem { EnvSol = 0, EnvNuvens = 1, EnvOceano = 2, EnvTerreno = 3 };
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

        void SetRenderer(Smile::Renderer* R);           // MainWindow, no RendererInitialized
        void OnSceneLoaded(const QString& ScenePath, bool Additive); // apos LoadCookedScene OK

        // QAbstractListModel
        int rowCount(const QModelIndex& Parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& Index, int Role) const override;
        QHash<int, QByteArray> roleNames() const override;

        bool    Available() const { return Renderer != nullptr; }
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
        bool    MeshSelected() const;
        QString MeshName() const;
        QString MeshMaterial() const;
        int     MeshTris() const;
        int     MeshVerts() const;
        QString MeshVramText() const;
        QStringList MeshFlags() const;
        bool    MeshVisible() const;
        bool    Dirty() const { return VisDirty; }

        Q_INVOKABLE void toggleExpand(int row);
        Q_INVOKABLE void selectRow(int row);    // luz/mesh: seleciona no renderer (exclusivo)
        // Olho de mesh/pasta: flip de FRenderable::Visible + bump da TransformsVersion
        // (o rebuild leve da TLAS re-coleta as instancias pulando !Visible -> o GI/reflexos
        // respeitam o toggle no frame seguinte). Pasta = range inteiro do asset.
        Q_INVOKABLE void toggleEye(int row);
        // Duplo-clique: teleporta a camera pra enquadrar o objeto mantendo a orientacao
        // (mesh = AABB, luz = posicao + raio da fonte). Estilo tecla F da UE.
        Q_INVOKABLE void focusRow(int row);
        Q_INVOKABLE int  selectedRowIndex() const; // linha selecionada na lista flat (-1 = fora)
        // Setas cima/baixo: move a selecao pra proxima linha selecionavel (luz/mesh).
        Q_INVOKABLE void selectStep(int delta);
        // Salva o estado de visibilidade (meshes ocultas por asset + terreno) no
        // <cena>.visibility.json — chamado pelo botao salvar junto do saveLights.
        Q_INVOKABLE bool saveVisibility();
        Q_INVOKABLE void closePanel() { emit CloseRequested(); }

    public slots:
        void Rebuild();  // estrutura mudou (cena/luzes) — reconstroi a lista flat
        void Refresh();  // FrameReady: detecta selecao por picking e toggles externos

    signals:
        void AvailableChanged();
        void StructureChanged();
        void SelectionChanged();
        void FiltersChanged();
        void EnvChanged();
        void DirtyChanged();
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
        // Troca Rows preservando o scroll: mesma estrutura -> dataChanged; senao reset.
        void ApplyRows(QVector<FRow>&& NewRows);
        bool MatchesSearch(const QString& Name) const;
        void MarkDirty();
        bool LoadVisibility(); // le o JsonPath e aplica Visible/terreno nos assets presentes
        // Garante que a linha da selecao exista na lista flat (expande grupo/pasta e
        // reseta o chip de filtro se preciso). Retorna a linha, ou -1 se a busca a esconde.
        int  RevealSelection();

        Smile::Renderer* Renderer = nullptr;
        QVector<FRow>        Rows;
        QVector<FAssetRange> Assets;
        QString JsonPath;          // <cena>.visibility.json da cena principal (nao-aditiva)
        bool    VisDirty = false;
        QVector<bool>        AssetExpanded;      // paralelo a Assets
        bool    GroupExpanded[3] = { true, true, true };
        QString SearchText;
        int     FilterGroup = 0;                 // 0=Tudo, senao EGroup+1

        // Espelhos p/ o Refresh detectar mudanca externa (picking, settings, load).
        int  CachedSelMesh   = -1;
        int  CachedSelLight  = -1;
        int  CachedRenderableCount = -1;
        int  CachedLightCount      = -1;
        bool CachedClouds    = true;
        bool CachedOcean     = true;
        bool CachedTerrain   = false; // Terrain.IsLoaded()
        bool CachedTerrainOn = true;  // GetUseTerrain() (olho)
    };
}
