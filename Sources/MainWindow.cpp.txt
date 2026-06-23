#include "SmileEditor/MainWindow.h"
#include "SmileEditor/AboutDialog.h"
#include "SmileEditor/EnvironmentWindow.h"
#include "SmileEditor/LogBridge.h"
#include "SmileEditor/LucideIcon.h"
#include "SmileEditor/MenuBridge.h"
#include "SmileEditor/NativeWindowFilter.h"
#include "SmileEditor/QmlHost.h"
#include "SmileEditor/SmileLogo.h"
#include "SmileEditor/SmileLogoImageProvider.h"
#include "SmileEditor/StatusBridge.h"
#include "SmileEditor/WindowBridge.h"
#include "SmileEditor/ViewportWidget.h"
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
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QQuickWidget>
#include <QShortcut>
#include <QSizePolicy>
#include <QStatusBar>
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

        // Precisa existir antes de CreateDocks (o ConsolePanel.qml liga nele) e do sink de log.
        ConsoleLog = new LogBridge(this);
        WindowBr   = new WindowBridge(this); // botoes de janela da MainBar.qml
        Menus      = new MenuBridge(this);   // menus da MainBar.qml (precisa existir antes dela)

        CreateTopBar();
        setCentralWidget(CreateViewportChrome());
        CreateDocks();
        WireMenuActions(); // conecta os menus depois que Viewport e ConsoleDock existem

        // O LogBridge normaliza nivel->cor/tag e marshala para a thread da GUI; o ConsolePanel.qml
        // escuta LineAdded e preenche a ListView.
        Smile::SetLogSink([this](Smile::LogLevel level, std::string_view msg) {
            if (ConsoleLog) ConsoleLog->Append(level, msg);
        });

        CreateStatusBar();

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

    MainWindow::~MainWindow() {
        if (WinFilter) {
            qApp->removeNativeEventFilter(WinFilter);
            delete WinFilter;
        }
    }

    void MainWindow::changeEvent(QEvent* _Event) {
        QMainWindow::changeEvent(_Event);
        // Atualiza o icone maximizar/restaurar da MainBar quando o estado da janela muda
        // (botao, double-click no caption, Aero Snap).
        if (_Event->type() == QEvent::WindowStateChange && WindowBr)
            WindowBr->NotifyWindowStateChanged();
    }

    void MainWindow::CreateTopBar() {
        // Barra unificada em QML: windowBridge (min/max/fechar + zonas do hit-test), menuBridge
        // (menus) e smilelogo (logo procedural da engine como image provider).
        QQuickWidget* Bar = CreateQmlPanel(
            QStringLiteral("MainBar.qml"),
            { { QStringLiteral("windowBridge"), WindowBr },
              { QStringLiteral("menuBridge"),   Menus } },
            this,
            { { QStringLiteral("smilelogo"), new SmileLogoImageProvider() } });
        Bar->setObjectName("MainBar");
        setMenuWidget(Bar);

        // Frameless nativo (Slate-style): instala o filtro no HWND e recalcula o frame/sombra.
        // winId() realiza a janela nativa; o filtro precisa estar ativo antes do primeiro show.
        WinFilter = new NativeWindowFilter(winId(), WindowBr);
        qApp->installNativeEventFilter(WinFilter);
        NativeWindowFilter::EnableFrameless(winId());
    }

    void MainWindow::CreateStatusBar() {
        StatusBr = new StatusBridge(this);
        QQuickWidget* Bar = CreateQmlPanel(
            QStringLiteral("StatusBar.qml"),
            { { QStringLiteral("statusModel"), StatusBr } },
            this);
        Bar->setObjectName("StatusBar");

        // Hospeda no slot do QStatusBar (sempre no rodape, abaixo dos docks). addWidget com
        // stretch=1 ocupa a largura toda; nao usamos mais showMessage nativo.
        QStatusBar* Sb = statusBar();
        Sb->setObjectName("SmileStatusBar");
        Sb->setSizeGripEnabled(false);
        Sb->setContentsMargins(0, 0, 0, 0);
        Sb->addWidget(Bar, 1);
    }

    void MainWindow::WireMenuActions() {
        // Renderer pronto (ou nullptr). As acoes de cena/render so valem com a engine inicializada.
        auto RendererReady = [this]() -> Smile::Renderer* {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                return Viewport->GetRenderer();
            return nullptr;
        };

        // ---- Arquivo ----
        connect(Menus, &MenuBridge::LoadSceneRequested, this, [this, RendererReady]() {
            auto* R = RendererReady(); if (!R) return;
            const QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            const QString File = QFileDialog::getOpenFileName(
                this, tr("Carregar Cena Cozida"), Start, tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            if (!R->LoadCookedScene(File.toStdWString()))
                QMessageBox::warning(this, tr("Carregar Cena"),
                                     tr("Falha ao carregar a cena. Veja o console."));
        });
        connect(Menus, &MenuBridge::AddSceneRequested, this, [this, RendererReady]() {
            auto* R = RendererReady(); if (!R) return;
            const QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            const QString File = QFileDialog::getOpenFileName(
                this, tr("Adicionar Cena Cozida"), Start, tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            if (!R->LoadCookedScene(File.toStdWString(), /*Additive=*/true))
                QMessageBox::warning(this, tr("Adicionar Cena"),
                                     tr("Falha ao adicionar a cena. Veja o console."));
        });
        connect(Menus, &MenuBridge::QuitRequested, this, &QWidget::close);

        // ---- Janela ----
        connect(Menus, &MenuBridge::OpenEnvironmentRequested, this, &MainWindow::OnOpenEnvironmentWindow);
        connect(Menus, &MenuBridge::ToggleConsoleRequested, this, [this]() {
            if (ConsoleDock) ConsoleDock->setVisible(!ConsoleDock->isVisible());
        });

        // ---- Ajuda ----
        connect(Menus, &MenuBridge::AboutRequested, this, &MainWindow::OnHelpAbout);

        // ---- Atalhos globais (ApplicationShortcut: valem com o foco no viewport, nao so na barra).
        // Disparam o mesmo caminho dos menus (chamam o MenuBridge -> sinal -> handler acima).
        auto AddShortcut = [this](const QKeySequence& Seq, auto Slot) {
            auto* Sc = new QShortcut(Seq, this);
            Sc->setContext(Qt::ApplicationShortcut);
            connect(Sc, &QShortcut::activated, this, Slot);
        };
        AddShortcut(QKeySequence(tr("Ctrl+O")),       [this]{ Menus->loadScene(); });
        AddShortcut(QKeySequence(tr("Ctrl+Shift+O")), [this]{ Menus->addScene(); });
        AddShortcut(QKeySequence(tr("Ctrl+Shift+A")), [this]{ Menus->openEnvironment(); });
        AddShortcut(QKeySequence::Quit,               [this]{ close(); });
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
        ConsoleDock = new QDockWidget(tr("Console"), this);
        ConsoleDock->setObjectName("ConsoleDock");
        ConsoleDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        ConsoleDock->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable |
                                 QDockWidget::DockWidgetClosable);

        // Console em QML (ConsolePanel.qml), alimentado pela LogBridge via context property.
        QQuickWidget* Console = CreateQmlPanel(
            QStringLiteral("ConsolePanel.qml"),
            { { QStringLiteral("logModel"), ConsoleLog } },
            ConsoleDock);
        Console->setObjectName("ConsolePanel");

        ConsoleDock->setWidget(Console);

        // Barra de titulo virou 100% QML (dentro do ConsolePanel): esconde a nativa com um
        // title bar widget de altura zero. Perde-se arrastar/flutuar nativo (decisao do projeto);
        // fechar vem do menu QML -> CloseRequested -> close() do dock.
        auto* EmptyTitleBar = new QWidget(ConsoleDock);
        EmptyTitleBar->setFixedHeight(0);
        ConsoleDock->setTitleBarWidget(EmptyTitleBar);
        connect(ConsoleLog, &LogBridge::CloseRequested, ConsoleDock, &QDockWidget::close);

        addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);
        // Piso pra nunca colapsar a ponto de sumir; o tamanho de abertura vem do resizeDocks.
        Console->setMinimumHeight(120);
        // resizeDocks e a API confiavel pra altura inicial (resize() no dock e ignorado pelo layout).
        resizeDocks({ ConsoleDock }, { 160 }, Qt::Vertical);

        // Reflete o estado do dock no check do menu "Janela" (inclui o fechar via menu do console).
        connect(ConsoleDock, &QDockWidget::visibilityChanged, Menus, &MenuBridge::SetConsoleVisible);
    }

    void MainWindow::OnRendererReady() {
        if (!Viewport || !Viewport->GetRenderer()) return;

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
        if (!Viewport || !StatusBr) return;
        auto* Renderer = Viewport->GetRenderer();
        if (!Renderer || !Renderer->IsInitialized()) return;

        const float FPS = Viewport->GetFPS();
        const float FrameMs = FPS > 0.0f ? 1000.0f / FPS : 0.0f;

        // Textos SEM separador inicial — a StatusBar.qml os junta com "  |  ".
        QString OceanText;
        if (Renderer->GetUseWater()) {
            const auto& WaterStats = Renderer->GetWater().GetDebugStats();
            OceanText = QString("Ocean GPU: %1/%2 tiles  |  cull F:%3 C:%4 O:%5  |  draws:%6 L:%7 R:%8")
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
            SceneText = QString("meshes: %1/%2")
                .arg(Renderer->GetVisibleCount())
                .arg(Renderer->GetDrawCount());
        }

        StatusBr->SetStats(FPS, FrameMs, SceneText, OceanText);
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
                        if (StatusBr) StatusBr->ShowMessage(tr("Shader Recarregado com Sucesso."), 3000);
                    } else {
                        if (StatusBr) StatusBr->ShowMessage(tr("Erro ao Recarregar Shader no Renderer."), 3000);
                    }
                }
            } else {
                QString Errors = QString::fromUtf8(CompileProcess->readAllStandardError());
                if (Errors.isEmpty()) {
                    Errors = QString::fromUtf8(CompileProcess->readAllStandardOutput());
                }
                Smile::LogError("Falha ao Compilar Shader via CMake:\n" + Errors.toStdString());
                if (StatusBr) StatusBr->ShowMessage(tr("Falha na Compilação do Shader."), 3000);
            }
            CompileProcess->deleteLater();
        });
    }
}
