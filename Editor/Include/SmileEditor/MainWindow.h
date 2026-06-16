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
        void OnRendererReady();
        void UpdateStats();
        void TriggerShaderCompileAndReload(const QString& Path);

    private:
        void CreateMenuBar();
        void CreateDocks();
        QWidget* CreateViewportChrome();

        ViewportWidget*       Viewport    = nullptr;
        QPointer<AboutDialog> AboutDlg;
        QPointer<EnvironmentWindow> EnvironmentDlg;
        QMenu*                WindowMenu  = nullptr;

        QLabel*               FooterStatsLabel = nullptr;
        QTextEdit*            LogOutput   = nullptr;

        MaterialEditorPanel*  MaterialPanel    = nullptr;
        QString               CurrentHDRPath;

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
    };
} 
