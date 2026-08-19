#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <functional>

class QActionGroup;
class QLabel;
class QMenu;
class QFileSystemWatcher;
class QWidget;
class QEvent;
class QDockWidget;
class QDialog;
class QQmlEngine;

namespace SmileEditor {
    class ViewportWidget;
    class LogBridge;
    class WindowBridge;
    class NativeWindowFilter;
    class MenuBridge;
    class StatusBridge;
    class TimeOfDayBridge;
    class LightsBridge;
    class SceneOutlinerBridge;
    class SceneDocument;
    class CameraBookmarksBridge;
    class CaptureBridge;
    class McpBridge;
    class RenderSettingsController;
    class MaterialsBridge;
    class RenderSettingsBridge;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(QQmlEngine& qmlEngine, QWidget* parent = nullptr);
        ~MainWindow() override;

        // Cena .sscene passada na linha de comando (dev/smoke): carregada assim que o
        // renderer inicializar (OnRendererReady), sem passar pelo dialogo de arquivo.
        void SetStartupScene(const QString& Path) { StartupScenePath = Path; }

    signals:
        // Boot: repassa as etapas do Renderer::Initialize para a splash e avisa quando o editor
        // fica utilizavel (renderer pronto, cena de boot carregada — ou falha, para a splash
        // nunca sobreviver a um editor que nao vai renderizar).
        void BootProgress(const QString& label, const QString& detail, qreal fraction);
        void BootFinished();

    protected:
        void changeEvent(QEvent* event) override; // notifica o WindowBridge em max/restore
        bool eventFilter(QObject* obj, QEvent* event) override; // visibilidade da janela TOD -> menu
        void closeEvent(QCloseEvent* event) override; // prompt de sidecars nao salvos

    private slots:
        void OnRendererReady();
        void UpdateStats();
        void TriggerShaderCompileAndReload(const QString& Path);
        void ShowSettings();
        void ShowTimeOfDay();
        void ShowStats();
        void ShowDebugTargets();
        void ShowMaterials();

    private:
        void CreateTopBar();      // barra unificada QML (MainBar.qml + EditorMenuBar.qml)
        void WireMenuActions();   // conecta os sinais do MenuBridge a logica + atalhos globais
        void CreateStatusBar();   // barra de status QML (StatusBar.qml) no slot do QStatusBar
        void CreateDocks();
        QWidget* CreateViewportChrome();
        void RegisterViewport(ViewportWidget* viewport, QWidget* toolbar);
        // OnDone != nullptr => carga NAO interativa: os QMessageBox de falha ficam de fora e o
        // resultado volta pelo callback. E o que separa "o usuario escolheu um arquivo no menu"
        // de "o SmileMCP pediu a carga": um dialogo modal num ciclo automatizado trava o editor
        // esperando um clique que nao vem.
        using SceneLoadCallback = std::function<void(bool ok, const QString& error)>;
        void BeginSceneLoad(const QString& path, bool additive, SceneLoadCallback onDone = {});
        void FinishBootStage();   // idempotente: emite BootFinished uma unica vez
        void ContinueApprovedClose(QCloseEvent* event);

        QString               StartupScenePath;
        QQmlEngine*           SharedQmlEngine = nullptr; // lifetime pertence ao RunEditor
        ViewportWidget*       Viewport    = nullptr;
        QPointer<ViewportWidget> ActiveViewport;
        QPointer<QDialog>     SettingsDlg;

        StatusBridge*         StatusBr    = nullptr; // ponte C++ -> StatusBar.qml
        LogBridge*            ConsoleLog  = nullptr; // ponte C++ -> ConsolePanel.qml
        WindowBridge*         WindowBr    = nullptr; // ponte C++ -> MainBar.qml (botoes de janela)
        NativeWindowFilter*   WinFilter   = nullptr; // frameless nativo (Slate-style)
        MenuBridge*           Menus       = nullptr; // ponte C++ -> EditorMenuBar.qml
        QDockWidget*          ConsoleDock = nullptr; // p/ toggle/estado no menu Janela
        TimeOfDayBridge*      TodBridge   = nullptr; // ponte C++ -> TimeOfDayWindow.qml
        QPointer<QDialog>     TodDlg;                // janela flutuante do Time of Day
        QPointer<QDialog>     StatsDlg;              // janela flutuante de Estatisticas (VRAM)
        QPointer<QDialog>     DebugTargetsDlg;       // janela flutuante do visualizador de RTs
        LightsBridge*         LightsBr    = nullptr; // acoes/propriedades de luz (outliner)
        SceneOutlinerBridge*  OutlinerBr  = nullptr; // ponte C++ -> SceneOutlinerPanel.qml
        SceneDocument*        SceneDoc    = nullptr; // camada autorada da cena (.smap)
        QDockWidget*          LightsDock  = nullptr; // dock lateral do Scene Outliner ("Cena")
        MaterialsBridge*      MaterialsBr = nullptr; // ponte C++ -> MaterialsWindow.qml
        // Bookmarks de camera (<cena>.cameras.json). Nao tem janela propria: mora na pagina de
        // Renderizacao das Configuracoes, junto dos knobs que a captura precisa fixar.
        CameraBookmarksBridge* CameraBookmarksBr = nullptr;
        // Captura deterministica. Vizinha dos bookmarks pela mesma razao: o protocolo e pose fixa
        // + knobs fixos, e as duas metades tem de estar a um clique uma da outra.
        CaptureBridge*        CaptureBr  = nullptr;
        RenderSettingsController* RenderSettingsCtrl = nullptr;
        McpBridge*            McpBr      = nullptr; // named pipe local usada pelo SmileMCP
        RenderSettingsBridge* RenderBr   = nullptr; // ponte C++ -> knobs do SettingsWindow.qml
        QPointer<QDialog>     MaterialsDlg;          // janela flutuante do Editor de Materiais

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
        bool                  BootSplashActive = true;  // splash ainda esperando o BootFinished
        bool                  SceneLoadInProgress = false;
        bool                  CloseApproved = false;
        bool                  RendererShutdownForClose = false;
    };
}
