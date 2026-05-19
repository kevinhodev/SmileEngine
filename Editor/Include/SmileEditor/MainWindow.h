#pragma once

#include <QMainWindow>
#include <QPointer>

class QActionGroup;

namespace SmileEditor {

class ViewportWidget;
class AboutDialog;
class ConsoleWidget;

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

    ViewportWidget*       Viewport  = nullptr;
    QPointer<AboutDialog> AboutDlg;
    QActionGroup*         MSAAGroup = nullptr;

    ConsoleWidget*        Console   = nullptr;
};

} // namespace SmileEditor
