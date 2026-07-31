#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QString>

class QActionGroup;
class QLabel;
class QMenu;
class QFileSystemWatcher;
class QWidget;
class QEvent;
class QDockWidget;
class QDialog;

namespace SmileEditor {
    class ViewportWidget;
    class AboutDialog;
    class LogBridge;
    class WindowBridge;
    class NativeWindowFilter;
    class MenuBridge;
    class StatusBridge;
    class TimeOfDayBridge;
    class LightsBridge;
    class SceneOutlinerBridge;
    class MaterialsBridge;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(QWidget* parent = nullptr);
        ~MainWindow() override;

        // Cena .sscene passada na linha de comando (dev/smoke): carregada assim que o
        // renderer inicializar (OnRendererReady), sem passar pelo dialogo de arquivo.
        void SetStartupScene(const QString& Path) { StartupScenePath = Path; }

    protected:
        void changeEvent(QEvent* event) override; // notifica o WindowBridge em max/restore
        bool eventFilter(QObject* obj, QEvent* event) override; // visibilidade da janela TOD -> menu
        void closeEvent(QCloseEvent* event) override; // prompt de sidecars nao salvos

    private slots:
        void OnHelpAbout();
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

        QString               StartupScenePath;
        ViewportWidget*       Viewport    = nullptr;
        QPointer<ViewportWidget> ActiveViewport;
        QPointer<AboutDialog> AboutDlg;
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
        QDockWidget*          LightsDock  = nullptr; // dock lateral do Scene Outliner ("Cena")
        MaterialsBridge*      MaterialsBr = nullptr; // ponte C++ -> MaterialsWindow.qml
        QPointer<QDialog>     MaterialsDlg;          // janela flutuante do Editor de Materiais

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
        QElapsedTimer         StatsThrottle;    // status bar atualiza a ~5Hz, nao por frame
    };
} 
