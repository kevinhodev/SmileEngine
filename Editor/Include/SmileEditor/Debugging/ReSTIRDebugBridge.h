#pragma once

#include <QObject>
#include <QPointer>
#include <QEvent>
#include <QVariantList>
#include <QVariantMap>
#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    class DebugTargetsBridge;
    class RenderSettingsBridge;
    class ViewportWidget;

    // Fachada do laboratório ReSTIR. Não mantém uma segunda cópia das configurações: os mapas
    // são snapshots de apresentação montados a partir do FRenderSettings e das bridges donas dos
    // subsistemas. A janela passa a ter um contrato pequeno e coeso, enquanto persistência, MCP
    // e o renderer continuam compartilhando a mesma fonte de verdade.
    class ReSTIRDebugBridge final : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY Updated)
        Q_PROPERTY(QVariantMap overview READ GetOverview NOTIFY Updated)
        Q_PROPERTY(QVariantMap di READ GetDI NOTIFY Updated)
        Q_PROPERTY(QVariantMap gi READ GetGI NOTIFY Updated)
        Q_PROPERTY(QVariantMap transport READ GetTransport NOTIFY Updated)
        Q_PROPERTY(QVariantMap sceneInspector READ GetSceneInspector NOTIFY Updated)
        Q_PROPERTY(QVariantList rayEpsilons READ GetRayEpsilons NOTIFY Updated)
        Q_PROPERTY(bool pixelDebugAvailable READ PixelDebugAvailable NOTIFY Updated)
        Q_PROPERTY(bool pixelDebugArmed READ PixelDebugArmed NOTIFY Updated)
        Q_PROPERTY(bool pixelDebugPending READ PixelDebugPending NOTIFY Updated)
        Q_PROPERTY(QVariantMap pixelDebug READ GetPixelDebug NOTIFY Updated)
        Q_PROPERTY(QString pixelDebugStatus READ PixelDebugStatus NOTIFY Updated)

    public:
        explicit ReSTIRDebugBridge(RenderSettingsBridge* settings,
                                   DebugTargetsBridge* debugTargets,
                                   QObject* parent = nullptr);
        ~ReSTIRDebugBridge() override;

        void SetRenderer(RendererHandle renderer);
        void SetViewport(ViewportWidget* viewport);
        void SetWindowVisible(bool visible);

        bool Available() const { return static_cast<bool>(Renderer); }
        QVariantMap GetOverview() const;
        QVariantMap GetDI() const;
        QVariantMap GetGI() const;
        QVariantMap GetTransport() const;
        QVariantMap GetSceneInspector() const;
        QVariantList GetRayEpsilons() const;

        bool PixelDebugAvailable() const;
        bool PixelDebugArmed() const { return PixelPickArmed; }
        bool PixelDebugPending() const { return PixelReadbackPending; }
        QVariantMap GetPixelDebug() const { return PixelSnapshot; }
        QString PixelDebugStatus() const;

        Q_INVOKABLE void Refresh();
        Q_INVOKABLE void OpenWindow();
        Q_INVOKABLE void SetVisualizationActive(bool active);
        Q_INVOKABLE void ArmPixelDebug();
        Q_INVOKABLE void ClearPixelDebug();

        Q_INVOKABLE void ToggleDI();
        Q_INVOKABLE void SetDIAnalyticCandidates(int value);
        Q_INVOKABLE void SetDIMeshCandidates(int value);
        Q_INVOKABLE void ToggleDIMeshLights();
        Q_INVOKABLE void ToggleDIInitialVisibility();
        Q_INVOKABLE void ToggleDIBrdfRatio();
        Q_INVOKABLE void ToggleDICompatNeighbors();
        Q_INVOKABLE void ToggleDIMeshCompactSupport();

        Q_INVOKABLE void SetIndirectPrimary(int value);
        Q_INVOKABLE void SetIndirectFallback(int value);
        Q_INVOKABLE void ToggleGI();
        Q_INVOKABLE void ToggleGIHalfRes();
        Q_INVOKABLE void ToggleGIVisibility();
        Q_INVOKABLE void ToggleGICompatNeighbors();
        Q_INVOKABLE void ToggleGIBackfacePolicy();
        Q_INVOKABLE void ToggleGISourceDebug();
        Q_INVOKABLE void ToggleGIMeasureTerminatorOff();

        Q_INVOKABLE void ToggleReGIR();
        Q_INVOKABLE void ToggleRadianceCache();
        Q_INVOKABLE void ToggleRadianceCacheQuery();
        Q_INVOKABLE void ToggleRadianceCacheAutoWarmup();
        Q_INVOKABLE void ToggleRadianceCacheStats();
        Q_INVOKABLE void ToggleRadianceCacheStatsDetail();
        Q_INVOKABLE void ToggleRadianceCacheStatsSource();
        Q_INVOKABLE void ToggleRadianceCacheDedicatedUpdate();
        Q_INVOKABLE void ToggleRadianceCacheCompactUpdate();
        Q_INVOKABLE void ToggleRadianceCachePrevTerminal();
        Q_INVOKABLE void SetRadianceCacheUpdateFraction(double value);
        Q_INVOKABLE void SetRadianceCacheMaxVertices(int value);
        Q_INVOKABLE void SetRadianceCacheMinRoughness(double value);
        Q_INVOKABLE void SetRadianceCacheMinSamples(int value);
        Q_INVOKABLE void SetRadianceCacheCellSize(double value);
        Q_INVOKABLE void SetRadianceCacheLodDistance(double value);
        Q_INVOKABLE void SetRadianceCacheDebugMode(int value);
        Q_INVOKABLE void ResetRadianceCache();

        Q_INVOKABLE void SetRayEpsilon(const QString& key, double value);
        Q_INVOKABLE void ResetRayEpsilons();

        Q_INVOKABLE void ToggleSceneInspector();
        Q_INVOKABLE void SetSceneInspectorMode(int mode);
        Q_INVOKABLE void ToggleRtShaderTimer();

    signals:
        void Updated();
        void OpenWindowRequested();

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        RenderSettingsBridge* Settings = nullptr;
        DebugTargetsBridge* DebugTargets = nullptr;
        QPointer<ViewportWidget> Viewport;
        RendererHandle Renderer;
        bool WindowVisible = false;
        bool VisualizationActive = false;
        bool PixelPickArmed = false;
        bool PixelReadbackPending = false;
        QString PixelDebugMessage;
        QVariantMap PixelSnapshot;

        void SyncPreviewConsumer();
        void SyncDebugExecutionGate();
        void ReleaseRendererGate();
        void CancelPixelInteraction(bool clearSnapshot);
        bool ConsumePixelDebug();
    };
}
