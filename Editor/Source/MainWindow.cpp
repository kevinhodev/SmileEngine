#include "SmileEditor/MainWindow.h"
#include "SmileEditor/AboutDialog.h"
#include "SmileEditor/ViewportWidget.h"
#include "SmileEditor/ConsoleWidget.h"

#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/D3D12Device.h"

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QWidget>

namespace SmileEditor {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("SmileEditor"));
    resize(1280, 720);

    setDockNestingEnabled(true);
    setDockOptions(dockOptions() | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

    CreateMenuBar();

    Viewport = new ViewportWidget(this);
    setCentralWidget(Viewport);

    CreateDocks();

    Smile::SetLogSink([this](Smile::LogLevel level, std::string_view msg) {
        if (!Console) return;
        Console->AppendLine(level,
            QString::fromUtf8(msg.data(), static_cast<qsizetype>(msg.size())));
    });

    connect(Viewport, &ViewportWidget::FrameReady, this, &MainWindow::UpdateStats);

    statusBar()->showMessage(tr("Pronto"));
}

void MainWindow::CreateMenuBar() {
    auto* fileMenu   = menuBar()->addMenu(tr("&File"));
    auto* exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    auto* renderMenu = menuBar()->addMenu(tr("&Renderização"));
    auto* msaaMenu   = renderMenu->addMenu(tr("&MSAA"));

    MSAAGroup = new QActionGroup(this);
    MSAAGroup->setExclusive(true);

    struct { const char* label; int count; } msaaOptions[] = {
        { "&Desativado (1x)", 1 },
        { "&2x",              2 },
        { "&4x",              4 },
        { "&8x",              8 },
    };
    for (auto& opt : msaaOptions) {
        auto* action = msaaMenu->addAction(tr(opt.label));
        action->setCheckable(true);
        action->setData(opt.count);
        MSAAGroup->addAction(action);
        if (opt.count == 1) action->setChecked(true);
    }
    connect(MSAAGroup, &QActionGroup::triggered, this, [this](QAction* a) {
        OnMSAAChanged(a->data().toInt());
    });

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About SmileEngine..."), this, &MainWindow::OnHelpAbout);
}

void MainWindow::CreateDocks() {
    auto* consoleDock = new QDockWidget(tr("Console"), this);
    consoleDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    Console = new ConsoleWidget(consoleDock);
    consoleDock->setWidget(Console);

    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    consoleDock->resize(consoleDock->width(), 180);
}

void MainWindow::OnMSAAChanged(int sampleCount) {
    if (Viewport && Viewport->GetRenderer())
        Viewport->GetRenderer()->SetMSAA(static_cast<Smile::u32>(sampleCount));
}

void MainWindow::UpdateStats() {
    if (!Console || !Viewport) return;
    auto* r = Viewport->GetRenderer();
    if (!r || !r->IsInitialized()) return;

    Smile::Vec3    cam   = r->GetCameraPos();
    Smile::u32     msaa  = r->GetMSAA();
    float          fps   = Viewport->GetFPS();

    Console->SetStats(QString(
        "FPS: %1  |  Frame: %2 ms  |  MSAA: %3x\n"
        "Câmera: (%4, %5, %6)  |  Pitch: %7°  Yaw: %8°")
        .arg(fps,  0, 'f', 1)
        .arg(fps > 0.0f ? 1000.0f / fps : 0.0f, 0, 'f', 2)
        .arg(msaa)
        .arg(cam.X, 0, 'f', 2)
        .arg(cam.Y, 0, 'f', 2)
        .arg(cam.Z, 0, 'f', 2)
        .arg(r->GetPitch(), 0, 'f', 1)
        .arg(r->GetYaw(),   0, 'f', 1));
}

void MainWindow::OnHelpAbout() {
    if (!AboutDlg) {
        QString gpu;
        if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
            gpu = QString::fromStdWString(Viewport->GetRenderer()->GetDevice().GetAdapterDescription());
        AboutDlg = new AboutDialog(gpu, this);
        AboutDlg->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    AboutDlg->show();
    AboutDlg->raise();
    AboutDlg->activateWindow();
}

} // namespace SmileEditor
