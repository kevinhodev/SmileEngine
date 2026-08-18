#pragma once

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QPoint>
#include <QElapsedTimer>
#include <QString>
#include <QVariantList>
#include <QStringList>
#include <array>
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
        Q_PROPERTY(QStringList debugTargetNames READ GetDebugTargetNames NOTIFY DebugTargetsChanged)
        Q_PROPERTY(int debugTargetIndex READ GetDebugTargetIndex NOTIFY ViewStateChanged)
        Q_PROPERTY(bool rtShaderTimerAvailable READ IsRtShaderTimerAvailable NOTIFY DebugTargetsChanged)
        Q_PROPERTY(bool rtShaderTimerEnabled READ IsRtShaderTimerEnabled NOTIFY DebugSettingsChanged)
        Q_PROPERTY(bool bvhDebugAvailable READ IsBvhDebugAvailable NOTIFY DebugTargetsChanged)
        Q_PROPERTY(bool bvhDebugEnabled READ IsBvhDebugEnabled NOTIFY DebugSettingsChanged)
        Q_PROPERTY(int bvhDebugMode READ GetBvhDebugMode NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QVariantList debugSelection READ GetDebugSelection NOTIFY DebugSettingsChanged)
        Q_PROPERTY(int debugColumns READ GetDebugColumns NOTIFY DebugSettingsChanged)
        Q_PROPERTY(double debugExposure READ GetDebugExposure NOTIFY DebugSettingsChanged)
        Q_PROPERTY(int debugPreviewSeq READ GetDebugPreviewSeq NOTIFY DebugPreviewUpdated)
        Q_PROPERTY(bool debugPreviewReady READ IsDebugPreviewReady NOTIFY DebugPreviewUpdated)
        Q_PROPERTY(bool debugProbeInspecting READ IsDebugProbeInspecting NOTIFY DebugSettingsChanged)
        Q_PROPERTY(int debugProbeIndex READ GetDebugProbeIndex NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QString debugProbeCoord READ GetDebugProbeCoord NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QString debugProbeWorld READ GetDebugProbeWorld NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QString debugProbeGrid READ GetDebugProbeGrid NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QString debugProbeDistanceRange READ GetDebugProbeDistanceRange NOTIFY DebugSettingsChanged)
        Q_PROPERTY(QString debugProbeDirection READ GetDebugProbeDirection NOTIFY DebugProbeDirectionChanged)
        Q_PROPERTY(QString debugProbeSample READ GetDebugProbeSample NOTIFY DebugProbeSampleChanged)
        Q_PROPERTY(bool debugProbePointPickArmed READ IsDebugProbePointPickArmed NOTIFY DebugProbePointChanged)
        Q_PROPERTY(QString debugProbePointSummary READ GetDebugProbePointSummary NOTIFY DebugProbePointChanged)
        Q_PROPERTY(QVariantList debugProbeContributors READ GetDebugProbeContributors NOTIFY DebugProbePointChanged)
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
        QStringList       GetDebugTargetNames() const;
        int               GetDebugTargetIndex() const;
        bool              IsRtShaderTimerAvailable() const;
        bool              IsRtShaderTimerEnabled() const;
        bool              IsBvhDebugAvailable() const;
        bool              IsBvhDebugEnabled() const;
        int               GetBvhDebugMode() const;
        QVariantList      GetDebugSelection() const;
        int               GetDebugColumns() const;
        double            GetDebugExposure() const;
        int               GetDebugPreviewSeq() const { return DebugPreviewSeq; }
        bool              IsDebugPreviewReady() const;
        QImage            DebugPreviewImageCopy() const;
        void              SetDebugPreviewEnabled(bool enabled);
        // Durante resize interativo, o DXGI preserva o backbuffer até o gesto terminar.
        void              BeginInteractiveResize();
        void              EndInteractiveResize();
        bool              IsDebugProbeInspecting() const { return DebugProbeSessionActive; }
        int               GetDebugProbeIndex() const;
        QString           GetDebugProbeCoord() const;
        QString           GetDebugProbeWorld() const;
        QString           GetDebugProbeGrid() const;
        QString           GetDebugProbeDistanceRange() const;
        QString           GetDebugProbeDirection() const { return DebugProbeDirection; }
        QString           GetDebugProbeSample() const { return DebugProbeSample; }
        bool              IsDebugProbePointPickArmed() const { return DebugProbePointPickArmed; }
        QString           GetDebugProbePointSummary() const { return DebugProbePointSummary; }
        QVariantList      GetDebugProbeContributors() const { return DebugProbeContributors; }
        QString           GetViewModeLabel() const;

        Q_INVOKABLE void SetGizmoMode(int mode);
        Q_INVOKABLE void SetGizmoSpace(int space);
        Q_INVOKABLE void ToggleGizmoSpace();
        Q_INVOKABLE void ToggleSnap(int mode);
        Q_INVOKABLE void SetSnapValue(int mode, double value);

        Q_INVOKABLE void SelectLit();
        Q_INVOKABLE void SelectReflectionHeatmap();
        Q_INVOKABLE void SelectDebugTarget(int index);
        Q_INVOKABLE void ToggleDebugSelection(int index);
        Q_INVOKABLE void ClearDebugSelection();
        Q_INVOKABLE void ToggleRtShaderTimer();
        Q_INVOKABLE void ToggleBvhDebug();
        Q_INVOKABLE void SetBvhDebugMode(int mode);
        Q_INVOKABLE void SetDebugColumns(int columns);
        Q_INVOKABLE void SetDebugExposure(double exposure);
        Q_INVOKABLE void InspectDDGIProbe(int targetIndex, double u, double v,
                                          double tileAspect);
        Q_INVOKABLE void StepDebugProbe(int dx, int dy, int dz);
        Q_INVOKABLE void ClearDebugProbeInspection();
        Q_INVOKABLE void UpdateDebugProbeDirection(double u, double v);
        Q_INVOKABLE void ArmDebugProbePointPick();
        Q_INVOKABLE void ClearDebugProbePoint();
        Q_INVOKABLE void SelectDebugProbeContributor(int probeIndex);

        // Republica alvos que passam a existir após load ou recriação de recursos.
        void NotifyDebugTargetsChanged() {
            if (DebugProbeSessionActive) ClearDebugProbeInspection();
            emit DebugTargetsChanged();
            emit ViewStateChanged();
            emit DebugSettingsChanged();
        }
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
        void DebugSettingsChanged();
        void SettingsRequested();
        void DebugTargetsChanged();
        void DebugPreviewUpdated();
        void DebugProbeDirectionChanged();
        void DebugProbeSampleChanged();
        void DebugProbePointChanged();
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
        void InvalidateDebugPreview();
        void ResetDebugProbePoint(bool CancelRendererRequest = true);
        // Atualiza a seleção persistente; point-pick mantém foco temporário separado.
        void SelectDebugProbe(int ProbeIndex);
        // Decomposição única do índice global da probe inspecionada.
        struct FDebugProbeCoord {
            int  X = 0, Y = 0, Z = 0;
            int  CountX = 0, CountY = 0, CountZ = 0;
            int  Cascade = 0, LocalIndex = 0;
            Smile::Vec3 GridMin{};
            Smile::f32  Spacing = 1.0f;
        };
        bool GetDebugProbeCoordValues(FDebugProbeCoord& Out) const;

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
        QImage         DebugPreviewImage;
        mutable QMutex DebugPreviewMutex;
        int            DebugPreviewSeq = 0;
        bool           DebugProbeSessionActive = false;
        QStringList    DebugProbePreviousTargets;
        int            DebugProbePreviousColumns = 0;
        QString        DebugProbeDirection;
        QString        DebugProbeSample;
        bool           DebugProbePointPickArmed = false;
        // Seleção explícita restaurada quando um point-pick temporário não encontra contribuição.
        int            DebugProbeBaseIndex = -1;
        QString        DebugProbePointSummary;
        QVariantList   DebugProbeContributors;
        std::array<Smile::u32, 8> DebugProbeContributorIndices{};
        std::array<float, 8>      DebugProbeContributorWeights{};
        Smile::u32     DebugProbeContributorCount = 0;
        int            DebugProbeContributorRiskSlot = -1;
    };

    // requestImage pode rodar na render thread do Qt Quick; a imagem retornada é uma cópia.
    class DebugTargetPreviewImageProvider : public QQuickImageProvider {
    public:
        explicit DebugTargetPreviewImageProvider(ViewportWidget* Viewport)
            : QQuickImageProvider(QQuickImageProvider::Image), Viewport(Viewport) {}

        QImage requestImage(const QString&, QSize* Size, const QSize&) override {
            QImage Image = Viewport ? Viewport->DebugPreviewImageCopy() : QImage();
            if (Size) *Size = Image.size();
            return Image;
        }

    private:
        ViewportWidget* Viewport = nullptr;
    };
} 
