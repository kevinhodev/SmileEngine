#include "SmileEditor/Application/MainWindow.h"
#include "SmileEditor/UI/LogBridge.h"
#include "SmileEditor/Application/LucideIcon.h"
#include "SmileEditor/UI/MenuBridge.h"
#include "SmileEditor/Application/NativeWindowFilter.h"
#include "SmileEditor/UI/QmlHost.h"
#include "SmileEditor/Application/SmileLogo.h"
#include "SmileEditor/Application/SmileLogoImageProvider.h"
#include "SmileEditor/UI/StatusBridge.h"
#include "SmileEditor/Scene/TimeOfDayBridge.h"
#include "SmileEditor/Scene/CloudsBridge.h"
#include "SmileEditor/Rendering/RenderSettingsBridge.h"
#include "SmileEditor/Scene/LightsBridge.h"
#include "SmileEditor/Scene/SceneOutlinerBridge.h"
#include "SmileEditor/Scene/SceneDocument.h"
#include "SmileEditor/Scene/CameraBookmarksBridge.h"
#include "SmileEditor/Rendering/CaptureBridge.h"
#include "SmileEditor/Integration/McpBridge.h"
#include "SmileEditor/Rendering/RenderSettingsController.h"
#include "SmileEditor/Scene/MaterialsBridge.h"
#include "SmileEditor/Profiling/StatsBridge.h"
#include "SmileEditor/Debugging/DebugTargetsBridge.h"
#include "SmileEditor/UI/WindowBridge.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "SmileEditor/Application/DarkTheme.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/SceneLoader.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QDialog>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QCloseEvent>
#include <QMessageBox>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QFutureWatcher>
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
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace SmileEditor {
    MainWindow::MainWindow(QQmlEngine& _QmlEngine, QWidget* _Parent)
        : QMainWindow(_Parent), SharedQmlEngine(&_QmlEngine)
    {
        setWindowTitle(tr("Smile Engine"));
        setWindowIcon(QIcon(MakeSmileLogoPixmap(256)));
        resize(1536, 864);
        setMinimumSize(1120, 680);
        setWindowState(Qt::WindowMaximized);
        setObjectName("SmileMainWindow");

        setDockNestingEnabled(true);
        setDockOptions(dockOptions() | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

        // O sink deve existir antes de qualquer host QML emitir logs.
        ConsoleLog = new LogBridge(this);
        Smile::SetLogSink([Log = ConsoleLog](Smile::LogLevel Level, std::string_view Message) {
            Log->Append(Level, Message);
        });
        struct FSinkRollback {
            bool Armed = true;
            ~FSinkRollback() noexcept {
                if (!Armed) return;
                try {
                    Smile::SetLogSink({});
                } catch (...) {
                }
            }
        } SinkRollback;

        WindowBr   = new WindowBridge(this);
        Menus      = new MenuBridge(this);
        TodBridge  = new TimeOfDayBridge(this);
        CloudsBr   = new CloudsBridge(this);
        LightsBr   = new LightsBridge(this);
        OutlinerBr = new SceneOutlinerBridge(this);
        SceneDoc   = new SceneDocument(this);
        MaterialsBr = new MaterialsBridge(this);
        CameraBookmarksBr = new CameraBookmarksBridge(this);
        CaptureBr  = new CaptureBridge(this);
        RenderSettingsCtrl = new RenderSettingsController(this);
        McpBr      = new McpBridge(CaptureBr, CameraBookmarksBr, RenderSettingsCtrl, this);
        connect(McpBr, &McpBridge::ShutdownRequested, this, [this]() {
            // Automação pula o prompt, mas ainda aguarda o shutdown do renderer.
            CloseApproved = true;
            close();
        });
        RenderBr   = new RenderSettingsBridge(this);
        StatsBr    = new StatsBridge(this);
        DebugTargetsBr = new DebugTargetsBridge(this);

        // Mudanças externas invalidam os mesmos bindings usados pela UI.
        connect(RenderSettingsCtrl, &RenderSettingsController::GISettingsChanged,
                RenderBr, &RenderSettingsBridge::GISettingsChanged);
        connect(RenderSettingsCtrl, &RenderSettingsController::RenderSettingsChanged,
                RenderBr, &RenderSettingsBridge::RenderSettingsChanged);
        connect(RenderSettingsCtrl, &RenderSettingsController::StatsChanged,
                RenderBr, &RenderSettingsBridge::StatsChanged);
        connect(RenderSettingsCtrl, &RenderSettingsController::TimeOfDayChanged,
                TodBridge, &TimeOfDayBridge::StateChanged);
        connect(RenderSettingsCtrl, &RenderSettingsController::TimeOfDayChanged,
                TodBridge, &TimeOfDayBridge::TimeChanged);

        connect(LightsBr, &LightsBridge::LightsChanged,
                OutlinerBr, &SceneOutlinerBridge::Rebuild);
        connect(CloudsBr, &CloudsBridge::SettingsChanged,
                OutlinerBr, &SceneOutlinerBridge::Refresh);

        connect(OutlinerBr, &SceneOutlinerBridge::DirtyChanged,
                SceneDoc, &SceneDocument::markDirty);

        connect(MaterialsBr, &MaterialsBridge::VisibilityChanged,
                OutlinerBr, &SceneOutlinerBridge::Rebuild);
        connect(MaterialsBr, &MaterialsBridge::RevealInOutlinerRequested, this, [this]() {
            if (LightsDock) { LightsDock->show(); LightsDock->raise(); }
        });

        CreateTopBar();
        setCentralWidget(CreateViewportChrome());
        CreateDocks();
        WireMenuActions();

        CreateStatusBar();

        // Persistência e captura publicam o resultado assíncrono na barra de status.
        if (CameraBookmarksBr && StatusBr) {
            connect(CameraBookmarksBr, &CameraBookmarksBridge::Message, this,
                    [this](const QString& _Text) { StatusBr->ShowMessage(_Text, 4000); });
        }
        if (CaptureBr && StatusBr) {
            connect(CaptureBr, &CaptureBridge::Message, this,
                    [this](const QString& _Text) { StatusBr->ShowMessage(_Text, 6000); });
        }

        connect(StatsBr, &StatsBridge::Updated,                  this, &MainWindow::UpdateStats);
        connect(Viewport, &ViewportWidget::RendererInitialized, this, &MainWindow::OnRendererReady);
        // Oceano pode ser alterado pelo Outliner ou pelas configurações.
        connect(OutlinerBr, &SceneOutlinerBridge::EnvChanged,
                RenderBr, &RenderSettingsBridge::OceanSettingsChanged);
        connect(RenderBr, &RenderSettingsBridge::OceanSettingsChanged,
                OutlinerBr, &SceneOutlinerBridge::Refresh);
        connect(Viewport, &ViewportWidget::InitProgress, this,
                [this](const QString& _Label, const QString& _Detail, qreal _Fraction) {
            // Reserva o trecho final da barra para a cena de boot.
            const qreal Scale = StartupScenePath.isEmpty() ? 1.0 : 0.9;
            emit BootProgress(_Label, _Detail, _Fraction * Scale);
        });
        connect(Viewport, &ViewportWidget::RendererStopped, this, &MainWindow::FinishBootStage);

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

            // Reclassifica arquivos quando o diretório muda.
            connect(ShaderWatcher, &QFileSystemWatcher::directoryChanged, this,
                    [this, CollectShaders, ShadersSourceDir](const QString&) {
                const QStringList Now = CollectShaders(ShadersSourceDir);
                const QStringList Watched = ShaderWatcher->files();
                for (const QString& F : Now)
                    if (!Watched.contains(F)) ShaderWatcher->addPath(F);
            });

            Smile::LogDebug("Shader Watcher Ativo: " +
                           std::to_string(ShaderFiles.size()) + " Shaders em " +
                           ShadersSourceDir.toStdString());
        } else {
            Smile::LogWarning("Diretorio de shaders de origem nao encontrado: " + ShadersSourceDir.toStdString());
        }
        SinkRollback.Armed = false;
    }

    MainWindow::~MainWindow() {
        // Desconecta o sink antes de destruir os filhos Qt.
        try {
            Smile::SetLogSink({});
        } catch (...) {
            Smile::LogError("Falha absorvida ao desconectar o console durante o teardown");
        }
        if (WinFilter) {
            qApp->removeNativeEventFilter(WinFilter);
            delete WinFilter;
        }
    }

    void MainWindow::changeEvent(QEvent* _Event) {
        QMainWindow::changeEvent(_Event);
        if (_Event->type() == QEvent::WindowStateChange && WindowBr)
            WindowBr->NotifyWindowStateChanged();
    }

    bool MainWindow::eventFilter(QObject* _Obj, QEvent* _Event) {
        // Foco ou clique define a viewport ativa para bridges e atalhos globais.
        if (_Event->type() == QEvent::FocusIn || _Event->type() == QEvent::MouseButtonPress) {
            const QVariant TargetProperty = _Obj->property("smileViewportTarget");
            if (QObject* TargetObject = TargetProperty.value<QObject*>()) {
                if (auto* TargetViewport = qobject_cast<ViewportWidget*>(TargetObject)) {
                    ActiveViewport = TargetViewport;
                    if (StatsBr) StatsBr->SetViewport(TargetViewport);
                    if (DebugTargetsBr) DebugTargetsBr->SetViewport(TargetViewport);
                }
            }
        }

        if (TodDlg && _Obj == TodDlg &&
            (_Event->type() == QEvent::Show || _Event->type() == QEvent::Hide)) {
            if (Menus) Menus->SetTimeOfDayVisible(_Event->type() == QEvent::Show);
        }
        if (StatsDlg && _Obj == StatsDlg &&
            (_Event->type() == QEvent::Show || _Event->type() == QEvent::Hide)) {
            if (Menus) Menus->SetStatsVisible(_Event->type() == QEvent::Show);
        }
        if (DebugTargetsDlg && _Obj == DebugTargetsDlg &&
            (_Event->type() == QEvent::Show || _Event->type() == QEvent::Hide)) {
            const bool Shown = _Event->type() == QEvent::Show;
            if (Menus) Menus->SetDebugTargetsVisible(Shown);
            // Ocultar pausa o readback, mas preserva a seleção.
            if (DebugTargetsBr) DebugTargetsBr->SetPreviewEnabled(Shown);
        }
        if (MaterialsDlg && _Obj == MaterialsDlg &&
            (_Event->type() == QEvent::Show || _Event->type() == QEvent::Hide)) {
            const bool Shown = _Event->type() == QEvent::Show;
            if (Menus)       Menus->SetMaterialsVisible(Shown);
            if (MaterialsBr) MaterialsBr->SetPreviewEnabled(Shown);
        }
        return QMainWindow::eventFilter(_Obj, _Event);
    }

    void MainWindow::closeEvent(QCloseEvent* _Event) {
        // O HWND só é destruído após a render thread encerrar Initialize/Present.
        if (RendererShutdownForClose) {
            _Event->ignore();
            return;
        }
        if (CloseApproved) {
            ContinueApprovedClose(_Event);
            return;
        }

        const bool LightsDirty    = LightsBr    && LightsBr->Dirty();
        const bool VisDirty       = OutlinerBr  && OutlinerBr->Dirty();
        const bool MaterialsDirty = MaterialsBr && MaterialsBr->Dirty();
        const bool MapDirty       = SceneDoc    && SceneDoc->Dirty();
        if (!LightsDirty && !VisDirty && !MaterialsDirty && !MapDirty) {
            CloseApproved = true;
            ContinueApprovedClose(_Event);
            return;
        }

        QStringList Pending;
        if (LightsDirty)    Pending << tr("luzes");
        if (VisDirty)       Pending << tr("visibilidade");
        if (MaterialsDirty) Pending << tr("materiais");
        if (MapDirty)       Pending << tr("cena");

        const auto Choice = QMessageBox::question(
            this, tr("Alterações não salvas"),
            tr("Há alterações não salvas de: %1.\nSalvar antes de sair?")
                .arg(Pending.join(QStringLiteral(", "))),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (Choice == QMessageBox::Cancel) {
            _Event->ignore();
            return;
        }
        if (Choice == QMessageBox::Save) {
            QStringList Failed;
            if (LightsDirty    && !LightsBr->saveLights())       Failed << tr("luzes");
            if (VisDirty       && !OutlinerBr->saveVisibility()) Failed << tr("visibilidade");
            if (MaterialsDirty && !MaterialsBr->saveMaterials()) Failed << tr("materiais");
            if (MapDirty       && !SceneDoc->save())             Failed << tr("cena");

            if (!Failed.isEmpty()) {
                // Falha de escrita mantém Cancelar como opção segura padrão.
                const auto OnFailure = QMessageBox::warning(
                    this, tr("Falha ao salvar"),
                    tr("Não foi possível salvar: %1.\n"
                       "Fechar mesmo assim descarta essas alterações.")
                        .arg(Failed.join(QStringLiteral(", "))),
                    QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (OnFailure != QMessageBox::Discard) {
                    _Event->ignore();
                    return;
                }
            }
        }
        CloseApproved = true;
        ContinueApprovedClose(_Event);
    }

    void MainWindow::ContinueApprovedClose(QCloseEvent* _Event) {
        if (Viewport && !Viewport->IsRendererStopped()) {
            RendererShutdownForClose = true;
            _Event->ignore();
            Viewport->BeginRendererShutdown();
            return;
        }
        QMainWindow::closeEvent(_Event);
    }

    void MainWindow::CreateTopBar() {
        QQuickWidget* Bar = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("MainBar.qml"),
            { { QStringLiteral("windowBridge"), WindowBr },
              { QStringLiteral("menuBridge"),   Menus } },
            this,
            { { QStringLiteral("smilelogo"), new SmileLogoImageProvider() } });
        Bar->setObjectName("MainBar");
        setMenuWidget(Bar);

        // O filtro nativo deve existir antes do primeiro show do HWND frameless.
        WinFilter = new NativeWindowFilter(winId(), WindowBr);
        connect(WinFilter, &NativeWindowFilter::InteractiveResizeStarted, this, [this]() {
            if (Viewport) Viewport->BeginInteractiveResize();
        });
        connect(WinFilter, &NativeWindowFilter::InteractiveResizeFinished, this, [this]() {
            if (Viewport) Viewport->EndInteractiveResize();
        });
        qApp->installNativeEventFilter(WinFilter);
        NativeWindowFilter::EnableFrameless(winId());
    }

    void MainWindow::CreateStatusBar() {
        StatusBr = new StatusBridge(this);
        QQuickWidget* Bar = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("StatusBar.qml"),
            { { QStringLiteral("statusModel"), StatusBr } },
            this);
        Bar->setObjectName("StatusBar");

        QStatusBar* Sb = statusBar();
        Sb->setObjectName("SmileStatusBar");
        Sb->setSizeGripEnabled(false);
        Sb->setContentsMargins(0, 0, 0, 0);
        Sb->addWidget(Bar, 1);
    }

    void MainWindow::BeginSceneLoad(const QString& _Path, bool _Additive) {
        if (SceneLoadInProgress) {
            if (StatusBr) StatusBr->ShowMessage(tr("Uma cena já está sendo preparada"), 2500);
            return;
        }
        if (!Viewport || !Viewport->GetRenderer() || !Viewport->GetRenderer()->IsInitialized()) {
            FinishBootStage();
            return;
        }

        SceneLoadInProgress = true;
        if (StatusBr) {
            StatusBr->ShowMessage(
                _Additive ? tr("Preparando cena adicional…") : tr("Preparando cena…"));
        }
        if (BootSplashActive) emit BootProgress(tr("Preparando cena…"), {}, 0.93);

        using Result = Smile::FSceneImportResultPtr;
        auto* Watcher = new QFutureWatcher<Result>(this);
        connect(Watcher, &QFutureWatcher<Result>::finished, this,
                [this, Watcher, Path = _Path, Additive = _Additive]() {
            Result Imported = Watcher->result();
            Watcher->deleteLater();
            if (CloseApproved || RendererShutdownForClose) {
                SceneLoadInProgress = false;
                FinishBootStage();
                return;
            }
            if (!Imported) {
                SceneLoadInProgress = false;
                FinishBootStage();
                if (StatusBr) StatusBr->ShowMessage(tr("Falha ao preparar a cena"), 5000);
                QMessageBox::warning(
                    this, Additive ? tr("Adicionar Cena") : tr("Carregar Cena"),
                    tr("Falha ao carregar a cena. Veja o console."));
                return;
            }

            if (StatusBr) StatusBr->ShowMessage(tr("Finalizando recursos da cena…"));
            if (BootSplashActive)
                emit BootProgress(tr("Finalizando recursos da cena…"), {}, 0.98);
            const bool Queued = Viewport && Viewport->CommitImportedSceneAsync(
                std::move(Imported), Additive,
                [this, Path, Additive](bool _Success, const QString& _Error) {
                    Q_UNUSED(_Error);
                    SceneLoadInProgress = false;
                    FinishBootStage();

                    if (!_Success) {
                        if (StatusBr)
                            StatusBr->ShowMessage(tr("Falha ao finalizar a cena"), 5000);
                        QMessageBox::warning(
                            this, Additive ? tr("Adicionar Cena") : tr("Carregar Cena"),
                            tr("Falha ao carregar a cena. Veja o console."));
                        return;
                    }

                    if (LightsBr)    LightsBr->OnSceneLoaded(Path, Additive);
                    // A camada autorada deve ser aplicada antes de reconstruir o outliner.
                    if (SceneDoc)    SceneDoc->OnSceneLoaded(Path, Additive);
                    if (OutlinerBr)  OutlinerBr->OnSceneLoaded(Path, Additive);
                    if (MaterialsBr) MaterialsBr->OnSceneLoaded(Path, Additive);
                    if (CameraBookmarksBr) CameraBookmarksBr->OnSceneLoaded(Path, Additive);
                    if (CaptureBr)   CaptureBr->OnSceneLoaded(Path, Additive);
                    if (McpBr)       McpBr->OnSceneLoaded(Path, Additive);
                    if (DebugTargetsBr) DebugTargetsBr->NotifyResourcesChanged();
                    if (StatusBr) {
                        StatusBr->ShowMessage(
                            Additive ? tr("Cena adicionada") : tr("Cena carregada"), 3000);
                    }
                });
            if (!Queued) {
                SceneLoadInProgress = false;
                FinishBootStage();
                if (StatusBr) StatusBr->ShowMessage(tr("Renderizador indisponível"), 5000);
                QMessageBox::warning(
                    this, Additive ? tr("Adicionar Cena") : tr("Carregar Cena"),
                    tr("O renderizador foi encerrado antes de finalizar a cena."));
            }
        });

        Watcher->setFuture(QtConcurrent::run(
            [ScenePath = _Path.toStdWString()]() {
                return Smile::LoadCookedSceneData(ScenePath);
            }));
    }

    void MainWindow::WireMenuActions() {
        auto RendererReady = [this]() -> RendererHandle {
            if (Viewport && Viewport->GetRenderer() && Viewport->GetRenderer()->IsInitialized())
                return Viewport->GetRenderer();
            return {};
        };

        // ---- Arquivo ----
        connect(Menus, &MenuBridge::LoadSceneRequested, this, [this, RendererReady]() {
            if (!RendererReady()) return;
            const QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            const QString File = QFileDialog::getOpenFileName(
                this, tr("Carregar Cena Cozida"), Start, tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            BeginSceneLoad(File, /*additive=*/false);
        });
        connect(Menus, &MenuBridge::AddSceneRequested, this, [this, RendererReady]() {
            if (!RendererReady()) return;
            const QString Start = QStringLiteral(SMILE_ASSETS_DIR) + QStringLiteral("/Scenes");
            const QString File = QFileDialog::getOpenFileName(
                this, tr("Adicionar Cena Cozida"), Start, tr("Cena SmileEngine (*.sscene)"));
            if (File.isEmpty()) return;
            BeginSceneLoad(File, /*additive=*/true);
        });
        connect(Menus, &MenuBridge::QuitRequested, this, &QWidget::close);

        // ---- Janela ----
        connect(Menus, &MenuBridge::ToggleConsoleRequested, this, [this]() {
            if (ConsoleDock) ConsoleDock->setVisible(!ConsoleDock->isVisible());
        });
        connect(Menus, &MenuBridge::ToggleTimeOfDayRequested, this, [this]() {
            if (TodDlg && TodDlg->isVisible()) TodDlg->hide();
            else                               ShowTimeOfDay();
        });
        connect(Menus, &MenuBridge::ToggleLightsRequested, this, [this]() {
            if (LightsDock) LightsDock->setVisible(!LightsDock->isVisible());
        });
        connect(Menus, &MenuBridge::ToggleStatsRequested, this, [this]() {
            if (StatsDlg && StatsDlg->isVisible()) StatsDlg->hide();
            else                                   ShowStats();
        });
        connect(Menus, &MenuBridge::ToggleDebugTargetsRequested, this, [this]() {
            if (DebugTargetsDlg && DebugTargetsDlg->isVisible()) DebugTargetsDlg->hide();
            else                                                 ShowDebugTargets();
        });
        connect(Menus, &MenuBridge::ToggleMaterialsRequested, this, [this]() {
            if (MaterialsDlg && MaterialsDlg->isVisible()) MaterialsDlg->hide();
            else                                           ShowMaterials();
        });
        connect(Menus, &MenuBridge::SettingsRequested, this, &MainWindow::ShowSettings);
        connect(Viewport, &ViewportWidget::SettingsRequested, this, &MainWindow::ShowSettings);
        connect(Viewport, &ViewportWidget::SceneEdited, SceneDoc, &SceneDocument::markDirty);

        // Seleções de objeto e luz são mutuamente exclusivas.
        connect(Viewport, &ViewportWidget::DeleteSelectionRequested, this, [this] {
            if (OutlinerBr && OutlinerBr->MeshSelected()) { OutlinerBr->deleteSelectedMesh(); return; }
            if (LightsBr && LightsBr->SelectedIndex() >= 0) LightsBr->removeLight(LightsBr->SelectedIndex());
        });
        connect(Viewport, &ViewportWidget::DuplicateSelectionRequested, this, [this] {
            if (OutlinerBr && OutlinerBr->MeshSelected()) { OutlinerBr->duplicateSelectedMesh(); return; }
            if (LightsBr && LightsBr->SelectedIndex() >= 0) LightsBr->duplicateLight(LightsBr->SelectedIndex());
        });

        // Atalhos globais disparam o mesmo caminho dos menus.
        auto AddShortcut = [this](const QKeySequence& Seq, auto Slot) {
            auto* Sc = new QShortcut(Seq, this);
            Sc->setContext(Qt::ApplicationShortcut);
            connect(Sc, &QShortcut::activated, this, Slot);
        };
        AddShortcut(QKeySequence(tr("Ctrl+O")),       [this]{ Menus->loadScene(); });
        AddShortcut(QKeySequence(tr("Ctrl+Shift+O")), [this]{ Menus->addScene(); });
        AddShortcut(QKeySequence(tr("Ctrl+Shift+T")), [this]{ Menus->toggleDebugTargets(); });
        AddShortcut(QKeySequence(tr("Ctrl+,")),       [this]{ ShowSettings(); });
        AddShortcut(QKeySequence::Quit,               [this]{ close(); });
        AddShortcut(QKeySequence(tr("Alt+1")), [this] {
            if (ActiveViewport) ActiveViewport->SelectLit();
        });
        AddShortcut(QKeySequence(tr("Alt+5")), [this] {
            if (ActiveViewport) ActiveViewport->SelectReflectionHeatmap();
        });
    }

    void MainWindow::RegisterViewport(ViewportWidget* _Viewport, QWidget* _Toolbar) {
        if (!_Viewport) return;

        connect(_Viewport, &ViewportWidget::RendererStopped, this, [this, _Viewport]() {
            if (!RendererShutdownForClose || _Viewport != Viewport) return;
            RendererShutdownForClose = false;
            // Destrói a árvore QWidget depois que o callback Stopped retornar.
            QTimer::singleShot(0, this, [this]() { close(); });
        });

        const QVariant Target = QVariant::fromValue(static_cast<QObject*>(_Viewport));
        _Viewport->setProperty("smileViewportTarget", Target);
        _Viewport->installEventFilter(this);

        if (_Toolbar) {
            _Toolbar->setProperty("smileViewportTarget", Target);
            _Toolbar->installEventFilter(this);
        }

        if (!ActiveViewport) {
            ActiveViewport = _Viewport;
            if (StatsBr) StatsBr->SetViewport(_Viewport);
            if (DebugTargetsBr) DebugTargetsBr->SetViewport(_Viewport);
        }
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

        QVariantMap ToolbarProperties;
        ToolbarProperties.insert(
            QStringLiteral("viewportModel"),
            QVariant::fromValue(static_cast<QObject*>(Viewport)));
        ToolbarProperties.insert(
            QStringLiteral("renderModel"),
            QVariant::fromValue(static_cast<QObject*>(RenderBr)));
        ToolbarProperties.insert(
            QStringLiteral("debugModel"),
            QVariant::fromValue(static_cast<QObject*>(DebugTargetsBr)));

        QQuickWidget* Toolbar = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("ViewportToolbar.qml"),
            {},
            Shell,
            {},
            ToolbarProperties);
        Toolbar->setObjectName("ViewportToolbar");
        Toolbar->setFixedHeight(34);
        Toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        RegisterViewport(Viewport, Toolbar);
        Layout->addWidget(Toolbar);
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

        QQuickWidget* Console = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("ConsolePanel.qml"),
            { { QStringLiteral("logModel"), ConsoleLog } },
            ConsoleDock);
        Console->setObjectName("ConsolePanel");

        ConsoleDock->setWidget(Console);

        // O chrome QML substitui a title bar nativa do dock.
        auto* EmptyTitleBar = new QWidget(ConsoleDock);
        EmptyTitleBar->setFixedHeight(0);
        ConsoleDock->setTitleBarWidget(EmptyTitleBar);
        connect(ConsoleLog, &LogBridge::CloseRequested, ConsoleDock, &QDockWidget::close);

        addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);
        Console->setMinimumHeight(82);
        resizeDocks({ ConsoleDock }, { 124 }, Qt::Vertical);

        connect(ConsoleDock, &QDockWidget::visibilityChanged, Menus, &MenuBridge::SetConsoleVisible);

        // Cena / Scene Outliner.
        LightsDock = new QDockWidget(tr("Cena"), this);
        LightsDock->setObjectName("SceneOutlinerDock");
        LightsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        LightsDock->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable |
                                QDockWidget::DockWidgetClosable);

        QQuickWidget* OutlinerPanel = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("SceneOutlinerPanel.qml"),
            { { QStringLiteral("outlinerModel"), OutlinerBr },
              { QStringLiteral("sceneDoc"),       SceneDoc },
              { QStringLiteral("lightsModel"),   LightsBr },
              { QStringLiteral("cloudsModel"),   CloudsBr } },
            LightsDock);
        OutlinerPanel->setObjectName("SceneOutlinerPanel");
        LightsDock->setWidget(OutlinerPanel);

        auto* OutlinerEmptyTitleBar = new QWidget(LightsDock);
        OutlinerEmptyTitleBar->setFixedHeight(0);
        LightsDock->setTitleBarWidget(OutlinerEmptyTitleBar);
        connect(OutlinerBr, &SceneOutlinerBridge::CloseRequested, LightsDock, &QDockWidget::close);

        addDockWidget(Qt::RightDockWidgetArea, LightsDock);
        OutlinerPanel->setMinimumWidth(300);
        resizeDocks({ LightsDock }, { 340 }, Qt::Horizontal);

        connect(LightsDock, &QDockWidget::visibilityChanged, Menus, &MenuBridge::SetLightsVisible);
    }

    void MainWindow::OnRendererReady() {
        if (!Viewport || !Viewport->GetRenderer()) return;

        Viewport->GetRenderer()->LoadMoonTexture(
            QString(SMILE_ASSETS_DIR "/Textures/Sky/moon_lroc_color_2k.jpg").toStdWString());

        Viewport->GetRenderer()->LoadStarCatalog(
            QString(SMILE_ASSETS_DIR "/Sky/stars.sstars").toStdWString());

        if (RenderBr) {
            RenderBr->SetRenderer(Viewport->GetRenderer());
            RenderBr->SetViewport(Viewport);
        }

        if (CloudsBr) {
            CloudsBr->SetViewport(Viewport);
            CloudsBr->SetRenderer(Viewport->GetRenderer());
        }

        if (CameraBookmarksBr) CameraBookmarksBr->SetRenderer(Viewport->GetRenderer());
        if (CaptureBr) CaptureBr->SetRenderer(Viewport->GetRenderer());
        if (RenderSettingsCtrl) RenderSettingsCtrl->SetViewport(Viewport);

        if (TodBridge) {
            TodBridge->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    TodBridge, &TimeOfDayBridge::Refresh, Qt::UniqueConnection);
        }

        if (LightsBr) {
            LightsBr->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    LightsBr, &LightsBridge::Refresh, Qt::UniqueConnection);
        }

        if (OutlinerBr) {
            OutlinerBr->SetRenderer(Viewport->GetRenderer());
            SceneDoc->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    OutlinerBr, &SceneOutlinerBridge::Refresh, Qt::UniqueConnection);
        }

        if (MaterialsBr) {
            MaterialsBr->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    MaterialsBr, &MaterialsBridge::Refresh, Qt::UniqueConnection);
        }

        if (!StartupScenePath.isEmpty()) {
            // A cena de boot faz parte do estágio coberto pela splash.
            BeginSceneLoad(StartupScenePath, /*additive=*/false);
        } else {
            FinishBootStage();
        }
    }

    void MainWindow::FinishBootStage() {
        if (!BootSplashActive) return;
        BootSplashActive = false;
        emit BootFinished();
    }

    void MainWindow::UpdateStats() {
        if (!ActiveViewport || !StatsBr || !StatusBr) return;
        auto Renderer = ActiveViewport->GetRenderer();
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) return;

        const float FPS = static_cast<float>(StatsBr->GetFPS());
        const float FrameMs = FPS > 0.0f ? 1000.0f / FPS : 0.0f;

        QString OceanText;
        if (RendererAccess->Settings().GetUseWater()) {
            const auto& WaterStats = RendererAccess->GetWater().GetDebugStats();
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
        if (RendererAccess->GetDrawCount() > 0) {
            SceneText = QString("meshes: %1/%2")
                .arg(RendererAccess->GetVisibleCount())
                .arg(RendererAccess->GetDrawCount());
        }

        StatusBr->SetStats(FPS, FrameMs, SceneText, OceanText);
    }

    void MainWindow::ShowSettings() {
        if (!SettingsDlg) {
            auto* Dialog = new QDialog(this);
            Dialog->setObjectName(QStringLiteral("SettingsWindow"));
            Dialog->setWindowTitle(tr("Configurações — SmileEngine"));
            Dialog->setWindowIcon(windowIcon());
            Dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                                   Qt::WindowMinMaxButtonsHint);
            Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Dialog->resize(960, 640);
            Dialog->setMinimumSize(960, 640);

            auto* SettingsWindowBridge = new WindowBridge(Dialog, Dialog);
            QQuickWidget* Panel = CreateQmlPanel(*SharedQmlEngine,
                QStringLiteral("SettingsWindow.qml"),
                { { QStringLiteral("viewportModel"), Viewport },
                  { QStringLiteral("renderModel"), RenderBr },
                  { QStringLiteral("cameraBookmarks"), CameraBookmarksBr },
                  { QStringLiteral("capture"), CaptureBr },
                  { QStringLiteral("settingsWindow"), SettingsWindowBridge } },
                Dialog);
            Panel->setObjectName(QStringLiteral("SettingsPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            SettingsDlg = Dialog;
        }

        if (SettingsDlg->isMinimized()) SettingsDlg->showNormal();
        else                            SettingsDlg->show();

        // Preserva a posição escolhida pelo usuário após a primeira abertura.
        if (!SettingsDlg->property("smilePositioned").toBool()) {
            const QPoint Center = frameGeometry().center();
            SettingsDlg->move(Center.x() - SettingsDlg->width() / 2,
                              Center.y() - SettingsDlg->height() / 2);
            SettingsDlg->setProperty("smilePositioned", true);
        }
        SettingsDlg->raise();
        SettingsDlg->activateWindow();
    }

    void MainWindow::ShowTimeOfDay() {
        if (!TodDlg) {
            auto* Dialog = new QDialog(this);
            Dialog->setObjectName(QStringLiteral("TimeOfDayWindow"));
            Dialog->setWindowTitle(tr("Time of Day — SmileEngine"));
            Dialog->setWindowIcon(windowIcon());
            Dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                                   Qt::WindowMinimizeButtonHint);
            Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Dialog->resize(840, 672);
            Dialog->setMinimumSize(840, 672);

            auto* TodWindowBridge = new WindowBridge(Dialog, Dialog);
            QQuickWidget* Panel = CreateQmlPanel(*SharedQmlEngine,
                QStringLiteral("TimeOfDayWindow.qml"),
                { { QStringLiteral("todModel"), TodBridge },
                  { QStringLiteral("todWindow"), TodWindowBridge } },
                Dialog);
            Panel->setObjectName(QStringLiteral("TimeOfDayWindowPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            Dialog->installEventFilter(this);
            TodDlg = Dialog;
        }

        if (TodDlg->isMinimized()) TodDlg->showNormal();
        else                       TodDlg->show();

        // Preserva a posição escolhida pelo usuário após a primeira abertura.
        if (!TodDlg->property("smilePositioned").toBool()) {
            const QPoint Center = frameGeometry().center();
            TodDlg->move(Center.x() - TodDlg->width() / 2,
                         Center.y() - TodDlg->height() / 2);
            TodDlg->setProperty("smilePositioned", true);
        }
        TodDlg->raise();
        TodDlg->activateWindow();
    }

    void MainWindow::ShowStats() {
        if (!StatsDlg) {
            auto* Dialog = new QDialog(this);
            Dialog->setObjectName(QStringLiteral("StatsWindow"));
            Dialog->setWindowTitle(tr("Profiler — SmileEngine"));
            Dialog->setWindowIcon(windowIcon());
            Dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                                   Qt::WindowMinimizeButtonHint);
            Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Dialog->resize(880, 760);
            Dialog->setMinimumSize(760, 620);

            auto* StatsWindowBridge = new WindowBridge(Dialog, Dialog);
            QQuickWidget* Panel = CreateQmlPanel(*SharedQmlEngine,
                QStringLiteral("StatsWindow.qml"),
                { { QStringLiteral("statsModel"), StatsBr },
                  { QStringLiteral("statsWindow"), StatsWindowBridge } },
                Dialog);
            Panel->setObjectName(QStringLiteral("StatsWindowPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            Dialog->installEventFilter(this);
            StatsDlg = Dialog;
        }

        if (StatsDlg->isMinimized()) StatsDlg->showNormal();
        else                         StatsDlg->show();

        // Preserva a posição escolhida pelo usuário após a primeira abertura.
        if (!StatsDlg->property("smilePositioned").toBool()) {
            const QPoint Center = frameGeometry().center();
            StatsDlg->move(Center.x() - StatsDlg->width() / 2,
                           Center.y() - StatsDlg->height() / 2);
            StatsDlg->setProperty("smilePositioned", true);
        }
        StatsDlg->raise();
        StatsDlg->activateWindow();
    }

    void MainWindow::ShowDebugTargets() {
        if (!DebugTargetsDlg) {
            auto* Dialog = new QDialog(this);
            Dialog->setObjectName(QStringLiteral("DebugTargetsWindow"));
            Dialog->setWindowTitle(tr("Render targets — SmileEngine"));
            Dialog->setWindowIcon(windowIcon());
            Dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                                   Qt::WindowMinimizeButtonHint |
                                   Qt::WindowMaximizeButtonHint);
            Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Dialog->resize(1180, 720);
            Dialog->setMinimumSize(900, 560);

            auto* DebugWindowBridge = new WindowBridge(Dialog, Dialog);
            QQuickWidget* Panel = CreateQmlPanel(*SharedQmlEngine,
                QStringLiteral("DebugTargetsWindow.qml"),
                { { QStringLiteral("debugModel"), DebugTargetsBr },
                  { QStringLiteral("debugWindow"), DebugWindowBridge } },
                Dialog,
                { { QStringLiteral("debugtargetpreview"),
                    new DebugTargetPreviewImageProvider(DebugTargetsBr) } });
            Panel->setObjectName(QStringLiteral("DebugTargetsWindowPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            Dialog->installEventFilter(this);
            DebugTargetsDlg = Dialog;
        }

        if (DebugTargetsDlg->isMinimized()) DebugTargetsDlg->showNormal();
        else                                DebugTargetsDlg->show();

        if (!DebugTargetsDlg->property("smilePositioned").toBool()) {
            const QPoint Center = geometry().center();
            DebugTargetsDlg->move(Center.x() - DebugTargetsDlg->width() / 2,
                                  Center.y() - DebugTargetsDlg->height() / 2);
            DebugTargetsDlg->setProperty("smilePositioned", true);
        }
        DebugTargetsDlg->raise();
        DebugTargetsDlg->activateWindow();
    }

    void MainWindow::ShowMaterials() {
        if (!MaterialsDlg) {
            auto* Dialog = new QDialog(this);
            Dialog->setObjectName(QStringLiteral("MaterialsWindow"));
            Dialog->setWindowTitle(tr("Materiais — SmileEngine"));
            Dialog->setWindowIcon(windowIcon());
            Dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint |
                                   Qt::WindowMinimizeButtonHint);
            Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Dialog->resize(1460, 800);
            Dialog->setMinimumSize(1240, 660);

            auto* MaterialsWindowBridge = new WindowBridge(Dialog, Dialog);
            QQuickWidget* Panel = CreateQmlPanel(*SharedQmlEngine,
                QStringLiteral("MaterialsWindow.qml"),
                { { QStringLiteral("materialsModel"),  MaterialsBr },
                  { QStringLiteral("materialsWindow"), MaterialsWindowBridge } },
                Dialog,
                { { QStringLiteral("materialpreview"),
                    new MaterialPreviewImageProvider(MaterialsBr) },
                  { QStringLiteral("materialthumb"),
                    new MaterialThumbImageProvider(MaterialsBr) } });
            Panel->setObjectName(QStringLiteral("MaterialsWindowPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            Dialog->installEventFilter(this);
            MaterialsDlg = Dialog;

            connect(MaterialsBr, &MaterialsBridge::CloseRequested, Dialog, &QDialog::hide);
        }

        if (MaterialsDlg->isMinimized()) MaterialsDlg->showNormal();
        else                             MaterialsDlg->show();

        // Preserva a posição escolhida pelo usuário após a primeira abertura.
        if (!MaterialsDlg->property("smilePositioned").toBool()) {
            const QPoint Center = frameGeometry().center();
            MaterialsDlg->move(Center.x() - MaterialsDlg->width() / 2,
                               Center.y() - MaterialsDlg->height() / 2);
            MaterialsDlg->setProperty("smilePositioned", true);
        }
        MaterialsDlg->raise();
        MaterialsDlg->activateWindow();
    }

    void MainWindow::TriggerShaderCompileAndReload(const QString& _Path) {
        Smile::LogInfo("Alteracao Detectada no Shader: " + QFileInfo(_Path).fileName().toStdString());

        // Includes forçam reload completo; shaders preservam o stem até a conclusão assíncrona.
        const QFileInfo ShaderInfo(_Path);
        const bool IsInclude = ShaderInfo.suffix().compare("hlsli", Qt::CaseInsensitive) == 0;
        const std::string ChangedStem =
            IsInclude ? std::string() : ShaderInfo.completeBaseName().toStdString();

        // Cancela capturas antes da compilação para não misturar pipelines em uma sessão.
        if (Viewport && Viewport->GetRenderer()) {
            Viewport->GetRenderer()->NotifyShaderReloadQueued(ChangedStem);
        }

#ifdef SMILE_CMAKE_BINARY_DIR
        QString BuildDir = QStringLiteral(SMILE_CMAKE_BINARY_DIR);
#else
        QString BuildDir = QDir(QCoreApplication::applicationDirPath()).filePath("..");
#endif

        QProcess* CompileProcess = new QProcess(this);
#ifdef SMILE_BUILD_CONFIG
        const QString BuildConfig = QStringLiteral(SMILE_BUILD_CONFIG);
#elif defined(_DEBUG)
        const QString BuildConfig = QStringLiteral("Debug");
#else
        const QString BuildConfig = QStringLiteral("Release");
#endif
        QStringList Arguments = {
            "--build", BuildDir, "--config", BuildConfig, "--target", "Shaders"
        };

        Smile::LogInfo("Compilando Shader via CMake (" + BuildConfig.toStdString() + ")...");
        CompileProcess->start("cmake", Arguments);

        connect(CompileProcess, &QProcess::finished, this, [this, CompileProcess, ChangedStem](int _ExitCode, QProcess::ExitStatus _Status) {
            if (_Status == QProcess::NormalExit && _ExitCode == 0) {
                if (Viewport && Viewport->GetRenderer()) {
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
