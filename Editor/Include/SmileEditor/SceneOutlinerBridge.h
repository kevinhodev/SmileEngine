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
        Q_PROPERTY(bool meshSelected READ MeshSelected NOTIFY SelectionChanged)
        Q_PROPERTY(QString meshName READ MeshName NOTIFY SelectionChanged)

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
        bool    MeshSelected() const;
        QString MeshName() const;

        Q_INVOKABLE void toggleExpand(int row);
        Q_INVOKABLE void selectRow(int row);    // luz/mesh: seleciona no renderer (exclusivo)
        Q_INVOKABLE int  selectedRowIndex() const; // linha selecionada na lista flat (-1 = fora)
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

        Smile::Renderer* Renderer = nullptr;
        QVector<FRow>        Rows;
        QVector<FAssetRange> Assets;
        QVector<bool>        AssetExpanded;      // paralelo a Assets
        bool    GroupExpanded[3] = { true, true, true };
        QString SearchText;
        int     FilterGroup = 0;                 // 0=Tudo, senao EGroup+1

        // Espelhos p/ o Refresh detectar mudanca externa (picking, settings, load).
        int  CachedSelMesh   = -1;
        int  CachedSelLight  = -1;
        int  CachedRenderableCount = -1;
        int  CachedLightCount      = -1;
        bool CachedClouds  = true;
        bool CachedOcean   = true;
        bool CachedTerrain = false;
    };
}
