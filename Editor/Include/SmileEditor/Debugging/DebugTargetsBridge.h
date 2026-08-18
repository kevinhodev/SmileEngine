#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QPointer>
#include <QQuickImageProvider>
#include <QStringList>
#include <QVariantList>
#include <array>
#include "Smile/Math/Math.h"
#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    class ViewportWidget;

    class DebugTargetsBridge final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QStringList debugTargetNames READ GetDebugTargetNames NOTIFY Updated)
        Q_PROPERTY(int debugTargetIndex READ GetDebugTargetIndex NOTIFY Updated)
        Q_PROPERTY(bool rtShaderTimerAvailable READ IsRtShaderTimerAvailable NOTIFY Updated)
        Q_PROPERTY(bool rtShaderTimerEnabled READ IsRtShaderTimerEnabled NOTIFY Updated)
        Q_PROPERTY(bool bvhDebugAvailable READ IsBvhDebugAvailable NOTIFY Updated)
        Q_PROPERTY(bool bvhDebugEnabled READ IsBvhDebugEnabled NOTIFY Updated)
        Q_PROPERTY(int bvhDebugMode READ GetBvhDebugMode NOTIFY Updated)
        Q_PROPERTY(QVariantList debugSelection READ GetDebugSelection NOTIFY Updated)
        Q_PROPERTY(int debugColumns READ GetDebugColumns NOTIFY Updated)
        Q_PROPERTY(double debugExposure READ GetDebugExposure NOTIFY Updated)
        Q_PROPERTY(int debugPreviewSeq READ GetDebugPreviewSeq NOTIFY PreviewUpdated)
        Q_PROPERTY(bool debugPreviewReady READ IsDebugPreviewReady NOTIFY PreviewUpdated)
        Q_PROPERTY(bool debugProbeInspecting READ IsDebugProbeInspecting NOTIFY Updated)
        Q_PROPERTY(int debugProbeIndex READ GetDebugProbeIndex NOTIFY Updated)
        Q_PROPERTY(QString debugProbeCoord READ GetDebugProbeCoord NOTIFY Updated)
        Q_PROPERTY(QString debugProbeWorld READ GetDebugProbeWorld NOTIFY Updated)
        Q_PROPERTY(QString debugProbeGrid READ GetDebugProbeGrid NOTIFY Updated)
        Q_PROPERTY(QString debugProbeDistanceRange READ GetDebugProbeDistanceRange NOTIFY Updated)
        Q_PROPERTY(QString debugProbeDirection READ GetDebugProbeDirection NOTIFY Updated)
        Q_PROPERTY(QString debugProbeSample READ GetDebugProbeSample NOTIFY Updated)
        Q_PROPERTY(bool debugProbePointPickArmed READ IsDebugProbePointPickArmed NOTIFY Updated)
        Q_PROPERTY(QString debugProbePointSummary READ GetDebugProbePointSummary NOTIFY Updated)
        Q_PROPERTY(QVariantList debugProbeContributors READ GetDebugProbeContributors NOTIFY Updated)
        Q_PROPERTY(QString viewModeLabel READ GetViewModeLabel NOTIFY Updated)

    public:
        explicit DebugTargetsBridge(QObject* parent = nullptr);

        void SetViewport(ViewportWidget* viewport);
        void SetPreviewEnabled(bool enabled);
        void NotifyResourcesChanged();

        QStringList GetDebugTargetNames() const;
        int GetDebugTargetIndex() const;
        bool IsRtShaderTimerAvailable() const;
        bool IsRtShaderTimerEnabled() const;
        bool IsBvhDebugAvailable() const;
        bool IsBvhDebugEnabled() const;
        int GetBvhDebugMode() const;
        QVariantList GetDebugSelection() const;
        int GetDebugColumns() const;
        double GetDebugExposure() const;
        int GetDebugPreviewSeq() const { return DebugPreviewSeq; }
        bool IsDebugPreviewReady() const;
        QImage PreviewImageCopy() const;
        bool IsDebugProbeInspecting() const { return DebugProbeSessionActive; }
        int GetDebugProbeIndex() const;
        QString GetDebugProbeCoord() const;
        QString GetDebugProbeWorld() const;
        QString GetDebugProbeGrid() const;
        QString GetDebugProbeDistanceRange() const;
        QString GetDebugProbeDirection() const { return DebugProbeDirection; }
        QString GetDebugProbeSample() const { return DebugProbeSample; }
        bool IsDebugProbePointPickArmed() const { return DebugProbePointPickArmed; }
        QString GetDebugProbePointSummary() const { return DebugProbePointSummary; }
        QVariantList GetDebugProbeContributors() const { return DebugProbeContributors; }
        QString GetViewModeLabel() const;

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

    signals:
        void Updated();
        void PreviewUpdated();

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        struct FDebugProbeCoord {
            int X = 0, Y = 0, Z = 0;
            int CountX = 0, CountY = 0, CountZ = 0;
            int Cascade = 0, LocalIndex = 0;
            Smile::Vec3 GridMin{};
            Smile::f32 Spacing = 1.0f;
        };

        bool GetDebugProbeCoordValues(FDebugProbeCoord& out) const;
        void ConsumeReadbacks();
        void InvalidatePreview();
        void ResetDebugProbePoint(bool cancelRendererRequest = true);
        void SelectDebugProbe(int probeIndex);

        QPointer<ViewportWidget> Viewport;
        RendererHandle Renderer;
        QImage DebugPreviewImage;
        mutable QMutex DebugPreviewMutex;
        int DebugPreviewSeq = 0;
        bool PreviewEnabled = false;
        bool DebugProbeSessionActive = false;
        QStringList DebugProbePreviousTargets;
        int DebugProbePreviousColumns = 0;
        QString DebugProbeDirection;
        QString DebugProbeSample;
        bool DebugProbePointPickArmed = false;
        int DebugProbeBaseIndex = -1;
        QString DebugProbePointSummary;
        QVariantList DebugProbeContributors;
        std::array<Smile::u32, 8> DebugProbeContributorIndices{};
        std::array<float, 8> DebugProbeContributorWeights{};
        Smile::u32 DebugProbeContributorCount = 0;
        int DebugProbeContributorRiskSlot = -1;
    };

    class DebugTargetPreviewImageProvider : public QQuickImageProvider {
    public:
        explicit DebugTargetPreviewImageProvider(DebugTargetsBridge* bridge)
            : QQuickImageProvider(QQuickImageProvider::Image), Bridge(bridge) {}

        QImage requestImage(const QString&, QSize* size, const QSize&) override {
            QImage image = Bridge ? Bridge->PreviewImageCopy() : QImage();
            if (size) *size = image.size();
            return image;
        }

    private:
        QPointer<DebugTargetsBridge> Bridge;
    };
}
