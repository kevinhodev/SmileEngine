#include "SmileEditor/MainWindow.h"
#include "SmileEditor/AboutDialog.h"
#include "SmileEditor/EnvironmentWindow.h"
#include "SmileEditor/LucideIcon.h"
#include "SmileEditor/SmileLogo.h"
#include "SmileEditor/ViewportWidget.h"
#include "SmileEditor/MaterialEditorPanel.h"
#include "SmileEditor/DarkTheme.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/D3D12Device.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTextEdit>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace SmileEditor {
    MainWindow::MainWindow(QWidget* _Parent)
        : QMainWindow(_Parent)
    {
        setWindowTitle(tr("Smile Engine"));
        setWindowIcon(QIcon(MakeSmileLogoPixmap(256)));
        resize(1536, 864);
        setMinimumSize(1120, 680);
        setWindowState(Qt::WindowMaximized);
        setObjectName("SmileMainWindow");

        setDockNestingEnabled(true);
        setDockOptions(dockOptions() | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

        CreateMenuBar();
        setCentralWidget(CreateViewportChrome());
        CreateDocks();

        Smile::SetLogSink([this](Smile::LogLevel level, std::string_view msg) {
            if (!LogOutput) return;
            const char* Color = level == Smile::LogLevel::Error   ? "#ff5f57"
                              : level == Smile::LogLevel::Warning ? "#f3b43f"
                                                                  : "#a7b5ff";
            const char* Tag   = level == Smile::LogLevel::Error   ? "[ERR]"
                              : level == Smile::LogLevel::Warning ? "[WARN]"
                                                                  : "[INFO]";

            LogOutput->append(QString("<span style='color:#777'>[%1]</span> "
                                      "<span style='color:%2'><b>%3</b></span> "
                                      "<span style='color:#b9b5aa'>%4</span>")
                .arg(QTime::currentTime().toString("HH:mm:ss"),
                     Color,
                     Tag,
                     QString::fromUtf8(msg.data(), static_cast<qsizetype>(msg.size()))));
        });

        statusBar()->setObjectName("SmileStatusBar");
        FooterStatsLabel = new QLabel(tr("FPS: --  |  Frame: -- ms"), this);
        FooterStatsLabel->setObjectName("FooterStatsLabel");
        statusBar()->addPermanentWidget(FooterStatsLabel);
        statusBar()->showMessage(tr("Pronto"));

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

#ifdef SMILE_SHADERS_SOURCE_DIR
        QString ShadersSourceDir = QStringLiteral(SMILE_SHADERS_SOURCE_DIR);
#else
        QString ShadersSourceDir = QDir(QCoreApplication::applicationDirPath()).filePath("../Shaders");
#endif
        if (QDir(ShadersSourceDir).exists()) {
            ShaderWatcher = new QFileSystemWatcher(this);
            QStringList ShadersToWatch = {
                QDir(ShadersSourceDir).filePath("Triangle.vs.hlsl"),
                QDir(ShadersSourceDir).filePath("Triangle.ps.hlsl")
            };
            ShaderWatcher->addPaths(ShadersToWatch);

            connect(ShaderWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& _Path) {
                TriggerShaderCompileAndReload(_Path);

                QTimer::singleShot(100, this, [this, _Path]() {
                    if (ShaderWatcher && !ShaderWatcher->files().contains(_Path)) {
                        ShaderWatcher->addPath(_Path);
                    }
                });
            });
            Smile::LogInfo("Shader Watcher Ativo. Monitorando Pasta: " + ShadersSourceDir.toStdString());
        } else {
            Smile::LogWarning("Diretorio de shaders de origem nao encontrado: " + ShadersSourceDir.toStdString());
        }
    }

    void MainWindow::CreateMenuBar() {
        auto* TopBar = new QWidget(this);
        TopBar->setObjectName("SmileTopBar");
        TopBar->setFixedHeight(36);

        auto* Layout = new QHBoxLayout(TopBar);
        Layout->setContentsMargins(8, 0, 10, 0);
        Layout->setSpacing(8);
        Layout->setAlignment(Qt::AlignVCenter);

        auto* Logo = new QLabel(TopBar);
        Logo->setObjectName("SmileTopLogo");
        Logo->setFixedSize(22, 22);
        Logo->setPixmap(MakeSmileLogoPixmap(22));
        Logo->setScaledContents(true);
        Layout->addWidget(Logo, 0, Qt::AlignVCenter);

        auto* Brand = new QLabel(tr("Smile Engine"), TopBar);
        Brand->setObjectName("SmileBrandLabel");
        Brand->setFixedHeight(24);
        Brand->setAlignment(Qt::AlignVCenter);
        Layout->addWidget(Brand, 0, Qt::AlignVCenter);

        auto* Menus = new QMenuBar(TopBar);
        Menus->setObjectName("SmileMenuBar");
        Menus->setNativeMenuBar(false);
        Menus->setFixedHeight(24);

        auto* FileMenu = Menus->addMenu(tr("Arquivo"));
        auto* ExitAction = FileMenu->addAction(tr("Sair"), this, &QWidget::close);
        ExitAction->setShortcut(QKeySequence::Quit);

        auto* EditMenu = Menus->addMenu(tr("Editar"));
        EditMenu->addAction(tr("Desfazer"))->setEnabled(false);
        EditMenu->addAction(tr("Refazer"))->setEnabled(false);

        WindowMenu = Menus->addMenu(tr("Janela"));
        auto* EnvironmentAction = WindowMenu->addAction(
            MakeLucideIcon(QStringLiteral("cloud-sun"), QColor(221, 216, 202), 18),
            tr("Ambiente & Céu"),
            this,
            &MainWindow::OnOpenEnvironmentWindow);
        EnvironmentAction->setShortcut(QKeySequence(tr("Ctrl+Shift+A")));
        WindowMenu->addSeparator();

        auto* ProjectMenu = Menus->addMenu(tr("Projeto"));
        auto* RenderMenu  = ProjectMenu->addMenu(tr("Renderização"));
        auto* MSAAMenu    = RenderMenu->addMenu(tr("MSAA"));

        MSAAGroup = new QActionGroup(this);
        MSAAGroup->setExclusive(true);

        struct { const char* Label; int Count; } MSAAOptions[] = {
            { "Desativado (1x)", 1 },
            { "2x",              2 },
            { "4x",              4 },
            { "8x",              8 },
        };
        for (auto& Option : MSAAOptions) {
            auto* Action = MSAAMenu->addAction(tr(Option.Label));
            Action->setCheckable(true);
            Action->setData(Option.Count);
            MSAAGroup->addAction(Action);
            if (Option.Count == 1) Action->setChecked(true);
        }
        connect(MSAAGroup, &QActionGroup::triggered, this, [this](QAction* Action) {
            OnMSAAChanged(Action->data().toInt());
        });

        // VSync: liga/desliga o trava-no-vblank do Present (default ligado).
        auto* VSyncAction = RenderMenu->addAction(tr("VSync"));
        VSyncAction->setCheckable(true);
        VSyncAction->setChecked(true);
        connect(VSyncAction, &QAction::toggled, this, [this](bool Enabled) {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                Viewport->GetRenderer()->SetVSync(Enabled);
        });

        auto* HelpMenu = Menus->addMenu(tr("Ajuda"));
        HelpMenu->addAction(tr("Sobre o Smile Engine..."), this, &MainWindow::OnHelpAbout);
        Layout->addWidget(Menus, 0, Qt::AlignVCenter);

        Layout->addStretch(1);

        setMenuWidget(TopBar);
    }

    QWidget* MainWindow::CreateViewportChrome() {
        auto* Shell = new QFrame(this);
        Shell->setObjectName("ViewportPanel");
        Shell->setFrameShape(QFrame::NoFrame);
        Shell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto* Layout = new QVBoxLayout(Shell);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(0);

        Viewport = new ViewportWidget(Shell);
        Viewport->setObjectName("MainViewport");
        Layout->addWidget(Viewport, 1);

        return Shell;
    }

    void MainWindow::CreateDocks() {
        auto RegisterDock = [this](QDockWidget* Dock) {
            if (!WindowMenu) return;
            QAction* ToggleAction = Dock->toggleViewAction();
            ToggleAction->setText(Dock->windowTitle());
            WindowMenu->addAction(ToggleAction);
        };

        auto* MaterialDock = new QDockWidget(tr("Recursos"), this);
        MaterialDock->setObjectName("ResourcesDock");
        MaterialDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
        MaterialDock->setFeatures(QDockWidget::DockWidgetMovable |
                                  QDockWidget::DockWidgetFloatable |
                                  QDockWidget::DockWidgetClosable);

        MaterialPanel = new MaterialEditorPanel(MaterialDock);
        MaterialDock->setWidget(MaterialPanel);
        addDockWidget(Qt::RightDockWidgetArea, MaterialDock);
        MaterialDock->setMinimumWidth(300);
        RegisterDock(MaterialDock);

        auto* ConsoleDock = new QDockWidget(tr("Console"), this);
        ConsoleDock->setObjectName("ConsoleDock");
        ConsoleDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        ConsoleDock->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable |
                                 QDockWidget::DockWidgetClosable);

        auto* Container = new QWidget(ConsoleDock);
        Container->setObjectName("ConsolePanel");
        auto* Layout = new QVBoxLayout(Container);
        Layout->setContentsMargins(8, 6, 8, 8);
        Layout->setSpacing(6);

        LogOutput = new QTextEdit(Container);
        LogOutput->setObjectName("LogOutput");
        LogOutput->setReadOnly(true);
        LogOutput->setFont(QFont("Consolas", 9));
        LogOutput->document()->setMaximumBlockCount(500);
        Layout->addWidget(LogOutput, 1);

        ConsoleDock->setWidget(Container);
        addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);
        ConsoleDock->resize(ConsoleDock->width(), 210);
        RegisterDock(ConsoleDock);

        resizeDocks({ MaterialDock }, { 320 }, Qt::Horizontal);
    }

    void MainWindow::OnRendererReady() {
        if (!Viewport || !Viewport->GetRenderer()) return;
        if (MaterialPanel) MaterialPanel->InitializeWithRenderer(Viewport->GetRenderer());
        LoadDefaultHDR();
        if (EnvironmentDlg) {
            EnvironmentDlg->InitializeWithRenderer(Viewport->GetRenderer());
            EnvironmentDlg->SetCurrentHDRPath(CurrentHDRPath);
        }
    }

    void MainWindow::OnOpenEnvironmentWindow() {
        if (!EnvironmentDlg) {
            EnvironmentDlg = new EnvironmentWindow(this);
            EnvironmentDlg->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(EnvironmentDlg, &EnvironmentWindow::HDRChanged, this, [this](const QString& Path) {
                CurrentHDRPath = Path;
            });
        }

        if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized()) {
            EnvironmentDlg->InitializeWithRenderer(Viewport->GetRenderer());
        }
        EnvironmentDlg->SetCurrentHDRPath(CurrentHDRPath);
        EnvironmentDlg->show();
        EnvironmentDlg->raise();
        EnvironmentDlg->activateWindow();
    }

    void MainWindow::LoadDefaultHDR() {
        if (DefaultHDRLoaded || !Viewport || !Viewport->GetRenderer() || !Viewport->GetRenderer()->IsInitialized()) {
            return;
        }
        DefaultHDRLoaded = true;

#ifdef SMILE_ASSETS_DIR
        QDir HdrDir(QString::fromUtf8(SMILE_ASSETS_DIR) + "/HDRi");
        const QStringList Hdrs = HdrDir.entryList(QStringList{ "*.hdr" }, QDir::Files, QDir::Name);
        if (!Hdrs.isEmpty()) {
            const QString File = HdrDir.filePath(Hdrs.first());
            if (Viewport->GetRenderer()->LoadHDREnvironment(File.toStdWString())) {
                CurrentHDRPath = File;
                if (EnvironmentDlg) {
                    EnvironmentDlg->SetCurrentHDRPath(CurrentHDRPath);
                }
            } else {
                Smile::LogError("Falha ao auto-carregar HDR: " + File.toStdString());
            }
        }
#endif
    }

    void MainWindow::OnMSAAChanged(int _SampleCount) {
        if (Viewport && Viewport->GetRenderer()) {
            Viewport->GetRenderer()->SetMSAA(static_cast<Smile::u32>(_SampleCount));
        }
    }

    void MainWindow::UpdateStats() {
        if (!Viewport) return;
        auto* Renderer = Viewport->GetRenderer();
        if (!Renderer || !Renderer->IsInitialized()) return;

        float FPS = Viewport->GetFPS();
        const float FrameMs = FPS > 0.0f ? 1000.0f / FPS : 0.0f;

        if (FooterStatsLabel) {
            FooterStatsLabel->setText(QString("FPS: %1  |  Frame: %2 ms")
                .arg(FPS, 0, 'f', 1)
                .arg(FrameMs, 0, 'f', 2));
        }
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

    void MainWindow::TriggerShaderCompileAndReload(const QString& _Path) {
        Smile::LogInfo("Alteracao detectada no shader: " + QFileInfo(_Path).fileName().toStdString());

#ifdef SMILE_CMAKE_BINARY_DIR
        QString BuildDir = QStringLiteral(SMILE_CMAKE_BINARY_DIR);
#else
        QString BuildDir = QDir(QCoreApplication::applicationDirPath()).filePath("..");
#endif

        QProcess* CompileProcess = new QProcess(this);
        QStringList Arguments = { "--build", BuildDir, "--target", "Shaders" };

        Smile::LogInfo("Compilando shaders via CMake...");
        CompileProcess->start("cmake", Arguments);

        connect(CompileProcess, &QProcess::finished, this, [this, CompileProcess](int ExitCode, QProcess::ExitStatus Status) {
            Q_UNUSED(Status);
            if (ExitCode == 0) {
                if (Viewport && Viewport->GetRenderer()) {
                    if (Viewport->GetRenderer()->ReloadShaders()) {
                        statusBar()->showMessage(tr("Shaders recarregados com sucesso."), 3000);
                    } else {
                        statusBar()->showMessage(tr("Erro ao recarregar shaders no renderer."), 3000);
                    }
                }
            } else {
                QString Errors = QString::fromUtf8(CompileProcess->readAllStandardError());
                if (Errors.isEmpty()) {
                    Errors = QString::fromUtf8(CompileProcess->readAllStandardOutput());
                }
                Smile::LogError("Falha ao compilar shaders via CMake:\n" + Errors.toStdString());
                statusBar()->showMessage(tr("Falha na compilação do shader."), 3000);
            }
            CompileProcess->deleteLater();
        });
    }
}
