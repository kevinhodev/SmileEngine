#pragma once

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
class QQmlEngine;

namespace SmileEditor {
    class ViewportWidget;
    class LogBridge;
    class WindowBridge;
    class NativeWindowFilter;
    class MenuBridge;
    class StatusBridge;
    class TimeOfDayBridge;
    class CloudsBridge;
    class WeatherBridge;
    class OceanBridge;
    class LightsBridge;
    class SceneOutlinerBridge;
    class SceneDocument;
    class CameraBookmarksBridge;
    class CaptureBridge;
    class McpBridge;
    class RenderSettingsController;
    class MaterialsBridge;
    class RenderSettingsBridge;
    class StatsBridge;
    class DebugTargetsBridge;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(QQmlEngine& qmlEngine, QWidget* parent = nullptr);
        ~MainWindow() override;

        // Cena de linha de comando carregada quando o renderer fica disponível.
        void SetStartupScene(const QString& Path) { StartupScenePath = Path; }

    signals:
        void BootProgress(const QString& label, const QString& detail, qreal fraction);
        void BootFinished();

    protected:
        void changeEvent(QEvent* event) override;
        bool eventFilter(QObject* obj, QEvent* event) override;
        void closeEvent(QCloseEvent* event) override;

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
        void CreateTopBar();
        void WireMenuActions();
        void CreateStatusBar();
        void CreateDocks();
        QWidget* CreateViewportChrome();
        void RegisterViewport(ViewportWidget* viewport, QWidget* toolbar);
        void BeginSceneLoad(const QString& path, bool additive);
        void FinishBootStage();
        void ContinueApprovedClose(QCloseEvent* event);

        QString               StartupScenePath;
        QQmlEngine*           SharedQmlEngine = nullptr;
        ViewportWidget*       Viewport    = nullptr;
        QPointer<ViewportWidget> ActiveViewport;
        QPointer<QDialog>     SettingsDlg;

        StatusBridge*         StatusBr    = nullptr;
        LogBridge*            ConsoleLog  = nullptr;
        WindowBridge*         WindowBr    = nullptr;
        NativeWindowFilter*   WinFilter   = nullptr;
        MenuBridge*           Menus       = nullptr;
        QDockWidget*          ConsoleDock = nullptr;
        TimeOfDayBridge*      TodBridge   = nullptr;
        CloudsBridge*         CloudsBr    = nullptr;
        WeatherBridge*        WeatherBr   = nullptr;
        OceanBridge*          OceanBr     = nullptr;
        QPointer<QDialog>     TodDlg;
        QPointer<QDialog>     StatsDlg;
        QPointer<QDialog>     DebugTargetsDlg;
        LightsBridge*         LightsBr    = nullptr;
        SceneOutlinerBridge*  OutlinerBr  = nullptr;
        SceneDocument*        SceneDoc    = nullptr;
        QDockWidget*          LightsDock  = nullptr;
        MaterialsBridge*      MaterialsBr = nullptr;
        CameraBookmarksBridge* CameraBookmarksBr = nullptr;
        CaptureBridge*        CaptureBr  = nullptr;
        RenderSettingsController* RenderSettingsCtrl = nullptr;
        McpBridge*            McpBr      = nullptr;
        RenderSettingsBridge* RenderBr   = nullptr;
        StatsBridge*          StatsBr    = nullptr;
        DebugTargetsBridge*   DebugTargetsBr = nullptr;
        QPointer<QDialog>     MaterialsDlg;

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
        bool                  BootSplashActive = true;
        bool                  SceneLoadInProgress = false;
        bool                  CloseApproved = false;
        bool                  RendererShutdownForClose = false;
    };
}
