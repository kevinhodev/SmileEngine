#pragma once

#include "SmileEditor/Viewport/RenderThread.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace Smile { class Renderer; }

namespace SmileEditor {
    class ViewportWidget;

    class StatsBridge final : public QObject {
        Q_OBJECT
        Q_PROPERTY(double fps READ GetFPS NOTIFY Updated)
        Q_PROPERTY(double frameTimeMs READ GetFrameTimeMs NOTIFY Updated)
        Q_PROPERTY(int visibleDrawCount READ GetVisibleDrawCount NOTIFY Updated)
        Q_PROPERTY(int occludedDrawCount READ GetOccludedDrawCount NOTIFY Updated)
        Q_PROPERTY(int totalDrawCount READ GetTotalDrawCount NOTIFY Updated)
        Q_PROPERTY(QString internalResolution READ GetInternalResolution NOTIFY Updated)
        Q_PROPERTY(QString outputResolution READ GetOutputResolution NOTIFY Updated)
        Q_PROPERTY(QString gpuName READ GetGPUName NOTIFY Updated)
        Q_PROPERTY(QString vramText READ GetVRAMText NOTIFY Updated)
        Q_PROPERTY(QString vramUsageText READ GetVRAMUsageText NOTIFY Updated)
        Q_PROPERTY(bool vramOverBudget READ IsVRAMOverBudget NOTIFY Updated)
        Q_PROPERTY(double vramBudgetFrac READ GetVRAMBudgetFrac NOTIFY Updated)
        Q_PROPERTY(QString vramNonLocalText READ GetVRAMNonLocalText NOTIFY Updated)
        Q_PROPERTY(QVariantList vramBreakdown READ GetVRAMBreakdown NOTIFY Updated)
        Q_PROPERTY(QString gpuFrameText READ GetGpuFrameText NOTIFY Updated)
        Q_PROPERTY(double gpuFrameMs READ GetGpuFrameMs NOTIFY Updated)
        Q_PROPERTY(QVariantList gpuTimings READ GetGpuTimings NOTIFY Updated)
        Q_PROPERTY(QVariantList shadowCascades READ GetShadowCascades NOTIFY Updated)

    public:
        explicit StatsBridge(QObject* Parent = nullptr);

        void SetViewport(ViewportWidget* Value);

        double       GetFPS() const;
        double       GetFrameTimeMs() const;
        int          GetVisibleDrawCount() const;
        int          GetTotalDrawCount() const;
        int          GetOccludedDrawCount() const;
        QString      GetInternalResolution() const;
        QString      GetOutputResolution() const;
        QString      GetGPUName() const;
        QString      GetVRAMText() const;
        QString      GetVRAMUsageText() const;
        bool         IsVRAMOverBudget() const;
        double       GetVRAMBudgetFrac() const;
        QString      GetVRAMNonLocalText() const;
        QVariantList GetVRAMBreakdown() const;
        QString      GetGpuFrameText() const;
        double       GetGpuFrameMs() const;
        QVariantList GetGpuTimings() const;
        QVariantList GetShadowCascades() const;

    signals:
        void Updated();

    private:
        void Refresh();
        void Capture(Smile::Renderer& Renderer);
        QVariantList BuildVRAMBreakdown(Smile::Renderer& Renderer) const;
        QVariantList BuildGpuTimings(Smile::Renderer& Renderer);
        QVariantList BuildShadowCascades(Smile::Renderer& Renderer) const;

        struct FSnapshot {
            double       FPS = 0.0;
            int          VisibleDrawCount = 0;
            int          TotalDrawCount = 0;
            int          OccludedDrawCount = 0;
            QString      InternalResolution = QStringLiteral("—");
            QString      OutputResolution = QStringLiteral("—");
            QString      GPUName = QStringLiteral("Inicializando GPU…");
            QString      VRAMText = QStringLiteral("—");
            QString      VRAMUsageText = QStringLiteral("—");
            bool         VRAMOverBudget = false;
            double       VRAMBudgetFrac = 0.0;
            QString      VRAMNonLocalText = QStringLiteral("—");
            QVariantList VRAMBreakdown;
            QString      GPUFrameText = QStringLiteral("—");
            double       GPUFrameMs = 0.0;
            QVariantList GpuTimings;
            QVariantList ShadowCascades;
        } Snapshot;

        QPointer<ViewportWidget> Viewport;
        RendererHandle           Renderer;
        QElapsedTimer            RefreshTimer;
        QStringList              GpuTimingOrder;
    };
}
