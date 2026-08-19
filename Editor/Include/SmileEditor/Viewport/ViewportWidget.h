#pragma once

#include <QWidget>
#include <QSet>
#include <QPoint>
#include <QElapsedTimer>
#include <QString>
#include <deque>
#include <functional>
#include <memory>
#include "Smile/Math/Math.h"
#include "SmileEditor/Viewport/GizmoController.h"
#include "SmileEditor/Viewport/RenderThread.h"

class QTimer;
class QPaintEngine;
class QKeyEvent;
class QMouseEvent;

namespace Smile {
    class Renderer;
    struct FSceneImportResult;
}

namespace SmileEditor {
    class ViewportWidget : public QWidget {
        Q_OBJECT
        Q_PROPERTY(int viewMode READ GetViewMode NOTIFY ViewStateChanged)
        Q_PROPERTY(int gizmoMode READ GetGizmoMode NOTIFY GizmoModeChanged)
        Q_PROPERTY(int gizmoSpace READ GetGizmoSpace NOTIFY GizmoSpaceChanged)
        Q_PROPERTY(int gizmoSpaceEffective READ GetGizmoSpaceEffective NOTIFY GizmoSpaceChanged)
        Q_PROPERTY(int gizmoSpaceRestriction READ GetGizmoSpaceRestriction NOTIFY GizmoSpaceChanged)
        Q_PROPERTY(bool snapTranslateOn READ GetSnapTranslateOn NOTIFY GizmoSnapChanged)
        Q_PROPERTY(bool snapRotateOn READ GetSnapRotateOn NOTIFY GizmoSnapChanged)
        Q_PROPERTY(bool snapScaleOn READ GetSnapScaleOn NOTIFY GizmoSnapChanged)
        Q_PROPERTY(double snapTranslateM READ GetSnapTranslateM NOTIFY GizmoSnapChanged)
        Q_PROPERTY(double snapRotateDeg READ GetSnapRotateDeg NOTIFY GizmoSnapChanged)
        Q_PROPERTY(double snapScaleStep READ GetSnapScaleStep NOTIFY GizmoSnapChanged)
        Q_PROPERTY(QString viewModeLabel READ GetViewModeLabel NOTIFY ViewStateChanged)

    public:
        // Valores fazem parte do contrato com o QML; lacunas preservam modos removidos.
        enum ViewMode {
            Lit = 0,
            ReflectionHeatmap = 3
        };
        Q_ENUM(ViewMode)

        explicit ViewportWidget(QWidget* parent = nullptr);
        ~ViewportWidget() override;

        // Solicita shutdown assíncrono; o objeto deve viver até RendererStopped.
        void              BeginRendererShutdown();
        bool              IsRendererStopped() const { return RendererStoppedFlag; }
        RendererHandle    GetRenderer() const { return Renderer; }
        using SceneCommitCallback = std::function<void(bool, const QString&)>;
        bool CommitImportedSceneAsync(
            std::shared_ptr<Smile::FSceneImportResult> Imported,
            bool Additive,
            SceneCommitCallback Completion);
        float            GetFPS()      const { return LastFPS; }
        int               GetViewMode() const { return CurrentViewMode; }
        int               GetGizmoMode() const {
            return static_cast<int>(GizmoCtrl.GetMode());
        }
        int               GetGizmoSpace() const {
            return static_cast<int>(GizmoCtrl.GetSpace());
        }
        int               GetGizmoSpaceEffective() const {
            return static_cast<int>(GizmoCtrl.EffectiveSpace());
        }
        int               GetGizmoSpaceRestriction() const {
            return static_cast<int>(GizmoCtrl.Restriction());
        }
        bool              GetSnapTranslateOn() const { return GizmoCtrl.Snap().TranslateOn; }
        bool              GetSnapRotateOn()    const { return GizmoCtrl.Snap().RotateOn; }
        bool              GetSnapScaleOn()     const { return GizmoCtrl.Snap().ScaleOn; }
        double            GetSnapTranslateM()  const { return GizmoCtrl.Snap().TranslateM; }
        double            GetSnapRotateDeg()   const { return GizmoCtrl.Snap().RotateDeg; }
        double            GetSnapScaleStep()   const { return GizmoCtrl.Snap().ScaleStep; }
        // Durante resize interativo, o DXGI preserva o backbuffer até o gesto terminar.
        void              BeginInteractiveResize();
        void              EndInteractiveResize();
        void              NotifyRendererResourcesChanged() { emit RendererResourcesChanged(); }
        // Benchmarks MCP precisam manter o mesmo pacing mesmo quando o Codex recebe o foco.
        // O comportamento interativo continua throttled por default.
        void              SetBackgroundThrottleEnabled(bool Enabled);
        QString           GetViewModeLabel() const;

        Q_INVOKABLE void SetGizmoMode(int mode);
        Q_INVOKABLE void SetGizmoSpace(int space);
        Q_INVOKABLE void ToggleGizmoSpace();
        Q_INVOKABLE void ToggleSnap(int mode);
        Q_INVOKABLE void SetSnapValue(int mode, double value);

        Q_INVOKABLE void SelectLit();
        Q_INVOKABLE void SelectReflectionHeatmap();
        Q_INVOKABLE void RequestSettings();

        QPaintEngine* paintEngine() const override;

    protected:
        void showEvent(QShowEvent* event)   override;
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void hideEvent(QHideEvent* event)   override;

        void keyPressEvent(QKeyEvent* event)     override;
        void keyReleaseEvent(QKeyEvent* event)   override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event)  override;

    signals:
        void FrameReady();
        void RendererInitialized();
        void InitProgress(const QString& label, const QString& detail, qreal fraction);
        void ObjectSelected(int sceneIndex);
        // Atalhos pertencem ao foco da viewport para não interceptar campos de texto globais.
        void DeleteSelectionRequested();
        void DuplicateSelectionRequested();
        void ViewStateChanged();
        void GizmoModeChanged();
        void GizmoSpaceChanged();
        void GizmoSnapChanged();
        // Emitido apenas quando um gesto altera a camada autorada de renderizáveis.
        void SceneEdited();
        void SettingsRequested();
        void RendererResourcesChanged();
        void RendererStopped();

    private slots:
        void OnRenderTimer();

    private:
        void EnsureRendererIsInitialized();
        void OnRendererInitialized();
        void OnFrameCompleted(bool Success, bool Terminal, const QString& Error);
        void OnFrameRendered();
        void OnRendererInitializationFailed(const QString& Error);
        void OnRenderThreadStopped();
        void FlushPendingGizmoInput(Smile::Renderer& Renderer);
        void ApplyPendingResize();
    public:
        // Serializa operações que recriam recursos com a produção de frames.
        bool EnqueueRendererJob(const QString& CoalesceKey,
                                RenderThread::RendererJob Job,
                                SceneCommitCallback Completion);
    private:
        void DispatchNextRendererJob();
        bool IsHeld(int key) const { return HeldKeys.contains(key); }

        RenderThread   RendererThread;
        RendererHandle Renderer;
        GizmoController GizmoCtrl;
        QTimer*       RedrawTimer       = nullptr;
        QTimer*       InitializationDebounce = nullptr;
        QTimer*       ResizeDebounce    = nullptr;
        bool          Initialized       = false;
        bool          RendererShutdownRequested = false;
        bool          RendererStoppedFlag = false;
        bool          InteractiveResize = false;
        bool          BackgroundThrottleEnabled = true;
        QSize         PendingResizeSize;
        QSize         AppliedResizeSize;

        QSet<int>     HeldKeys;
        Smile::Vec2   MouseDelta       = Smile::Vec2::Zero();
        bool          MouseLookActive  = false;
        bool          IgnoreNextMove   = false;
        bool          RendererOwnsSurface = false;
        struct FQueuedRendererJob {
            QString                     CoalesceKey;
            RenderThread::RendererJob   Execute;
            SceneCommitCallback         Completion;
        };
        std::deque<FQueuedRendererJob> RendererJobs;
        bool          RendererJobActive = false;
        // Capturado com o evento porque o gesto pode ser aplicado no frame seguinte.
        bool          PendingGizmoSnapInverted = false;
        bool          GizmoMousePending = false;
        bool          GizmoReleasePending = false;
        Smile::u32    PendingGizmoMouseX = 0;
        Smile::u32    PendingGizmoMouseY = 0;
        float         LastFPS          = 0.0f;
        float         LastFrameDeltaTime = 0.0f;
        QElapsedTimer FrameTimer;
        int           CurrentViewMode  = Lit;
    };
} 
