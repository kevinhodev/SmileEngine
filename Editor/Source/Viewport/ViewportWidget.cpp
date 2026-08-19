#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Core/Logger.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QGuiApplication>
#include <QCursor>
#include <QMetaObject>
#include <QPointer>
#include <QPalette>
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    static constexpr float kMouseSensitivity = 0.15f;  
    static constexpr int   kResizeDebounceMs = 80;
    ViewportWidget::ViewportWidget(QWidget* _Parent)
        : QWidget(_Parent),
          Renderer(RendererThread.GetRenderer())
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        // Evita o flash branco do HWND antes do primeiro Present.
        setAttribute(Qt::WA_NoSystemBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        QPalette InitialPalette = palette();
        InitialPalette.setColor(QPalette::Window, Qt::black);
        setPalette(InitialPalette);
        setAutoFillBackground(true);

        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 200);
        setMouseTracking(true);

        // Present controla o pacing; o timer apenas agenda quando o event loop fica livre.
        RedrawTimer = new QTimer(this);
        RedrawTimer->setInterval(0);
        RedrawTimer->setSingleShot(true);
        connect(RedrawTimer, &QTimer::timeout, this, &ViewportWidget::OnRenderTimer);

        // Aguarda o primeiro layout estabilizar antes de criar a swapchain.
        InitializationDebounce = new QTimer(this);
        InitializationDebounce->setSingleShot(true);
        InitializationDebounce->setInterval(kResizeDebounceMs);
        connect(InitializationDebounce, &QTimer::timeout,
                this, &ViewportWidget::EnsureRendererIsInitialized);

        // Coalesce resizes que não passam por WM_ENTERSIZEMOVE.
        ResizeDebounce = new QTimer(this);
        ResizeDebounce->setSingleShot(true);
        ResizeDebounce->setInterval(kResizeDebounceMs);
        connect(ResizeDebounce, &QTimer::timeout, this, &ViewportWidget::ApplyPendingResize);

        // Limita o editor em segundo plano a aproximadamente 10 FPS.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState State) {
            RedrawTimer->setInterval(
                BackgroundThrottleEnabled && State != Qt::ApplicationActive ? 100 : 0);
        });

        FrameTimer.start();
    }

    ViewportWidget::~ViewportWidget() {
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        // O fluxo normal já espera RendererStopped; isto cobre teardown parcial.
        RendererThread.RequestStop();
        RendererThread.Join();
    }

    void ViewportWidget::BeginRendererShutdown() {
        if (RendererShutdownRequested) return;
        RendererShutdownRequested = true;
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        RendererJobs.clear();
        RendererThread.RequestStop();
        if (RendererThread.IsStopped()) OnRenderThreadStopped();
    }

    QPaintEngine* ViewportWidget::paintEngine() const {
        return nullptr;
    }

    QString ViewportWidget::GetViewModeLabel() const {
        switch (CurrentViewMode) {
        case ReflectionHeatmap:
            return QStringLiteral("Heatmap");
        case Lit:
        default:
            return QStringLiteral("Lit");
        }
    }

    void ViewportWidget::SetBackgroundThrottleEnabled(bool _Enabled) {
        BackgroundThrottleEnabled = _Enabled;
        if (!RedrawTimer) return;
        const bool InBackground =
            QGuiApplication::applicationState() != Qt::ApplicationActive;
        RedrawTimer->setInterval(_Enabled && InBackground ? 100 : 0);
    }


    void ViewportWidget::SetGizmoMode(int _Mode) {
        if (_Mode < 0 || _Mode > static_cast<int>(GizmoController::EMode::Scale)) return;
        const auto Next = static_cast<GizmoController::EMode>(_Mode);
        if (GizmoCtrl.GetMode() == Next) return;
        GizmoCtrl.SetMode(Next);
        if (GizmoCtrl.GetMode() != Next) return;
        emit GizmoModeChanged();
        // O modo pode alterar a restrição de espaço exibida pela UI.
        emit GizmoSpaceChanged();
    }

    void ViewportWidget::SetGizmoSpace(int _Space) {
        if (_Space < 0 || _Space > static_cast<int>(GizmoController::ESpace::Local)) return;
        const auto Next = static_cast<GizmoController::ESpace>(_Space);
        if (GizmoCtrl.GetSpace() == Next) return;
        GizmoCtrl.SetSpace(Next);
        if (GizmoCtrl.GetSpace() != Next) return;
        emit GizmoSpaceChanged();
    }

    void ViewportWidget::ToggleGizmoSpace() {
        SetGizmoSpace(GizmoCtrl.GetSpace() == GizmoController::ESpace::World
                          ? static_cast<int>(GizmoController::ESpace::Local)
                          : static_cast<int>(GizmoController::ESpace::World));
    }

    void ViewportWidget::ToggleSnap(int _Mode) {
        switch (_Mode) {
            case 1: GizmoCtrl.SetSnapTranslateOn(!GizmoCtrl.Snap().TranslateOn); break;
            case 2: GizmoCtrl.SetSnapRotateOn(!GizmoCtrl.Snap().RotateOn);       break;
            case 3: GizmoCtrl.SetSnapScaleOn(!GizmoCtrl.Snap().ScaleOn);         break;
            default: return;
        }
        emit GizmoSnapChanged();
    }

    void ViewportWidget::SetSnapValue(int _Mode, double _Value) {
        const float V = static_cast<float>(_Value);
        if (!(V > 0.0f)) return;
        switch (_Mode) {
            case 1: GizmoCtrl.SetSnapTranslateM(V); break;
            case 2: GizmoCtrl.SetSnapRotateDeg(V);  break;
            case 3: GizmoCtrl.SetSnapScaleStep(V);  break;
            default: return;
        }
        emit GizmoSnapChanged();
    }

    void ViewportWidget::SelectLit() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetFlickerMode(0);
        // O alvo de debug tem prioridade sobre o caminho normal.
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        CurrentViewMode = Lit;
        emit ViewStateChanged();
    }

    void ViewportWidget::SelectReflectionHeatmap() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        RendererAccess->SetFlickerMode(2);
        CurrentViewMode = ReflectionHeatmap;
        emit ViewStateChanged();
    }


    void ViewportWidget::RequestSettings() {
        emit SettingsRequested();
    }

    void ViewportWidget::EnsureRendererIsInitialized() {
        if (RendererShutdownRequested || RendererStoppedFlag || RendererThread.IsStarted()) return;
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        // Limpa o HWND antes de transferir a superfície ao D3D.
        RECT ClientRect{};
        if (GetClientRect(hWnd, &ClientRect)) {
            if (HDC DeviceContext = GetDC(hWnd)) {
                FillRect(DeviceContext, &ClientRect,
                         static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                ReleaseDC(hWnd, DeviceContext);
            }
        }
        const QSize InitialSize = size();
        AppliedResizeSize = InitialSize;

        QPointer<ViewportWidget> Self(this);
        RenderThread::Callbacks Hooks;
        Hooks.Initialized = [Self]() {
            if (!Self) return;
            QMetaObject::invokeMethod(Self, [Self]() {
                if (Self) Self->OnRendererInitialized();
            }, Qt::QueuedConnection);
        };
        Hooks.FrameCompleted = [Self](RenderThread::FrameCompletion Completion) {
            if (!Self) return;
            const QString Error = QString::fromUtf8(Completion.Error);
            QMetaObject::invokeMethod(Self,
                [Self, Success = Completion.Success,
                 Terminal = Completion.Terminal, Error]() {
                if (Self) Self->OnFrameCompleted(Success, Terminal, Error);
            }, Qt::QueuedConnection);
        };
        Hooks.Progress = [Self](const std::string& Label, const std::string& Detail,
                                float Fraction) {
            if (!Self) return;
            const QString LabelText  = QString::fromUtf8(Label);
            const QString DetailText = QString::fromUtf8(Detail);
            QMetaObject::invokeMethod(Self, [Self, LabelText, DetailText, Fraction]() {
                if (Self) emit Self->InitProgress(LabelText, DetailText,
                                                  static_cast<qreal>(Fraction));
            }, Qt::QueuedConnection);
        };
        Hooks.InitializationFailed = [Self](const std::string& Error) {
            if (!Self) return;
            const QString Message = QString::fromUtf8(Error);
            QMetaObject::invokeMethod(Self, [Self, Message]() {
                if (Self) Self->OnRendererInitializationFailed(Message);
            }, Qt::QueuedConnection);
        };
        Hooks.Stopped = [Self]() {
            if (!Self) return;
            QMetaObject::invokeMethod(Self, [Self]() {
                if (Self) Self->OnRenderThreadStopped();
            }, Qt::QueuedConnection);
        };
        RendererThread.Start(hWnd,
                             static_cast<unsigned int>(InitialSize.width()),
                             static_cast<unsigned int>(InitialSize.height()),
                             std::move(Hooks));
    }

    void ViewportWidget::OnRendererInitialized() {
        if (RendererShutdownRequested || Initialized || !RendererThread.IsReady()) return;
        Initialized = true;
        PendingResizeSize = size();
        ApplyPendingResize();
        FrameTimer.restart();
        emit RendererInitialized();
        emit RendererResourcesChanged();
        if (isVisible() && RedrawTimer) RedrawTimer->start();
    }

    void ViewportWidget::OnRendererInitializationFailed(const QString& _Error) {
        Q_UNUSED(_Error);
        Initialized = false;
        if (RedrawTimer) RedrawTimer->stop();
    }

    bool ViewportWidget::CommitImportedSceneAsync(
            std::shared_ptr<Smile::FSceneImportResult> _Imported,
            bool _Additive,
            SceneCommitCallback _Completion) {
        if (!_Imported) return false;
        return EnqueueRendererJob(
            {},
            [Imported = std::move(_Imported), _Additive](Smile::Renderer& _Renderer) mutable {
                RenderThread::JobCompletion Result;
                Result.Success = _Renderer.CommitCookedScene(
                    std::move(Imported), _Additive);
                if (!Result.Success)
                    Result.Error = "CommitCookedScene retornou false";
                return Result;
            },
            std::move(_Completion));
    }

    bool ViewportWidget::EnqueueRendererJob(
            const QString& _CoalesceKey,
            RenderThread::RendererJob _Job,
            SceneCommitCallback _Completion) {
        if (!_Job || RendererShutdownRequested || !RendererThread.IsReady()) return false;

        // Conserva somente o último job ainda não iniciado de cada família.
        if (!_CoalesceKey.isEmpty()) {
            for (auto It = RendererJobs.rbegin(); It != RendererJobs.rend(); ++It) {
                if (It->CoalesceKey == _CoalesceKey) {
                    It->Execute = std::move(_Job);
                    It->Completion = std::move(_Completion);
                    return true;
                }
            }
        }

        RendererJobs.push_back(FQueuedRendererJob{
            _CoalesceKey, std::move(_Job), std::move(_Completion) });
        DispatchNextRendererJob();
        return true;
    }

    void ViewportWidget::DispatchNextRendererJob() {
        if (RendererJobActive || RendererShutdownRequested || RendererJobs.empty()) return;

        FQueuedRendererJob Job = std::move(RendererJobs.front());
        RendererJobs.pop_front();
        if (RedrawTimer) RedrawTimer->stop();
        RendererJobActive = true;

        QPointer<ViewportWidget> Self(this);
        SceneCommitCallback Completion = std::move(Job.Completion);
        SceneCommitCallback FailureCompletion = Completion;
        const bool Queued = RendererThread.RequestRendererJob(
            std::move(Job.Execute),
            [Self, Completion = std::move(Completion)](
                    RenderThread::JobCompletion _Result) mutable {
                if (!Self) return;
                const bool Success = _Result.Success;
                const QString Error = QString::fromStdString(_Result.Error);
                QMetaObject::invokeMethod(Self,
                    [Self, Success, Error, Completion = std::move(Completion)]() mutable {
                        if (!Self) return;

                        // Publica o resultado antes de liberar o próximo job ou frame.
                        if (!Self->RendererShutdownRequested && Completion)
                            Completion(Success, Error);
                        Self->RendererThread.CompleteJob();
                        Self->RendererJobActive = false;
                        Self->DispatchNextRendererJob();

                        if (Self->PendingResizeSize.isValid() &&
                            Self->PendingResizeSize != Self->AppliedResizeSize &&
                            Self->ResizeDebounce && !Self->InteractiveResize)
                            Self->ResizeDebounce->start(0);

                        if (!Self->RendererShutdownRequested &&
                            Self->RendererJobs.empty() && Self->isVisible() &&
                            Self->RedrawTimer)
                            Self->RedrawTimer->start();
                    }, Qt::QueuedConnection);
            });

        if (!Queued) {
            RendererJobActive = false;
            if (!RendererShutdownRequested && FailureCompletion)
                FailureCompletion(false, QStringLiteral("render thread indisponivel"));
            DispatchNextRendererJob();
            if (RendererJobs.empty() && isVisible() && RedrawTimer) RedrawTimer->start(1);
        }
    }

    void ViewportWidget::OnRenderThreadStopped() {
        if (RendererStoppedFlag) return;
        RendererThread.Join();
        RendererStoppedFlag = true;
        Initialized = false;
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        emit RendererStopped();
    }

    void ViewportWidget::showEvent(QShowEvent* _Event) {
        QWidget::showEvent(_Event);
        // Evita criar e redimensionar a swapchain durante o primeiro layout.
        PendingResizeSize = size();
        if (!RendererThread.IsStarted()) {
            InitializationDebounce->start();
        } else {
            ApplyPendingResize();
        }
        FrameTimer.restart();
        if (Initialized && !RendererStoppedFlag && !RendererThread.HasFrameInFlight() &&
            !RendererThread.HasJobInFlight())
            RedrawTimer->start();
    }

    void ViewportWidget::hideEvent(QHideEvent* _Event) {
        RedrawTimer->stop();
        InitializationDebounce->stop();
        ResizeDebounce->stop();
        QWidget::hideEvent(_Event);
    }

    void ViewportWidget::resizeEvent(QResizeEvent* _Event) {
        QWidget::resizeEvent(_Event);
        PendingResizeSize = _Event->size();
        if (!Initialized) {
            if (!RendererThread.IsStarted()) InitializationDebounce->start();
            return;
        }
        if (InteractiveResize) return;
        ResizeDebounce->start();
    }

    void ViewportWidget::BeginInteractiveResize() {
        InteractiveResize = true;
        ResizeDebounce->stop();
        PendingResizeSize = size();
    }

    void ViewportWidget::EndInteractiveResize() {
        if (!InteractiveResize) return;
        InteractiveResize = false;
        PendingResizeSize = size();
        // ResizeBuffers roda depois de sair do callback nativo.
        ResizeDebounce->start(0);
    }

    void ViewportWidget::ApplyPendingResize() {
        if (!Initialized || InteractiveResize || !Renderer) return;

        if (RendererThread.HasFrameInFlight() || RendererThread.HasJobInFlight()) return;
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess) {
            // Mantém a GUI responsiva enquanto Present possui o Renderer.
            ResizeDebounce->start(1);
            return;
        }
        if (!RendererAccess->IsInitialized()) return;

        const QSize Target = PendingResizeSize;
        PendingResizeSize = {};
        if (Target.width() <= 0 || Target.height() <= 0 || Target == AppliedResizeSize) return;

        RendererAccess->Resize(static_cast<unsigned int>(Target.width()),
                               static_cast<unsigned int>(Target.height()));
        AppliedResizeSize = Target;
        emit RendererResourcesChanged();
    }

    void ViewportWidget::paintEvent(QPaintEvent* _Event) {
        Q_UNUSED(_Event);
    }

    void ViewportWidget::OnRenderTimer() {
        if (RendererShutdownRequested) return;
        EnsureRendererIsInitialized();
        if (!Initialized || !RendererThread.IsReady() ||
            RendererThread.HasFrameInFlight() || RendererThread.HasJobInFlight()) return;

        // O tick é coalescido quando outro acesso sincronizado possui o Renderer.
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) {
            if (isVisible() && RedrawTimer) RedrawTimer->start(1);
            return;
        }

        // Precisão sub-ms mantém o delta estável em frame rates altos.
        float DeltaTime = static_cast<float>(static_cast<double>(FrameTimer.nsecsElapsed()) / 1.0e9);
        FrameTimer.restart();
        DeltaTime = Smile::Clamp(DeltaTime, 0.0001f, 0.1f);
        LastFrameDeltaTime = DeltaTime;

        Smile::CameraInput CameraInput;
        CameraInput.Look  = MouseLookActive
            ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                          -MouseDelta.Y * kMouseSensitivity }   
            : Smile::Vec2::Zero();
        // Novos eventos continuam acumulando enquanto o frame está em execução.
        MouseDelta = Smile::Vec2::Zero();
        // Movimento de câmera exige mouse-look ativo para não conflitar com atalhos do gizmo.
        CameraInput.Move  = MouseLookActive
            ? Smile::Vec3{
                  static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),
                  static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),
                  static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),
              }
            : Smile::Vec3::Zero();
        CameraInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

        RendererAccess->UpdateCamera(CameraInput, DeltaTime);
        FlushPendingGizmoInput(*RendererAccess);
        // Submit resolve a restrição dependente da seleção e publica os handles deste frame.
        const int RestrictionBefore = GetGizmoSpaceRestriction();
        GizmoCtrl.Submit(*RendererAccess);
        if (GetGizmoSpaceRestriction() != RestrictionBefore) emit GizmoSpaceChanged();
        if (RendererThread.RequestFrame()) {
            if (!RendererOwnsSurface) {
                RendererOwnsSurface = true;
                setAutoFillBackground(false);
                setAttribute(Qt::WA_NoSystemBackground, true);
                setAttribute(Qt::WA_OpaquePaintEvent, true);
            }
            // O callback do frame rearma o timer single-shot.
        }
    }

    void ViewportWidget::OnFrameCompleted(bool _Success, bool _Terminal,
                                          const QString& _Error) {
        if (!_Success && !_Error.isEmpty() && _Terminal)
            Smile::LogError("Render thread encerrada apos falhas consecutivas: " +
                            _Error.toStdString());

        if (_Success && !RendererShutdownRequested) OnFrameRendered();

        // Toda conclusão libera FrameOutstanding neste ponto único.
        RendererThread.CompleteFrame();
        if (RendererShutdownRequested || _Terminal) {
            if (RedrawTimer) RedrawTimer->stop();
            return;
        }
        if (RendererThread.HasJobInFlight()) return;
        if (PendingResizeSize.isValid() && PendingResizeSize != AppliedResizeSize &&
            ResizeDebounce && !InteractiveResize)
            ResizeDebounce->start(0);
        if (isVisible() && RedrawTimer) RedrawTimer->start();
    }

    void ViewportWidget::OnFrameRendered() {
        // FrameReady observa um snapshot consistente antes de o próximo frame ser solicitado.
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) return;

        // Consome o resultado assíncrono do ID-buffer.
        int PickedIndex = -1;
        if (Renderer->TryGetPickResult(PickedIndex)) {
            Renderer->ClearLightSelection();
            if (PickedIndex >= 0) {
                Renderer->SetSelectedObject(PickedIndex);
                const auto& Renderables = Renderer->GetScene().Renderables();
                if (PickedIndex < static_cast<int>(Renderables.size())) {
                    Smile::LogDebug("Selecionado [" + std::to_string(PickedIndex) + "] " +
                                   Renderables[static_cast<size_t>(PickedIndex)].Name);
                }
            } else {
                Renderer->ClearSelection();
                Smile::LogDebug("Selecao limpa (clique no vazio)");
            }
            emit ObjectSelected(PickedIndex);
        }

        // EMA reduz a oscilação do valor exibido.
        const float InstFPS = LastFrameDeltaTime > 0.0f ? 1.0f / LastFrameDeltaTime : 0.0f;
        LastFPS = (LastFPS > 0.0f) ? (LastFPS * 0.96f + InstFPS * 0.04f) : InstFPS;

        emit FrameReady();
    }

    void ViewportWidget::keyPressEvent(QKeyEvent* _Event) {
        // Ações discretas ignoram autorepeat; HeldKeys permanece exclusivo do voo da câmera.
        if (!_Event->isAutoRepeat()) {
            if (_Event->key() == Qt::Key_Delete) {
                emit DeleteSelectionRequested();
                _Event->accept();
                return;
            }
            if (_Event->key() == Qt::Key_D && (_Event->modifiers() & Qt::ControlModifier)) {
                emit DuplicateSelectionRequested();
                _Event->accept();
                return;
            }
            if (_Event->key() == Qt::Key_QuoteLeft &&
                (_Event->modifiers() & Qt::ControlModifier)) {
                ToggleGizmoSpace();
                _Event->accept();
                return;
            }
            // Q/W/E/R trocam a ferramenta quando mouse-look e modificadores estão inativos.
            if (_Event->modifiers() == Qt::NoModifier && !MouseLookActive) {
                switch (_Event->key()) {
                    case Qt::Key_Q: SetGizmoMode(0); break;
                    case Qt::Key_W: SetGizmoMode(1); break;
                    case Qt::Key_E: SetGizmoMode(2); break;
                    case Qt::Key_R: SetGizmoMode(3); break;
                    default: break;
                }
            }
        }
        if (!_Event->isAutoRepeat())
            HeldKeys.insert(_Event->key());
        QWidget::keyPressEvent(_Event);
    }

    void ViewportWidget::keyReleaseEvent(QKeyEvent* _Event) {
        if (!_Event->isAutoRepeat())
            HeldKeys.remove(_Event->key());
        QWidget::keyReleaseEvent(_Event);
    }

    void ViewportWidget::FlushPendingGizmoInput(Smile::Renderer& _Renderer) {
        if (GizmoMousePending) {
            GizmoCtrl.SetSnapInverted(PendingGizmoSnapInverted);
            GizmoCtrl.OnMouseMove(_Renderer, PendingGizmoMouseX, PendingGizmoMouseY);
            GizmoMousePending = false;
        }
        if (GizmoReleasePending) {
            const bool Edited = GizmoCtrl.OnMouseRelease(_Renderer);
            GizmoReleasePending = false;
            if (Edited) emit SceneEdited();
        }
    }

    void ViewportWidget::mousePressEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::RightButton) {
            MouseLookActive = true;
            IgnoreNextMove  = false;
            setCursor(Qt::BlankCursor);
            QCursor::setPos(mapToGlobal(rect().center()));
            IgnoreNextMove = true;
            setFocus();
        }
        // Coordenadas lógicas mapeiam 1:1 para o ID-buffer da viewport.
        else if (_Event->button() == Qt::LeftButton && !MouseLookActive) {
            if (Renderer) {
                auto RendererAccess = Renderer.Lock();
                if (!RendererAccess || !RendererAccess->IsInitialized()) {
                    QWidget::mousePressEvent(_Event);
                    return;
                }
                const QPointF P = _Event->position();
                const unsigned int Px = static_cast<unsigned int>(P.x() > 0.0 ? P.x() : 0.0);
                const unsigned int Py = static_cast<unsigned int>(P.y() > 0.0 ? P.y() : 0.0);
                FlushPendingGizmoInput(*RendererAccess);
                // Ordem de picking: handle, ícone de luz e ID-buffer.
                if (!GizmoCtrl.OnMousePress(*RendererAccess, Px, Py)) {
                    const int LightHit = GizmoCtrl.PickLightIcon(*RendererAccess, Px, Py);
                    if (LightHit >= 0) {
                        Renderer->SetSelectedLight(LightHit);
                        Renderer->ClearSelection();
                        const auto& Lights = Renderer->GetScene().Lights();
                        Smile::LogDebug("Luz selecionada [" + std::to_string(LightHit) + "] " +
                                       Lights[static_cast<size_t>(LightHit)].Name);
                    } else {
                        Renderer->RequestPick(Px, Py);
                    }
                }
            }
            setFocus();
        }
        QWidget::mousePressEvent(_Event);
    }

    void ViewportWidget::mouseReleaseEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::RightButton) {
            MouseLookActive = false;
            MouseDelta      = Smile::Vec2::Zero();
            unsetCursor();
        }
        else if (_Event->button() == Qt::LeftButton) {
            if (GizmoCtrl.IsDragging() && Renderer) {
                const QPointF P = _Event->position();
                PendingGizmoMouseX = static_cast<Smile::u32>(P.x() > 0.0 ? P.x() : 0.0);
                PendingGizmoMouseY = static_cast<Smile::u32>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoMousePending = true;
                PendingGizmoSnapInverted = (_Event->modifiers() & Qt::ControlModifier) != 0;
                GizmoReleasePending = true;

                // Caso ocupado, OnRenderTimer aplica a posição final e encerra o gesto.
                auto RendererAccess = Renderer.TryLock();
                if (RendererAccess && RendererAccess->IsInitialized())
                    FlushPendingGizmoInput(*RendererAccess);
            } else {
                // O reset também segue o caminho diferido para sincronizar com o CSM.
                GizmoReleasePending = true;
                auto RendererAccess = Renderer ? Renderer.TryLock() : decltype(Renderer.TryLock()){};
                if (RendererAccess && RendererAccess->IsInitialized())
                    FlushPendingGizmoInput(*RendererAccess);
            }
        }
        QWidget::mouseReleaseEvent(_Event);
    }

    void ViewportWidget::mouseMoveEvent(QMouseEvent* _Event) {
        if (!MouseLookActive) {
            // Coalesce o movimento mais recente quando a render thread está ocupada.
            if (!GizmoReleasePending) {
                const QPointF P = _Event->position();
                PendingGizmoMouseX = static_cast<Smile::u32>(P.x() > 0.0 ? P.x() : 0.0);
                PendingGizmoMouseY = static_cast<Smile::u32>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoMousePending = true;
                PendingGizmoSnapInverted = (_Event->modifiers() & Qt::ControlModifier) != 0;
            }
            auto RendererAccess = Renderer.TryLock();
            if (RendererAccess && RendererAccess->IsInitialized()) {
                FlushPendingGizmoInput(*RendererAccess);
            }
            QWidget::mouseMoveEvent(_Event);
            return;
        }

        if (IgnoreNextMove) {
            IgnoreNextMove = false;
            QWidget::mouseMoveEvent(_Event);
            return;
        }

        QPoint Center = mapToGlobal(rect().center());
        QPoint Position    = _Event->globalPosition().toPoint();

        MouseDelta.X += static_cast<float>(Position.x() - Center.x());
        MouseDelta.Y += static_cast<float>(Position.y() - Center.y());

        IgnoreNextMove = true;
        QCursor::setPos(Center);

        QWidget::mouseMoveEvent(_Event);
    }
} 
