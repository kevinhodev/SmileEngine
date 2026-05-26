#include "SmileEditor/MainWindow.h"
#include "SmileEditor/AboutDialog.h"
#include "SmileEditor/ViewportWidget.h"
#include "SmileEditor/MaterialEditorPanel.h"
#include "SmileEditor/DarkTheme.h"
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
#include <QFileSystemWatcher>
#include <QTimer>
#include <QApplication>


namespace SmileEditor {
    MainWindow::MainWindow(QWidget* _Parent)
        : QMainWindow(_Parent)
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

        StylesheetWatcher = new QFileSystemWatcher(this);
        const QStringList QSSFiles = GetStylesheetFiles();
        StylesheetWatcher->addPaths(QSSFiles);

        connect(StylesheetWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& _Path) {
            LoadAndApplyStylesheets(*static_cast<QApplication*>(QCoreApplication::instance()));

            QTimer::singleShot(100, this, [this, _Path]() {
                if (StylesheetWatcher && !StylesheetWatcher->files().contains(_Path)) {
                    StylesheetWatcher->addPath(_Path);
                }
            });
        });

        statusBar()->showMessage(tr("Pronto"));
    }

    void MainWindow::CreateMenuBar() {
        auto* FileMenu   = menuBar()->addMenu(tr("&File"));
        auto* ExitAction = FileMenu->addAction(tr("E&xit"), this, &QWidget::close);
        ExitAction->setShortcut(QKeySequence::Quit);

        auto* RenderMenu = menuBar()->addMenu(tr("&Renderização"));
        auto* MSAAMenu   = RenderMenu->addMenu(tr("&MSAA"));

        MSAAGroup = new QActionGroup(this);
        MSAAGroup->setExclusive(true);

        struct { const char* Label; int Count; } MSAAOptions[] = {
            { "&Desativado (1x)", 1 },
            { "&2x",              2 },
            { "&4x",              4 },
            { "&8x",              8 },
        };
        for (auto& Option : MSAAOptions) {
            auto* Action = MSAAMenu->addAction(tr(Option.Label));
            Action->setCheckable(true);
            Action->setData(Option.Count);
            MSAAGroup->addAction(Action);
            if (Option.Count == 1) Action->setChecked(true);
        }
        connect(MSAAGroup, &QActionGroup::triggered, this, [this](QAction* a) {
            OnMSAAChanged(a->data().toInt());
        });

        auto* HelpMenu = menuBar()->addMenu(tr("&Help"));
        HelpMenu->addAction(tr("&About SmileEngine..."), this, &MainWindow::OnHelpAbout);
    }

    void MainWindow::CreateDocks() {
        auto* MaterialDock = new QDockWidget(tr("Material"), this);
        MaterialDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
        MaterialDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

        MaterialPanel = new MaterialEditorPanel(MaterialDock);
        MaterialDock->setWidget(MaterialPanel);
        addDockWidget(Qt::RightDockWidgetArea, MaterialDock);
        MaterialDock->setMinimumWidth(240);

        auto* ConsoleDock = new QDockWidget(tr("Console"), this);
        ConsoleDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

        auto* Container = new QWidget(ConsoleDock);
        auto* Layout    = new QVBoxLayout(Container);
        Layout->setContentsMargins(4, 4, 4, 4);
        Layout->setSpacing(4);

        StatsLabel = new QLabel(tr("Aguardando renderer..."), Container);
        StatsLabel->setObjectName("StatsLabel");
        StatsLabel->setFont(QFont("Consolas", 9));
        Layout->addWidget(StatsLabel);

        LogOutput = new QTextEdit(Container);
        LogOutput->setObjectName("LogOutput");
        LogOutput->setReadOnly(true);
        LogOutput->setFont(QFont("Consolas", 9));
        LogOutput->document()->setMaximumBlockCount(500);
        Layout->addWidget(LogOutput);

        ConsoleDock->setWidget(Container);
        addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);
        ConsoleDock->resize(ConsoleDock->width(), 180);
    }

    void MainWindow::OnRendererReady() {
        if (MaterialPanel && Viewport && Viewport->GetRenderer())
            MaterialPanel->InitializeWithRenderer(Viewport->GetRenderer());
    }

    void MainWindow::OnMSAAChanged(int _SampleCount) {
        if (Viewport && Viewport->GetRenderer()) {
            Viewport->GetRenderer()->SetMSAA(static_cast<Smile::u32>(_SampleCount));
        }
    }

    void MainWindow::UpdateStats() {
        if (!StatsLabel || !Viewport) return;
        auto* Renderer = Viewport->GetRenderer();
        if (!Renderer || !Renderer->IsInitialized()) return;

        Smile::Vec3 Camera = Renderer->GetCameraPos();
        float Pitch        = Renderer->GetPitch();
        float Yaw          = Renderer->GetYaw();
        Smile::u32 MSAA    = Renderer->GetMSAA();
        float FPS          = Viewport->GetFPS();

        StatsLabel->setText(QString(
            "FPS: %1  |  Frame: %2 ms  |  MSAA: %3x\n"
            "Câmera: (%4, %5, %6)  |  Pitch: %7°  Yaw: %8°")
            .arg(FPS,   0, 'f', 1)
            .arg(FPS > 0.0f ? 1000.0f / FPS : 0.0f, 0, 'f', 2)
            .arg(MSAA)
            .arg(Camera.X, 0, 'f', 2)
            .arg(Camera.Y, 0, 'f', 2)
            .arg(Camera.Z, 0, 'f', 2)
            .arg(Pitch, 0, 'f', 1)
            .arg(Yaw,   0, 'f', 1));
    }

    void MainWindow::OnHelpAbout() {
        if (!AboutDlg) {
            QString GPU;
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized()) {
                GPU = QString::fromStdWString(Viewport->GetRenderer()->GetDevice().GetAdapterDescription());
            }
            AboutDlg = new AboutDialog(GPU, this);
            AboutDlg->setAttribute(Qt::WA_DeleteOnClose, false);
        }
        AboutDlg->show();
        AboutDlg->raise();
        AboutDlg->activateWindow();
    }
} 
