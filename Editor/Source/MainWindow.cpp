#include "SmileEditor/MainWindow.h"
#include "SmileEditor/LogBridge.h"
#include "SmileEditor/LucideIcon.h"
#include "SmileEditor/MenuBridge.h"
#include "SmileEditor/NativeWindowFilter.h"
#include "SmileEditor/QmlHost.h"
#include "SmileEditor/SmileLogo.h"
#include "SmileEditor/SmileLogoImageProvider.h"
#include "SmileEditor/StatusBridge.h"
#include "SmileEditor/TimeOfDayBridge.h"
#include "SmileEditor/RenderSettingsBridge.h"
#include "SmileEditor/LightsBridge.h"
#include "SmileEditor/SceneOutlinerBridge.h"
#include "SmileEditor/SceneDocument.h"
#include "SmileEditor/CameraBookmarksBridge.h"
#include "SmileEditor/CaptureBridge.h"
#include "SmileEditor/McpBridge.h"
#include "SmileEditor/RenderSettingsController.h"
#include "SmileEditor/MaterialsBridge.h"
#include "SmileEditor/WindowBridge.h"
#include "SmileEditor/ViewportWidget.h"
#include "SmileEditor/DarkTheme.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/D3D12Device.h"

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

        // Precisa existir antes de CreateDocks (o ConsolePanel.qml liga nele) e do sink de log.
        ConsoleLog = new LogBridge(this);
        // Liga cedo: CreateTopBar/CreateViewportChrome/CreateDocks ja podem emitir logs e erros
        // QML. SetLogSink e quiescente, entao o destrutor consegue desligar isto com seguranca.
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

        WindowBr   = new WindowBridge(this); // botoes de janela da MainBar.qml
        Menus      = new MenuBridge(this);   // menus da MainBar.qml (precisa existir antes dela)
        TodBridge  = new TimeOfDayBridge(this); // painel Time of Day (renderer chega depois)
        LightsBr   = new LightsBridge(this);          // acoes/props de luz (renderer depois)
        OutlinerBr = new SceneOutlinerBridge(this);   // Scene Outliner (renderer depois)
        SceneDoc   = new SceneDocument(this);         // camada autorada (.smap)
        MaterialsBr = new MaterialsBridge(this);      // Editor de Materiais (renderer depois)
        CameraBookmarksBr = new CameraBookmarksBridge(this); // bookmarks de camera (renderer depois)
        CaptureBr  = new CaptureBridge(this);         // captura deterministica (renderer depois)
        RenderSettingsCtrl = new RenderSettingsController(this);
        McpBr      = new McpBridge(CaptureBr, CameraBookmarksBr, RenderSettingsCtrl, this);
        connect(McpBr, &McpBridge::ShutdownRequested, this, [this]() {
            // Benchmark automatizado nao pode parar num dialogo de sidecar. O fechamento ainda
            // passa pelo segundo estagio de closeEvent, que espera a render thread terminar.
            CloseApproved = true;
            close();
        });
        RenderBr   = new RenderSettingsBridge(this);  // knobs de render (renderer depois)

        // Mutacoes externas passam pelo controlador compartilhado. O lock e solto antes destes
        // sinais, entao os bindings podem reler o renderer sem reentrancia; a UI deixa de ficar
        // stale quando um preset foi aplicado pela named pipe em vez de por um clique QML.
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

        // Estrutura de luzes mudou (add/remover/duplicar/toggle/rename/cor) -> arvore refaz.
        connect(LightsBr, &LightsBridge::LightsChanged,
                OutlinerBr, &SceneOutlinerBridge::Rebuild);

        // Edicao pelo outliner (esconder/criar/apagar) suja a camada autorada. Move pelo gizmo
        // ainda nao acende este indicador — mas E salvo, porque o save le a cena viva inteira.
        connect(OutlinerBr, &SceneOutlinerBridge::DirtyChanged,
                SceneDoc, &SceneDocument::markDirty);

        // Isolar do Editor de Materiais mexe em FRenderable::Visible -> olhos do Outliner
        // refazem. "Selecionar na cena" revela o dock (a arvore segue a selecao sozinha).
        connect(MaterialsBr, &MaterialsBridge::VisibilityChanged,
                OutlinerBr, &SceneOutlinerBridge::Rebuild);
        connect(MaterialsBr, &MaterialsBridge::RevealInOutlinerRequested, this, [this]() {
            if (LightsDock) { LightsDock->show(); LightsDock->raise(); }
        });

        CreateTopBar();
        setCentralWidget(CreateViewportChrome());
        CreateDocks();
        WireMenuActions(); // conecta os menus depois que Viewport e ConsoleDock existem

        CreateStatusBar();

        // Bookmarks -> barra de status. Depois do CreateStatusBar porque o StatusBr nasce la.
        // Sem isto, uma falha de persistencia (disco cheio, arquivo somente-leitura, sidecar de
        // outra versao) ficava so no valor de retorno que a QML descarta, e gravar uma camera
        // parecia um clique sem efeito — o pior desfecho possivel, porque o usuario seguiria a
        // sessao de captura acreditando ter uma referencia que nao existe.
        if (CameraBookmarksBr && StatusBr) {
            connect(CameraBookmarksBr, &CameraBookmarksBridge::Message, this,
                    [this](const QString& _Text) { StatusBr->ShowMessage(_Text, 4000); });
        }
        // Captura -> barra de status, pelo mesmo argumento: o resultado sai N frames depois do
        // clique, e sem um aviso o operador nao tem como distinguir "ainda aquecendo" de "o
        // pedido nao entrou". O caminho do PNG tambem sai por aqui.
        if (CaptureBr && StatusBr) {
            connect(CaptureBr, &CaptureBridge::Message, this,
                    [this](const QString& _Text) { StatusBr->ShowMessage(_Text, 6000); });
        }

        connect(Viewport, &ViewportWidget::TelemetryUpdated,    this, &MainWindow::UpdateStats);
        connect(Viewport, &ViewportWidget::RendererInitialized, this, &MainWindow::OnRendererReady);
        // O oceano pode ser ligado tanto no Outliner quanto na janela de settings.
        // Propaga a invalidação nos dois sentidos para nenhum painel manter cache stale.
        connect(OutlinerBr, &SceneOutlinerBridge::EnvChanged,
                RenderBr, &RenderSettingsBridge::OceanSettingsChanged);
        connect(RenderBr, &RenderSettingsBridge::OceanSettingsChanged,
                OutlinerBr, &SceneOutlinerBridge::Refresh);
        // Splash: etapas do boot do renderer e rede de seguranca — se a render thread morrer
        // (falha de init ou parada precoce), a splash sai junto em vez de ficar por cima.
        connect(Viewport, &ViewportWidget::InitProgress, this,
                [this](const QString& _Label, const QString& _Detail, qreal _Fraction) {
            // Com cena de boot o renderer vale 90% da barra; os 10% finais sao o .sscene.
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

            // Diretorio mudou (arquivo novo/removido) -> adiciona os shaders novos ao watch.
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
        // Primeiro passo do teardown: espera callbacks em voo e impede que Renderer::Shutdown
        // alcance o LogBridge depois que os filhos Qt comecarem a ser destruidos.
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
        // Atualiza o icone maximizar/restaurar da MainBar quando o estado da janela muda
        // (botao, double-click no caption, Aero Snap).
        if (_Event->type() == QEvent::WindowStateChange && WindowBr)
            WindowBr->NotifyWindowStateChanged();
    }

    bool MainWindow::eventFilter(QObject* _Obj, QEvent* _Event) {
        // Cada viewport e sua toolbar apontam para o mesmo alvo. Foco ou clique em qualquer
        // superficie torna esse alvo o destinatario dos atalhos globais do viewport.
        if (_Event->type() == QEvent::FocusIn || _Event->type() == QEvent::MouseButtonPress) {
            const QVariant TargetProperty = _Obj->property("smileViewportTarget");
            if (QObject* TargetObject = TargetProperty.value<QObject*>()) {
                if (auto* TargetViewport = qobject_cast<ViewportWidget*>(TargetObject))
                    ActiveViewport = TargetViewport;
            }
        }

        // Reflete mostrar/esconder da janela TOD no check do menu "Janela" (cobre o X da
        // propria janela, que fecha via WindowBridge sem passar pelo toggle do menu).
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
            // A grade mora numa captura offscreen. Ocultar pausa o readback, mas preserva a
            // selecao para a janela voltar exatamente como o usuario a deixou.
            if (Viewport) Viewport->SetDebugPreviewEnabled(Shown);
        }
        if (MaterialsDlg && _Obj == MaterialsDlg &&
            (_Event->type() == QEvent::Show || _Event->type() == QEvent::Hide)) {
            const bool Shown = _Event->type() == QEvent::Show;
            if (Menus)       Menus->SetMaterialsVisible(Shown);
            if (MaterialsBr) MaterialsBr->SetPreviewEnabled(Shown); // preview so com a janela aberta
        }
        return QMainWindow::eventFilter(_Obj, _Event);
    }

    void MainWindow::closeEvent(QCloseEvent* _Event) {
        // Nunca destroi o HWND enquanto o worker ainda pode estar em Initialize/Present.
        // O primeiro close aprovado solicita a parada; Stopped agenda o segundo close.
        if (RendererShutdownForClose) {
            _Event->ignore();
            return;
        }
        if (CloseApproved) {
            ContinueApprovedClose(_Event);
            return;
        }

        // Sidecars com dirty flag (lights/visibility/materials) fechavam descartando em
        // silencio. Sao baratos de salvar — pergunta uma vez, com Salvar como default.
        const bool LightsDirty    = LightsBr    && LightsBr->Dirty();
        const bool VisDirty       = OutlinerBr  && OutlinerBr->Dirty();
        const bool MaterialsDirty = MaterialsBr && MaterialsBr->Dirty();
        if (!LightsDirty && !VisDirty && !MaterialsDirty) {
            CloseApproved = true;
            ContinueApprovedClose(_Event);
            return;
        }

        QStringList Pending;
        if (LightsDirty)    Pending << tr("luzes");
        if (VisDirty)       Pending << tr("visibilidade");
        if (MaterialsDirty) Pending << tr("materiais");

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
            if (LightsDirty)    LightsBr->saveLights();
            if (VisDirty)       OutlinerBr->saveVisibility();
            if (MaterialsDirty) MaterialsBr->saveMaterials();
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
        // Barra unificada em QML: windowBridge (min/max/fechar + zonas do hit-test), menuBridge
        // (menus) e smilelogo (logo procedural da engine como image provider).
        QQuickWidget* Bar = CreateQmlPanel(*SharedQmlEngine,
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

        // Hospeda no slot do QStatusBar (sempre no rodape, abaixo dos docks). addWidget com
        // stretch=1 ocupa a largura toda; nao usamos mais showMessage nativo.
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

        using Result = Smile::FPreparedCookedScenePtr;
        auto* Watcher = new QFutureWatcher<Result>(this);
        connect(Watcher, &QFutureWatcher<Result>::finished, this,
                [this, Watcher, Path = _Path, Additive = _Additive]() {
            Result Prepared = Watcher->result();
            Watcher->deleteLater();
            if (CloseApproved || RendererShutdownForClose) {
                SceneLoadInProgress = false;
                FinishBootStage();
                return;
            }
            if (!Prepared) {
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
            const bool Queued = Viewport && Viewport->CommitPreparedSceneAsync(
                std::move(Prepared), Additive,
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
                    // ANTES do outliner: o .smap pode criar e apagar objetos, e a arvore tem de
                    // ser construida sobre a lista ja editada, nao sobre a do asset cru.
                    if (SceneDoc)    SceneDoc->OnSceneLoaded(Path, Additive);
                    if (OutlinerBr)  OutlinerBr->OnSceneLoaded(Path, Additive);
                    if (MaterialsBr) MaterialsBr->OnSceneLoaded(Path, Additive);
                    if (CameraBookmarksBr) CameraBookmarksBr->OnSceneLoaded(Path, Additive);
                    if (CaptureBr)   CaptureBr->OnSceneLoaded(Path, Additive);
                    if (McpBr)       McpBr->OnSceneLoaded(Path, Additive);
                    if (Viewport)    Viewport->NotifyDebugTargetsChanged();
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
                return Smile::Renderer::PrepareCookedScene(ScenePath);
            }));
    }

    void MainWindow::WireMenuActions() {
        // Renderer pronto (ou nullptr). As acoes de cena/render so valem com a engine inicializada.
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

        // Del / Ctrl+D vindos do viewport (ver os sinais no ViewportWidget.h). Repetem o
        // despacho do SceneOutlinerPanel.qml em vez de generaliza-lo porque as duas selecoes
        // sao mutuamente exclusivas: no maximo um dos ramos existe em cada momento.
        connect(Viewport, &ViewportWidget::DeleteSelectionRequested, this, [this] {
            if (OutlinerBr && OutlinerBr->MeshSelected()) { OutlinerBr->deleteSelectedMesh(); return; }
            if (LightsBr && LightsBr->SelectedIndex() >= 0) LightsBr->removeLight(LightsBr->SelectedIndex());
        });
        connect(Viewport, &ViewportWidget::DuplicateSelectionRequested, this, [this] {
            if (OutlinerBr && OutlinerBr->MeshSelected()) { OutlinerBr->duplicateSelectedMesh(); return; }
            if (LightsBr && LightsBr->SelectedIndex() >= 0) LightsBr->duplicateLight(LightsBr->SelectedIndex());
        });

        // ---- Atalhos globais (ApplicationShortcut: valem com o foco no viewport, nao so na barra).
        // Disparam o mesmo caminho dos menus (chamam o MenuBridge -> sinal -> handler acima).
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
            // Deixa o callback Stopped retornar antes de destruir a arvore QWidget.
            QTimer::singleShot(0, this, [this]() { close(); });
        });

        const QVariant Target = QVariant::fromValue(static_cast<QObject*>(_Viewport));
        _Viewport->setProperty("smileViewportTarget", Target);
        _Viewport->installEventFilter(this);

        if (_Toolbar) {
            _Toolbar->setProperty("smileViewportTarget", Target);
            _Toolbar->installEventFilter(this);
        }

        if (!ActiveViewport) ActiveViewport = _Viewport;
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

        // Console em QML (ConsolePanel.qml), alimentado pela propriedade raiz logModel.
        QQuickWidget* Console = CreateQmlPanel(*SharedQmlEngine,
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
        // Drawer compacto por padrao, mas ainda redimensionavel pelo usuario. Com 82 px cabem
        // a faixa de comando e duas linhas; a abertura de 124 px mostra quatro eventos inteiros.
        Console->setMinimumHeight(82);
        // resizeDocks e a API confiavel pra altura inicial (resize() no dock e ignorado pelo layout).
        resizeDocks({ ConsoleDock }, { 124 }, Qt::Vertical);

        // Reflete o estado do dock no check do menu "Janela" (inclui o fechar via menu do console).
        connect(ConsoleDock, &QDockWidget::visibilityChanged, Menus, &MenuBridge::SetConsoleVisible);

        // O Time of Day virou janela flutuante (ShowTimeOfDay) — nao ha mais dock dele.

        // ---- Cena / Scene Outliner (dock lateral direito, ex-painel de Luzes) ----
        LightsDock = new QDockWidget(tr("Cena"), this);
        LightsDock->setObjectName("SceneOutlinerDock");
        LightsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        LightsDock->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable |
                                QDockWidget::DockWidgetClosable);

        QQuickWidget* OutlinerPanel = CreateQmlPanel(*SharedQmlEngine,
            QStringLiteral("SceneOutlinerPanel.qml"),
            { { QStringLiteral("outlinerModel"),  OutlinerBr },
              { QStringLiteral("sceneDoc"),        SceneDoc },
              { QStringLiteral("lightsModel"),    LightsBr },
              { QStringLiteral("renderModel"),    RenderBr },
              { QStringLiteral("viewportModel"),  Viewport } },
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

        // Time-of-Day: textura da lua (LROC color da NASA). Path resolvido via SMILE_ASSETS_DIR
        // (so o editor o conhece); em falha o renderer segue na lua procedural branca.
        Viewport->GetRenderer()->LoadMoonTexture(
            QString(SMILE_ASSETS_DIR "/Textures/Sky/moon_lroc_color_2k.jpg").toStdWString());

        // Catalogo de estrelas real (Yale BSC cozido pelo Tools/CookStars.py); sem o asset o
        // renderer segue no hash procedural.
        Viewport->GetRenderer()->LoadStarCatalog(
            QString(SMILE_ASSETS_DIR "/Sky/stars.sstars").toStdWString());

        // Knobs de render: so precisa do handle. Sem Refresh por frame de proposito — knob nao
        // muda sozinho, so por acao do usuario, e cada setter ja emite o sinal do dominio dele.
        if (RenderBr) {
            RenderBr->SetRenderer(Viewport->GetRenderer());
            RenderBr->SetViewport(Viewport);
        }

        // Bookmarks de camera: so precisa do handle. Sem Refresh por frame — a pose gravada so
        // muda por clique, e ler a camera todo frame nao diria nada sobre os SLOTS.
        if (CameraBookmarksBr) CameraBookmarksBr->SetRenderer(Viewport->GetRenderer());
        // Captura: idem, so o handle. O progresso e puxado por Timer do QML enquanto o card
        // estiver visivel — a sessao roda na render thread e a GUI so pergunta.
        if (CaptureBr) CaptureBr->SetRenderer(Viewport->GetRenderer());
        if (RenderSettingsCtrl) RenderSettingsCtrl->SetViewport(Viewport);

        // Painel TOD: liga a bridge no renderer e passa a atualizar o relogio por frame.
        if (TodBridge) {
            TodBridge->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    TodBridge, &TimeOfDayBridge::Refresh, Qt::UniqueConnection);
        }

        // Painel de Luzes: idem — o Refresh por frame sincroniza selecao por clique no
        // viewport e o arraste do gizmo com o painel.
        if (LightsBr) {
            LightsBr->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    LightsBr, &LightsBridge::Refresh, Qt::UniqueConnection);
        }

        // Scene Outliner: o Refresh por frame sincroniza picking do viewport, contagens e
        // toggles de ambiente com a arvore.
        if (OutlinerBr) {
            OutlinerBr->SetRenderer(Viewport->GetRenderer());
            SceneDoc->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    OutlinerBr, &SceneOutlinerBridge::Refresh, Qt::UniqueConnection);
        }

        // Editor de Materiais: o Refresh por frame segue o picking (clicar numa mesh
        // seleciona o material dela no browser, estilo "pick from scene" da Cry).
        if (MaterialsBr) {
            MaterialsBr->SetRenderer(Viewport->GetRenderer());
            connect(Viewport, &ViewportWidget::FrameReady,
                    MaterialsBr, &MaterialsBridge::Refresh, Qt::UniqueConnection);
        }

        if (!StartupScenePath.isEmpty()) {
            // A splash so sai quando a cena de boot terminar: fechar antes mostraria um
            // viewport vazio por segundos. BeginSceneLoad chama FinishBootStage em todo
            // desfecho, inclusive nos de falha.
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
        if (!Viewport || !StatusBr) return;
        auto Renderer = Viewport->GetRenderer();
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) return;

        // ~5Hz basta: formatar QString + relayout da StatusBar a 100+Hz era so custo —
        // ninguem le FPS piscando por frame.
        const float FPS = Viewport->GetFPS();
        const float FrameMs = FPS > 0.0f ? 1000.0f / FPS : 0.0f;

        // Textos SEM separador inicial — a StatusBar.qml os junta com "  |  ".
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

        // Centraliza apenas quando a janela ainda nao ganhou uma posicao util do window manager.
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
            // Mesmo padrao do SettingsWindow: QDialog frameless nao-modal com chrome 100% QML
            // (arrasto/minimizar/fechar via WindowBridge).
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

            Dialog->installEventFilter(this); // Show/Hide -> check do menu "Janela"
            TodDlg = Dialog;
        }

        if (TodDlg->isMinimized()) TodDlg->showNormal();
        else                       TodDlg->show();

        // Centraliza apenas quando a janela ainda nao ganhou uma posicao util do window manager.
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
            // Mesmo padrao do TimeOfDayWindow: QDialog frameless nao-modal com chrome QML.
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
                { { QStringLiteral("viewportModel"), Viewport },
                  { QStringLiteral("statsWindow"), StatsWindowBridge } },
                Dialog);
            Panel->setObjectName(QStringLiteral("StatsWindowPanel"));

            auto* DialogLayout = new QVBoxLayout(Dialog);
            DialogLayout->setContentsMargins(0, 0, 0, 0);
            DialogLayout->setSpacing(0);
            DialogLayout->addWidget(Panel);

            Dialog->installEventFilter(this); // Show/Hide -> check do menu "Janela"
            StatsDlg = Dialog;
        }

        if (StatsDlg->isMinimized()) StatsDlg->showNormal();
        else                         StatsDlg->show();

        // Centraliza apenas quando a janela ainda nao ganhou uma posicao util do window manager.
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
            // Mesmo padrao do StatsWindow: QDialog frameless nao-modal com chrome QML.
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
                { { QStringLiteral("viewportModel"), Viewport },
                  { QStringLiteral("debugWindow"), DebugWindowBridge } },
                Dialog,
                { { QStringLiteral("debugtargetpreview"),
                    new DebugTargetPreviewImageProvider(Viewport) } });
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
            // Mesmo padrao do TimeOfDayWindow/StatsWindow: QDialog frameless com chrome QML.
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

            Dialog->installEventFilter(this); // Show/Hide -> check do menu "Janela"
            MaterialsDlg = Dialog;

            connect(MaterialsBr, &MaterialsBridge::CloseRequested, Dialog, &QDialog::hide);
        }

        if (MaterialsDlg->isMinimized()) MaterialsDlg->showNormal();
        else                             MaterialsDlg->show();

        // Centraliza apenas quando a janela ainda nao ganhou uma posicao util do window manager.
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

        // O stem sai AQUI, na deteccao, e nao la no fim da compilacao — e o mesmo valor viaja para
        // as duas chamadas, entao nao ha como as duas derivarem coisas diferentes.
        //
        // .hlsli (include) afeta varios shaders -> stem vazio forca reload completo. Caso
        // contrario, deriva o stem do .cso: "WaterSurface.ps.hlsl" -> "WaterSurface.ps".
        const QFileInfo ShaderInfo(_Path);
        const bool IsInclude = ShaderInfo.suffix().compare("hlsli", Qt::CaseInsensitive) == 0;
        const std::string ChangedStem =
            IsInclude ? std::string() : ShaderInfo.completeBaseName().toStdString();

        // ANTES de disparar o compilador. A compilacao leva ~1,5 s e o renderer nao para durante
        // ela: uma captura de 128 frames cabe inteira nessa janela e sairia gravada como valida,
        // com parte dos frames feita com o .cso antigo. Medido — a captura foi gravada 1,4 s antes
        // de o ReloadShaders sequer ser chamado. Quem decide se ha o que cancelar e o renderer,
        // que e quem conhece o mapeamento de stems.
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
                    // O stem foi derivado na deteccao e capturado por valor: re-deriva-lo aqui
                    // abriria a porta para o cancelamento e o reload discordarem sobre qual
                    // pipeline mudou.
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
