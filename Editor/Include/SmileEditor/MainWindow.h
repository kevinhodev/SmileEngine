#pragma once

// Janela principal do editor: viewport central + docks placeholder + menu bar.

#include <QMainWindow>
#include <QPointer>

class QActionGroup;
class QLabel;
class QTextEdit;

namespace SmileEditor {

class ViewportWidget;
class AboutDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void OnHelpAbout();
    void OnMSAAChanged(int sampleCount);
    void UpdateStats();

private:
    void CreateMenuBar();
    void CreateDocks();

    ViewportWidget*       Viewport    = nullptr;
    QPointer<AboutDialog> AboutDlg;
    QActionGroup*         MSAAGroup   = nullptr;

    QLabel*               StatsLabel  = nullptr;
    QTextEdit*            LogOutput   = nullptr;
};

} // namespace SmileEditor
