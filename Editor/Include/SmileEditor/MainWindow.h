#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>

class QActionGroup;
class QLabel;
class QMenu;
class QTextEdit;
class QFileSystemWatcher;
class QWidget;

namespace SmileEditor {
    class ViewportWidget;
    class AboutDialog;
    class MaterialEditorPanel;
    class EnvironmentWindow;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(QWidget* parent = nullptr);
        ~MainWindow() override = default;

    private slots:
        void OnHelpAbout();
        void OnOpenEnvironmentWindow();
        void OnMSAAChanged(int sampleCount);
        void OnRendererReady();
        void UpdateStats();
        void TriggerShaderCompileAndReload(const QString& Path);

    private:
        void CreateMenuBar();
        void CreateDocks();
        QWidget* CreateViewportChrome();
        void LoadDefaultHDR();

        ViewportWidget*       Viewport    = nullptr;
        QPointer<AboutDialog> AboutDlg;
        QPointer<EnvironmentWindow> EnvironmentDlg;
        QActionGroup*         MSAAGroup   = nullptr;
        QMenu*                WindowMenu  = nullptr;

        QLabel*               FooterStatsLabel = nullptr;
        QTextEdit*            LogOutput   = nullptr;

        MaterialEditorPanel*  MaterialPanel    = nullptr;
        QString               CurrentHDRPath;
        bool                  DefaultHDRLoaded = false;

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
    };
} 
