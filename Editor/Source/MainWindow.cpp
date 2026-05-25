#include "SmileEditor/MainWindow.h"
#include "SmileEditor/AboutDialog.h"
#include "SmileEditor/ViewportWidget.h"
#include "SmileEditor/MaterialEditorPanel.h"

#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/D3D12Device.h"

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QVBoxLayout>
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
        if (!LogOutput) return;
        const char* color = level == Smile::LogLevel::Error   ? "#e05252"
                          : level == Smile::LogLevel::Warning ? "#e0a840"
                                                              : "#c8c8c8";
        const char* tag   = level == Smile::LogLevel::Error   ? "[ERR]"
                          : level == Smile::LogLevel::Warning ? "[WARN]"
                                                              : "[INFO]";
        LogOutput->append(QString("<span style='color:%1'><b>%2</b> %3</span>")
            .arg(color, tag, QString::fromUtf8(msg.data(), static_cast<qsizetype>(msg.size()))));
    });

    connect(Viewport, &ViewportWidget::FrameReady,          this, &MainWindow::UpdateStats);
    connect(Viewport, &ViewportWidget::RendererInitialized, this, &MainWindow::OnRendererReady);

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
    // --- Right dock: Material Editor ---
    auto* matDock = new QDockWidget(tr("Material"), this);
    matDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    matDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    MatPanel = new MaterialEditorPanel(matDock);
    matDock->setWidget(MatPanel);
    addDockWidget(Qt::RightDockWidgetArea, matDock);
    matDock->setMinimumWidth(240);

    // --- Bottom dock: Console ---
    auto* consoleDock = new QDockWidget(tr("Console"), this);
    consoleDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    auto* container = new QWidget(consoleDock);
    auto* layout    = new QVBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    StatsLabel = new QLabel(tr("Aguardando renderer..."), container);
    StatsLabel->setFont(QFont("Consolas", 9));
    StatsLabel->setStyleSheet("color: #a0c8ff;");
    layout->addWidget(StatsLabel);

    LogOutput = new QTextEdit(container);
    LogOutput->setReadOnly(true);
    LogOutput->setFont(QFont("Consolas", 9));
    LogOutput->setStyleSheet("background: #1a1a1f; color: #c8c8c8; border: none;");
    LogOutput->document()->setMaximumBlockCount(500);
    layout->addWidget(LogOutput);

    consoleDock->setWidget(container);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    consoleDock->resize(consoleDock->width(), 180);
}

void MainWindow::OnRendererReady() {
    if (MatPanel && Viewport && Viewport->GetRenderer())
        MatPanel->InitializeWithRenderer(Viewport->GetRenderer());
}

void MainWindow::OnMSAAChanged(int sampleCount) {
    if (Viewport && Viewport->GetRenderer()) {
        Viewport->GetRenderer()->SetMSAA(static_cast<Smile::u32>(sampleCount));
    }
}

void MainWindow::UpdateStats() {
    if (!StatsLabel || !Viewport) return;
    auto* r = Viewport->GetRenderer();
    if (!r || !r->IsInitialized()) return;

    Smile::Vec3 cam = r->GetCameraPos();
    float pitch     = r->GetPitch();
    float yaw       = r->GetYaw();
    Smile::u32 msaa = r->GetMSAA();
    float fps       = Viewport->GetFPS();

    StatsLabel->setText(QString(
        "FPS: %1  |  Frame: %2 ms  |  MSAA: %3x\n"
        "Câmera: (%4, %5, %6)  |  Pitch: %7°  Yaw: %8°")
        .arg(fps,   0, 'f', 1)
        .arg(fps > 0.0f ? 1000.0f / fps : 0.0f, 0, 'f', 2)
        .arg(msaa)
        .arg(cam.X, 0, 'f', 2)
        .arg(cam.Y, 0, 'f', 2)
        .arg(cam.Z, 0, 'f', 2)
        .arg(pitch, 0, 'f', 1)
        .arg(yaw,   0, 'f', 1));
}

void MainWindow::OnHelpAbout() {
    if (!AboutDlg) {
        QString gpu;
        if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized()) {
            gpu = QString::fromStdWString(Viewport->GetRenderer()->GetDevice().GetAdapterDescription());
        }
        AboutDlg = new AboutDialog(gpu, this);
        AboutDlg->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    AboutDlg->show();
    AboutDlg->raise();
    AboutDlg->activateWindow();
}

} // namespace SmileEditor
