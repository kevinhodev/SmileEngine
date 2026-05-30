#pragma once

#include <QMainWindow>
#include <QPointer>

class QActionGroup;
class QLabel;
class QTextEdit;
class QFileSystemWatcher;

namespace SmileEditor {
    class ViewportWidget;
    class AboutDialog;
    class MaterialEditorPanel;
    class EnvironmentPanel;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(QWidget* parent = nullptr);
        ~MainWindow() override = default;

    private slots:
        void OnHelpAbout();
        void OnMSAAChanged(int sampleCount);
        void OnRendererReady();
        void UpdateStats();
        void TriggerShaderCompileAndReload(const QString& Path);

    private:
        void CreateMenuBar();
        void CreateDocks();

        ViewportWidget*       Viewport    = nullptr;
        QPointer<AboutDialog> AboutDlg;
        QActionGroup*         MSAAGroup   = nullptr;

        QLabel*               StatsLabel  = nullptr;
        QTextEdit*            LogOutput   = nullptr;

        MaterialEditorPanel*  MaterialPanel    = nullptr;
        EnvironmentPanel*     EnvPanel         = nullptr;

        QFileSystemWatcher*   StylesheetWatcher = nullptr;
        QFileSystemWatcher*   ShaderWatcher     = nullptr;
    };
} 
