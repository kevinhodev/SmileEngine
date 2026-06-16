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
#include <QDirIterator>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
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

            const QString Html = QString("<span style='color:#777'>[%1]</span> "
                                         "<span style='color:%2'><b>%3</b></span> "
                                         "<span style='color:#b9b5aa'>%4</span>")
                .arg(QTime::currentTime().toString("HH:mm:ss"),
                     Color,
                     Tag,
                     QString::fromUtf8(msg.data(), static_cast<qsizetype>(msg.size())));

            QMetaObject::invokeMethod(LogOutput, [this, Html]() {
                if (LogOutput) LogOutput->append(Html);
            }, Qt::QueuedConnection);
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

            auto CollectShaders = [](const QString& _Root) {
                QStringList Files;
                QDirIterator Iterator(_Root, QStringList{ "*.hlsl", "*.hlsli" },
                                QDir::Files, QDirIterator::Subdirectories);
                while (Iterator.hasNext()) Files << Iterator.next();
                return Files;
            };
            const QStringList ShaderFiles = CollectShaders(ShadersSourceDir);
            if (!ShaderFiles.isEmpty()) ShaderWatcher->addPaths(ShaderFiles);

            QStringList ShaderDirs{ ShadersSourceDir };
            {
                QDirIterator D(ShadersSourceDir, QDir::Dirs | QDir::NoDotAndDotDot,
                               QDirIterator::Subdirectories);
                while (D.hasNext()) ShaderDirs << D.next();
            }
            ShaderWatcher->addPaths(ShaderDirs);

            connect(ShaderWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& _Path) {
                TriggerShaderCompileAndReload(_Path);
                QTimer::singleShot(100, this, [this, _Path]() {
                    if (ShaderWatcher && !ShaderWatcher->files().contains(_Path))
                        ShaderWatcher->addPath(_Path); 
                });
            });

            // Diretorio mudou (arquivo novo/removido) -> adiciona os shaders novos ao watch.
            connect(ShaderWatcher, &QFileSystemWatcher::directoryChanged, this,
                    [this, CollectShaders, ShadersSourceDir](const QString&) {
                const QStringList Now = CollectShaders(ShadersSourceDir);
                const QStringList Watched = ShaderWatcher->files();
                for (const QString& F : Now)
                    if (!Watched.contains(F)) ShaderWatcher->addPath(F);
            });

            Smile::LogInfo("Shader Watcher Ativo: " +
                           std::to_string(ShaderFiles.size()) + " Shaders em " +
                           ShadersSourceDir.toStdString());
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

        // Carrega uma cena cozida (.sscene) gerada pelo SmileCooker (importacao Bistro).
        auto* LoadSceneAction = FileMenu->addAction(tr("Carregar Cena…"));
        LoadSceneAction->setShortcut(QKeySequence(tr("Ctrl+O")));
        connect(LoadSceneAction, &QAction::triggered, this, [this]() {
            if (!Viewport || !Viewport->GetRenderer() || !Viewport->GetRenderer()->IsInitialized())
                return;
            QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            QString File = QFileDialog::getOpenFileName(
                this, tr("Carregar Cena Cozida"), Start,
                tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            const bool Ok = Viewport->GetRenderer()->LoadCookedScene(File.toStdWString());
            if (!Ok)
                QMessageBox::warning(this, tr("Carregar Cena"),
                                     tr("Falha ao carregar a cena. Veja o console."));
        });
        // Carrega uma segunda cena cozida POR CIMA da atual, sem limpar (ex.: o interior
        // da Bistro alinhado com o exterior ja carregado).
        auto* AddSceneAction = FileMenu->addAction(tr("Adicionar Cena…"));
        AddSceneAction->setShortcut(QKeySequence(tr("Ctrl+Shift+O")));
        connect(AddSceneAction, &QAction::triggered, this, [this]() {
            if (!Viewport || !Viewport->GetRenderer() || !Viewport->GetRenderer()->IsInitialized())
                return;
            QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            QString File = QFileDialog::getOpenFileName(
                this, tr("Adicionar Cena Cozida"), Start,
                tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            const bool Ok = Viewport->GetRenderer()->LoadCookedScene(File.toStdWString(), /*Additive=*/true);
            if (!Ok)
                QMessageBox::warning(this, tr("Adicionar Cena"),
                                     tr("Falha ao adicionar a cena. Veja o console."));
        });
        FileMenu->addSeparator();

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

        // VSync: liga/desliga o trava-no-vblank do Present (default ligado).
        auto* VSyncAction = RenderMenu->addAction(tr("VSync"));
        VSyncAction->setCheckable(true);
        VSyncAction->setChecked(true);
        connect(VSyncAction, &QAction::toggled, this, [this](bool Enabled) {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                Viewport->GetRenderer()->SetVSync(Enabled);
        });

        // Frustum culling (runtime, default ligado) — corta meshes fora da tela.
        auto* CullAction = RenderMenu->addAction(tr("Frustum Culling"));
        CullAction->setCheckable(true);
        CullAction->setChecked(true);
        connect(CullAction, &QAction::toggled, this, [this](bool Enabled) {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                Viewport->GetRenderer()->SetFrustumCulling(Enabled);
        });

        // Depth pre-pass (runtime, default ligado) — mata overdraw de shading; toggle p/ medir.
        auto* PrepassAction = RenderMenu->addAction(tr("Depth Pre-pass"));
        PrepassAction->setCheckable(true);
        PrepassAction->setChecked(false); // default OFF: na Bistro custou mais que economizou
        connect(PrepassAction, &QAction::toggled, this, [this](bool Enabled) {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                Viewport->GetRenderer()->SetDepthPrepass(Enabled);
        });

        // Fusao por material (default desligado) — aplica no proximo "Carregar Cena".
        auto* MergeAction = RenderMenu->addAction(tr("Fundir por material (recarregar)"));
        MergeAction->setCheckable(true);
        MergeAction->setChecked(false);
        connect(MergeAction, &QAction::toggled, this, [this](bool Enabled) {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                Viewport->GetRenderer()->SetMergeByMaterial(Enabled);
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

        // Time-of-Day: textura da lua (LROC color da NASA). Path resolvido via SMILE_ASSETS_DIR
        // (so o editor o conhece); em falha o renderer segue na lua procedural branca.
        Viewport->GetRenderer()->LoadMoonTexture(
            QString(SMILE_ASSETS_DIR "/Textures/Sky/moon_lroc_color_2k.jpg").toStdWString());

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

    void MainWindow::UpdateStats() {
        if (!Viewport) return;
        auto* Renderer = Viewport->GetRenderer();
        if (!Renderer || !Renderer->IsInitialized()) return;

        float FPS = Viewport->GetFPS();
        const float FrameMs = FPS > 0.0f ? 1000.0f / FPS : 0.0f;

        if (FooterStatsLabel) {
            const auto& WaterStats = Renderer->GetWater().GetDebugStats();
            QString WaterText;
            if (Renderer->GetUseWater()) {
                WaterText = QString("  |  Ocean GPU: %1/%2 tiles  |  cull F:%3 C:%4 O:%5  |  draws:%6 L:%7 R:%8")
                    .arg(WaterStats.ValidTileCount)
                    .arg(WaterStats.CandidateCount)
                    .arg(WaterStats.FrustumCulledCount)
                    .arg(WaterStats.CoveredByFinerCount)
                    .arg(WaterStats.OutOfBoundsCount)
                    .arg(WaterStats.DrawCommandCount)
                    .arg(WaterStats.LevelCount)
                    .arg(WaterStats.RingRadius);
            }

            QString SceneText;
            if (Renderer->GetDrawCount() > 0) {
                SceneText = QString("  |  meshes: %1/%2")
                    .arg(Renderer->GetVisibleCount())
                    .arg(Renderer->GetDrawCount());
            }

            FooterStatsLabel->setText(QString("FPS: %1  |  Frame: %2 ms%3%4")
                .arg(FPS, 0, 'f', 1)
                .arg(FrameMs, 0, 'f', 2)
                .arg(SceneText)
                .arg(WaterText));
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
        Smile::LogInfo("Alteracao Detectada no Shader: " + QFileInfo(_Path).fileName().toStdString());

#ifdef SMILE_CMAKE_BINARY_DIR
        QString BuildDir = QStringLiteral(SMILE_CMAKE_BINARY_DIR);
#else
        QString BuildDir = QDir(QCoreApplication::applicationDirPath()).filePath("..");
#endif

        QProcess* CompileProcess = new QProcess(this);
        QStringList Arguments = { "--build", BuildDir, "--target", "Shaders" };

        Smile::LogInfo("Compilando Shader via CMake...");
        CompileProcess->start("cmake", Arguments);

        connect(CompileProcess, &QProcess::finished, this, [this, CompileProcess, _Path](int _ExitCode, QProcess::ExitStatus _Status) {
            Q_UNUSED(_Status);
            if (_ExitCode == 0) {
                if (Viewport && Viewport->GetRenderer()) {
                    // .hlsli (include) afeta varios shaders -> stem vazio forca reload completo.
                    // Caso contrario, deriva o stem do .cso: "WaterSurface.ps.hlsl" -> "WaterSurface.ps".
                    const QFileInfo ShaderInfo(_Path);
                    const bool IsInclude = ShaderInfo.suffix().compare("hlsli", Qt::CaseInsensitive) == 0;
                    const std::string ChangedStem =
                        IsInclude ? std::string() : ShaderInfo.completeBaseName().toStdString();
                    if (Viewport->GetRenderer()->ReloadShaders(ChangedStem)) {
                        statusBar()->showMessage(tr("Shader Recarregado com Sucesso."), 3000);
                    } else {
                        statusBar()->showMessage(tr("Erro ao Recarregar Shader no Renderer."), 3000);
                    }
                }
            } else {
                QString Errors = QString::fromUtf8(CompileProcess->readAllStandardError());
                if (Errors.isEmpty()) {
                    Errors = QString::fromUtf8(CompileProcess->readAllStandardOutput());
                }
                Smile::LogError("Falha ao Compilar Shader via CMake:\n" + Errors.toStdString());
                statusBar()->showMessage(tr("Falha na Compilação do Shader."), 3000);
            }
            CompileProcess->deleteLater();
        });
    }
}
