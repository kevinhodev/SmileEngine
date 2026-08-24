#include "SmileEditor/Debugging/ReSTIRDebugBridge.h"
#include "SmileEditor/Debugging/DebugTargetsBridge.h"
#include "SmileEditor/Rendering/RenderSettingsBridge.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/GI/ReSTIRGI.h"
#include "Smile/Graphics/GI/ReGIR.h"
#include "Smile/Graphics/Lighting/ReSTIRDI.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"

#include <QEvent>
#include <QLocale>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    namespace {
        QString BoolState(bool requested, bool effective, bool ready) {
            if (!requested) return QStringLiteral("desligado");
            if (!ready) return QStringLiteral("indisponível");
            return effective ? QStringLiteral("ativo") : QStringLiteral("em espera");
        }

        QString ResolutionText(Smile::u32 width, Smile::u32 height) {
            if (width == 0 || height == 0) return QStringLiteral("—");
            return QStringLiteral("%1 × %2").arg(width).arg(height);
        }

        QString Number(Smile::f32 value, int precision = 3) {
            if (!std::isfinite(value)) return QStringLiteral("—");
            return QLocale::c().toString(static_cast<double>(value), 'f', precision);
        }

        QString Vec3Text(const Smile::f32 value[3], int precision = 3) {
            return QStringLiteral("(%1, %2, %3)")
                .arg(Number(value[0], precision), Number(value[1], precision),
                     Number(value[2], precision));
        }

        QString Vec4Text(const Smile::f32 value[4], int precision = 3) {
            return QStringLiteral("(%1, %2, %3, %4)")
                .arg(Number(value[0], precision), Number(value[1], precision),
                     Number(value[2], precision), Number(value[3], precision));
        }

        QString ShadingModelName(Smile::u32 model) {
            switch (model) {
            case 0: return QStringLiteral("Default Lit");
            case 1: return QStringLiteral("Folhagem");
            case 2: return QStringLiteral("Subsurface");
            case 3: return QStringLiteral("Água");
            default: return QStringLiteral("desconhecido");
            }
        }
    }

    ReSTIRDebugBridge::ReSTIRDebugBridge(RenderSettingsBridge* _Settings,
                                         DebugTargetsBridge* _DebugTargets,
                                         QObject* _Parent)
        : QObject(_Parent), Settings(_Settings), DebugTargets(_DebugTargets) {
        if (Settings) {
            connect(Settings, &RenderSettingsBridge::AvailableChanged,
                    this, &ReSTIRDebugBridge::Updated);
            connect(Settings, &RenderSettingsBridge::GISettingsChanged,
                    this, &ReSTIRDebugBridge::Updated);
            connect(Settings, &RenderSettingsBridge::StatsChanged,
                    this, &ReSTIRDebugBridge::Updated);
            connect(Settings, &RenderSettingsBridge::RenderSettingsChanged,
                    this, &ReSTIRDebugBridge::Updated);
        }
        if (DebugTargets) {
            connect(DebugTargets, &DebugTargetsBridge::Updated,
                    this, &ReSTIRDebugBridge::Updated);
        }
    }

    ReSTIRDebugBridge::~ReSTIRDebugBridge() {
        ReleaseRendererGate();
    }

    void ReSTIRDebugBridge::SetRenderer(RendererHandle _Renderer) {
        ReleaseRendererGate();
        Renderer = _Renderer;
        SyncDebugExecutionGate();
        emit Updated();
    }

    void ReSTIRDebugBridge::SetViewport(ViewportWidget* _Viewport) {
        if (Viewport == _Viewport) return;
        if (Viewport) {
            Viewport->removeEventFilter(this);
            Viewport->unsetCursor();
            disconnect(Viewport, nullptr, this, nullptr);
        }
        ReleaseRendererGate();
        Viewport = _Viewport;
        Renderer = Viewport ? Viewport->GetRenderer() : RendererHandle{};
        if (Viewport) {
            Viewport->installEventFilter(this);
            connect(Viewport, &ViewportWidget::FrameReady,
                    this, [this]() { ConsumePixelDebug(); });
            connect(Viewport, &ViewportWidget::RendererResourcesChanged,
                    this, &ReSTIRDebugBridge::Updated);
            connect(Viewport, &QObject::destroyed, this, [this]() {
                ReleaseRendererGate();
                Viewport = nullptr;
                Renderer = RendererHandle{};
                PixelPickArmed = false;
                PixelReadbackPending = false;
                emit Updated();
            });
        }
        SyncDebugExecutionGate();
        emit Updated();
    }

    void ReSTIRDebugBridge::SetWindowVisible(bool _Visible) {
        if (WindowVisible == _Visible) return;
        WindowVisible = _Visible;
        if (!WindowVisible) CancelPixelInteraction(false);
        SyncDebugExecutionGate();
        SyncPreviewConsumer();
        emit Updated();
    }

    void ReSTIRDebugBridge::SetVisualizationActive(bool _Active) {
        if (VisualizationActive == _Active) return;
        VisualizationActive = _Active;
        if (!VisualizationActive) CancelPixelInteraction(false);
        SyncPreviewConsumer();
    }

    void ReSTIRDebugBridge::SyncPreviewConsumer() {
        if (DebugTargets) {
            DebugTargets->SetPreviewConsumerActive(
                QStringLiteral("restir-lab"),
                WindowVisible && VisualizationActive);
        }
    }

    void ReSTIRDebugBridge::SyncDebugExecutionGate() {
        if (!Renderer) return;
        auto Access = Renderer.Lock();
        if (!Access) return;
        Access->SetDebugToolsWindowActive(WindowVisible);
        if (!WindowVisible) Access->CancelPixelDebug();
    }

    void ReSTIRDebugBridge::ReleaseRendererGate() {
        if (!Renderer) return;
        auto Access = Renderer.Lock();
        if (!Access) return;
        Access->SetDebugToolsWindowActive(false);
        Access->CancelPixelDebug();
    }

    QVariantMap ReSTIRDebugBridge::GetOverview() const {
        QVariantMap Out;
        Out.insert(QStringLiteral("available"), Available());
        if (!Renderer) {
            Out.insert(QStringLiteral("diStatus"), QStringLiteral("aguardando renderer"));
            Out.insert(QStringLiteral("giStatus"), QStringLiteral("aguardando renderer"));
            Out.insert(QStringLiteral("reGIRStatus"), QStringLiteral("aguardando renderer"));
            Out.insert(QStringLiteral("cacheStatus"), QStringLiteral("aguardando renderer"));
            return Out;
        }

        auto Access = Renderer.Lock();
        const auto& RS = Access->Settings();
        const auto& DI = Access->GetReSTIRDI();
        const auto& GI = Access->GetReSTIRGI();
        const auto& ReGIR = Access->GetReGIR();

        const bool DIRequested = RS.GetUseReSTIRDI();
        const bool GIRequested = RS.GetUseReSTIRGI();
        const bool ReGIRRequested = RS.GetUseReGIR();
        Out.insert(QStringLiteral("diRequested"), DIRequested);
        Out.insert(QStringLiteral("diActive"), DIRequested && DI.IsReady());
        Out.insert(QStringLiteral("diStatus"),
                   BoolState(DIRequested, DIRequested && DI.IsReady(), DI.IsReady()));
        Out.insert(QStringLiteral("giRequested"), GIRequested);
        Out.insert(QStringLiteral("giActive"), GIRequested && GI.IsReady());
        Out.insert(QStringLiteral("giStatus"),
                   BoolState(GIRequested, GIRequested && GI.IsReady(), GI.IsReady()));
        Out.insert(QStringLiteral("reGIRRequested"), ReGIRRequested);
        Out.insert(QStringLiteral("reGIRActive"), RS.ReGIRActive());
        Out.insert(QStringLiteral("reGIRStatus"),
                   BoolState(ReGIRRequested, RS.ReGIRActive(), ReGIR.IsReady()));
        Out.insert(QStringLiteral("cacheActive"),
                   RS.GetRadianceCacheEnabled() && RS.GetRadianceCacheQuery());
        Out.insert(QStringLiteral("cacheStatus"),
                   RS.GetRadianceCacheEnabled()
                       ? (RS.GetRadianceCacheQuery() ? QStringLiteral("consultando")
                                                     : QStringLiteral("somente produtor"))
                       : QStringLiteral("desligado"));
        Out.insert(QStringLiteral("indirectPrimary"),
                   static_cast<int>(RS.GetIndirectPrimary()));
        Out.insert(QStringLiteral("giResolution"),
                   ResolutionText(GI.TraceWidth(), GI.TraceHeight()));
        Out.insert(QStringLiteral("diResolution"),
                   ResolutionText(DI.DIRenderWidth(), DI.DIRenderHeight()));
        return Out;
    }

    QVariantMap ReSTIRDebugBridge::GetDI() const {
        QVariantMap Out;
        if (!Renderer) return Out;
        auto Access = Renderer.Lock();
        const auto& RS = Access->Settings();
        const auto& DI = Access->GetReSTIRDI();
        Out.insert(QStringLiteral("enabled"), RS.GetUseReSTIRDI());
        Out.insert(QStringLiteral("ready"), DI.IsReady());
        Out.insert(QStringLiteral("active"), RS.GetUseReSTIRDI() && DI.IsReady());
        Out.insert(QStringLiteral("analyticCandidates"),
                   static_cast<int>(RS.GetDIInitialCandidates()));
        Out.insert(QStringLiteral("effectiveAnalyticCandidates"),
                   static_cast<int>(DI.EffectiveAnalyticCandidates()));
        Out.insert(QStringLiteral("meshCandidates"),
                   static_cast<int>(RS.GetDIMeshCandidates()));
        Out.insert(QStringLiteral("effectiveMeshCandidates"),
                   static_cast<int>(DI.EffectiveMeshCandidates()));
        Out.insert(QStringLiteral("meshLightsInPool"), RS.GetDIMeshLightsInPool());
        Out.insert(QStringLiteral("effectiveMeshLights"),
                   static_cast<int>(DI.EffectiveMeshLightCount()));
        Out.insert(QStringLiteral("initialVisibility"), RS.GetDIInitialVisibility());
        Out.insert(QStringLiteral("brdfRatio"), RS.GetDIBrdfRatioDemodulation());
        Out.insert(QStringLiteral("meshCompactSupport"), RS.GetDIMeshCompactSupport());
        Out.insert(QStringLiteral("resolution"),
                   ResolutionText(DI.DIRenderWidth(), DI.DIRenderHeight()));
        return Out;
    }

    QVariantMap ReSTIRDebugBridge::GetGI() const {
        QVariantMap Out;
        if (!Renderer) return Out;
        auto Access = Renderer.Lock();
        const auto& RS = Access->Settings();
        const auto& GI = Access->GetReSTIRGI();
        Out.insert(QStringLiteral("enabled"), RS.GetUseReSTIRGI());
        Out.insert(QStringLiteral("ready"), GI.IsReady());
        Out.insert(QStringLiteral("active"), RS.GetUseReSTIRGI() && GI.IsReady());
        Out.insert(QStringLiteral("halfRes"), GI.GetHalfRes());
        Out.insert(QStringLiteral("halfResEffective"), GI.HalfResEffective());
        Out.insert(QStringLiteral("visibility"), RS.GetGIVisibility());
        Out.insert(QStringLiteral("backfacePolicy"), RS.GetGIBackfacePolicy());
        Out.insert(QStringLiteral("sourceDebug"), RS.GetGISourceDebug());
        Out.insert(QStringLiteral("measureTerminatorOff"),
                   RS.GetGIMeasureTerminatorOff());
        Out.insert(QStringLiteral("resolution"),
                   ResolutionText(GI.TraceWidth(), GI.TraceHeight()));
        Out.insert(QStringLiteral("indirectPrimary"),
                   static_cast<int>(RS.GetIndirectPrimary()));
        Out.insert(QStringLiteral("indirectFallback"),
                   static_cast<int>(RS.GetIndirectFallback()));
        return Out;
    }

    QVariantMap ReSTIRDebugBridge::GetTransport() const {
        QVariantMap Out;
        if (!Settings) return Out;
        Out.insert(QStringLiteral("reGIR"), Settings->IsReGIREnabled());
        Out.insert(QStringLiteral("cacheEnabled"), Settings->IsRadianceCacheEnabled());
        Out.insert(QStringLiteral("cacheQuery"), Settings->IsRadianceCacheQuery());
        Out.insert(QStringLiteral("autoWarmup"), Settings->IsRadianceCacheAutoWarmup());
        Out.insert(QStringLiteral("warmup"), Settings->GetRadianceCacheWarmup());
        Out.insert(QStringLiteral("stats"), Settings->IsRadianceCacheStats());
        Out.insert(QStringLiteral("statsDetail"), Settings->IsRadianceCacheStatsDetail());
        Out.insert(QStringLiteral("statsSource"), Settings->IsRadianceCacheStatsSource());
        Out.insert(QStringLiteral("sourceDebug"), Settings->IsGISourceDebug());
        Out.insert(QStringLiteral("dedicatedUpdate"),
                   Settings->IsRadianceCacheDedicatedUpdate());
        Out.insert(QStringLiteral("compactUpdate"),
                   Settings->IsRadianceCacheCompactUpdate());
        Out.insert(QStringLiteral("updateFraction"),
                   Settings->GetRadianceCacheUpdateFraction());
        Out.insert(QStringLiteral("prevTerminal"),
                   Settings->IsRadianceCachePrevTerminal());
        Out.insert(QStringLiteral("maxVertices"), Settings->GetRadianceCacheMaxVertices());
        Out.insert(QStringLiteral("minRoughness"), Settings->GetRadianceCacheMinRoughness());
        Out.insert(QStringLiteral("minSamples"), Settings->GetRadianceCacheMinSamples());
        Out.insert(QStringLiteral("cellSize"), Settings->GetRadianceCacheCellSize());
        Out.insert(QStringLiteral("lodDistance"), Settings->GetRadianceCacheLodDistance());
        Out.insert(QStringLiteral("debugMode"), Settings->GetRadianceCacheDebugMode());
        Out.insert(QStringLiteral("occupancy"), Settings->GetRadianceCacheOccupancy());
        Out.insert(QStringLiteral("hitRate"), Settings->GetRadianceCacheHitRate());
        Out.insert(QStringLiteral("convergence"), Settings->GetRadianceCacheConvergence());
        Out.insert(QStringLiteral("memoryMB"), Settings->GetRadianceCacheMemoryMB());
        Out.insert(QStringLiteral("summary"), Settings->GetRadianceCacheSummary());
        Out.insert(QStringLiteral("sourceBreakdown"),
                   Settings->GetRadianceCacheSourceBreakdown());
        Out.insert(QStringLiteral("missBreakdown"),
                   Settings->GetRadianceCacheMissBreakdown());
        return Out;
    }

    QVariantMap ReSTIRDebugBridge::GetSceneInspector() const {
        QVariantMap Out;
        if (!DebugTargets) return Out;
        Out.insert(QStringLiteral("available"), DebugTargets->IsBvhDebugAvailable());
        Out.insert(QStringLiteral("enabled"), DebugTargets->IsBvhDebugEnabled());
        Out.insert(QStringLiteral("mode"), DebugTargets->GetBvhDebugMode());
        Out.insert(QStringLiteral("rtTimerAvailable"),
                   DebugTargets->IsRtShaderTimerAvailable());
        Out.insert(QStringLiteral("rtTimerEnabled"),
                   DebugTargets->IsRtShaderTimerEnabled());
        Out.insert(QStringLiteral("viewMode"), DebugTargets->GetViewModeLabel());
        return Out;
    }

    QVariantList ReSTIRDebugBridge::GetRayEpsilons() const {
        return Settings ? Settings->GetRayEpsilons() : QVariantList{};
    }

    bool ReSTIRDebugBridge::PixelDebugAvailable() const {
        if (!Renderer || !Viewport) return false;
        auto Access = Renderer.Lock();
        return Access && Access->IsInitialized() &&
               Access->RenderWidth() > 0 && Access->RenderHeight() > 0;
    }

    QString ReSTIRDebugBridge::PixelDebugStatus() const {
        if (!PixelDebugMessage.isEmpty()) return PixelDebugMessage;
        if (!PixelDebugAvailable()) return QStringLiteral("aguardando renderer");
        if (PixelPickArmed) return QStringLiteral("clique no viewport · botão direito cancela");
        if (PixelReadbackPending) return QStringLiteral("aguardando fence da GPU");
        if (!PixelSnapshot.isEmpty()) return QStringLiteral("snapshot pronto");
        return QStringLiteral("pronto para capturar");
    }

    void ReSTIRDebugBridge::ArmPixelDebug() {
        if (!WindowVisible || !VisualizationActive || !PixelDebugAvailable()) return;
        PixelPickArmed = true;
        PixelReadbackPending = false;
        PixelDebugMessage.clear();
        if (Viewport) {
            Viewport->setCursor(Qt::CrossCursor);
            Viewport->setFocus();
        }
        emit Updated();
    }

    void ReSTIRDebugBridge::CancelPixelInteraction(bool _ClearSnapshot) {
        if (Renderer) {
            auto Access = Renderer.Lock();
            if (Access) Access->CancelPixelDebug();
        }
        PixelPickArmed = false;
        PixelReadbackPending = false;
        PixelDebugMessage.clear();
        if (_ClearSnapshot) PixelSnapshot.clear();
        if (Viewport) Viewport->unsetCursor();
    }

    void ReSTIRDebugBridge::ClearPixelDebug() {
        CancelPixelInteraction(true);
        emit Updated();
    }

    bool ReSTIRDebugBridge::eventFilter(QObject* _Watched, QEvent* _Event) {
        if (_Watched != Viewport || !PixelPickArmed ||
            _Event->type() != QEvent::MouseButtonPress) {
            return QObject::eventFilter(_Watched, _Event);
        }
        auto* Mouse = static_cast<QMouseEvent*>(_Event);
        if (Mouse->button() == Qt::RightButton) {
            CancelPixelInteraction(false);
            emit Updated();
            Mouse->accept();
            return true;
        }
        if (Mouse->button() != Qt::LeftButton || !Renderer) return false;

        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) return false;
        const QPointF Position = Mouse->position();
        const Smile::u32 X =
            static_cast<Smile::u32>(std::max(Position.x(), 0.0));
        const Smile::u32 Y =
            static_cast<Smile::u32>(std::max(Position.y(), 0.0));
        PixelPickArmed = false;
        PixelReadbackPending = Access->RequestPixelDebug(X, Y);
        PixelDebugMessage = PixelReadbackPending
            ? QString()
            : QStringLiteral("não foi possível iniciar a captura");
        if (Viewport) {
            Viewport->unsetCursor();
            Viewport->setFocus();
        }
        emit Updated();
        Mouse->accept();
        return true;
    }

    bool ReSTIRDebugBridge::ConsumePixelDebug() {
        if (!Renderer || !PixelReadbackPending) return false;
        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) return false;
        Smile::Renderer::FPixelDebugResult R{};
        if (!Access->ConsumePixelDebug(R)) return false;

        QVariantMap Surface;
        Surface.insert(QStringLiteral("valid"), R.SurfaceValid);
        Surface.insert(QStringLiteral("depth"), Number(R.DeviceDepth, 6));
        Surface.insert(QStringLiteral("worldPosition"), Vec3Text(R.WorldPosition));
        Surface.insert(QStringLiteral("baseColor"), Vec3Text(R.BaseColor));
        Surface.insert(QStringLiteral("normal"), Vec3Text(R.WorldNormal));
        Surface.insert(QStringLiteral("material"),
                       QStringLiteral("%1 · rough %2 · metal %3 · AO %4")
                           .arg(ShadingModelName(R.ShadingModel), Number(R.Roughness),
                                Number(R.Metallic), Number(R.AO)));

        QVariantMap DI;
        DI.insert(QStringLiteral("available"), R.DIAvailable);
        DI.insert(QStringLiteral("valid"), R.DIValid);
        DI.insert(QStringLiteral("output"), Vec4Text(R.DIOutput));
        DI.insert(QStringLiteral("reservoir"),
                  QStringLiteral("W %1 · M %2 · idade %3")
                      .arg(Number(R.DIWeight)).arg(R.DIM).arg(R.DIAge));
        QString Light = QStringLiteral("sem amostra");
        if (R.DILightIndex != 0xFFFFFFFFu) {
            if (R.DILightIndex < R.DIAnalyticLightCount) {
                Light = QStringLiteral("analítica #%1").arg(R.DILightIndex);
            } else if (R.DILightIndex <
                       R.DIAnalyticLightCount + R.DIMeshLightCount) {
                Light = QStringLiteral("mesh #%1")
                    .arg(R.DILightIndex - R.DIAnalyticLightCount);
            } else {
                Light = QStringLiteral("índice fora do pool · %1").arg(R.DILightIndex);
            }
        }
        DI.insert(QStringLiteral("sample"),
                  QStringLiteral("%1 · uv (%2, %3)")
                      .arg(Light, Number(R.DIUV[0], 4), Number(R.DIUV[1], 4)));
        DI.insert(QStringLiteral("normal"), Vec3Text(R.DINormal));

        QVariantMap GI;
        GI.insert(QStringLiteral("available"), R.GIAvailable);
        GI.insert(QStringLiteral("valid"), R.GIValid);
        GI.insert(QStringLiteral("halfRes"), R.GIHalfRes);
        GI.insert(QStringLiteral("tracePixel"),
                  QStringLiteral("reservoir %1,%2 · superfície %3,%4")
                      .arg(R.GITraceX).arg(R.GITraceY)
                      .arg(R.GISurfaceX).arg(R.GISurfaceY));
        GI.insert(QStringLiteral("output"), Vec4Text(R.GIOutput));
        GI.insert(QStringLiteral("reservoir"),
                  QStringLiteral("W %1 · M %2 · idade %3")
                      .arg(Number(R.GIWeight)).arg(R.GIM).arg(R.GIAge));
        GI.insert(QStringLiteral("hit"), Vec3Text(R.GIX2));
        GI.insert(QStringLiteral("radiance"), Vec3Text(R.GIRadiance));
        GI.insert(QStringLiteral("n1"), Vec3Text(R.GIN1));
        GI.insert(QStringLiteral("n2"), Vec3Text(R.GIN2));

        QVariantMap Snapshot;
        Snapshot.insert(QStringLiteral("ready"), true);
        Snapshot.insert(QStringLiteral("pixel"),
                        QStringLiteral("%1, %2").arg(R.ScreenX).arg(R.ScreenY));
        Snapshot.insert(QStringLiteral("renderPixel"),
                        QStringLiteral("%1, %2").arg(R.RenderX).arg(R.RenderY));
        Snapshot.insert(QStringLiteral("frame"), static_cast<int>(R.FrameIndex));
        Snapshot.insert(QStringLiteral("surface"), Surface);
        Snapshot.insert(QStringLiteral("di"), DI);
        Snapshot.insert(QStringLiteral("gi"), GI);
        PixelSnapshot = Snapshot;
        PixelReadbackPending = false;
        PixelDebugMessage.clear();
        emit Updated();
        return true;
    }

    void ReSTIRDebugBridge::Refresh() {
        if (!ConsumePixelDebug()) emit Updated();
    }
    void ReSTIRDebugBridge::OpenWindow() { emit OpenWindowRequested(); }

    void ReSTIRDebugBridge::ToggleDI() {
        if (Settings) Settings->ToggleReSTIRDI();
    }
    void ReSTIRDebugBridge::SetDIAnalyticCandidates(int _Value) {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); A->Settings().SetDIInitialCandidates(
              static_cast<Smile::u32>(std::clamp(_Value, 1, 64))); }
        emit Updated();
    }
    void ReSTIRDebugBridge::SetDIMeshCandidates(int _Value) {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); A->Settings().SetDIMeshCandidates(
              static_cast<Smile::u32>(std::clamp(_Value, 0, 64))); }
        emit Updated();
    }
    void ReSTIRDebugBridge::ToggleDIMeshLights() {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); auto& S = A->Settings();
          S.SetDIMeshLightsInPool(!S.GetDIMeshLightsInPool()); }
        emit Updated();
    }
    void ReSTIRDebugBridge::ToggleDIInitialVisibility() {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); auto& S = A->Settings();
          S.SetDIInitialVisibility(!S.GetDIInitialVisibility()); }
        emit Updated();
    }
    void ReSTIRDebugBridge::ToggleDIBrdfRatio() {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); auto& S = A->Settings();
          S.SetDIBrdfRatioDemodulation(!S.GetDIBrdfRatioDemodulation()); }
        emit Updated();
    }
    void ReSTIRDebugBridge::ToggleDIMeshCompactSupport() {
        if (!Renderer) return;
        { auto A = Renderer.Lock(); auto& S = A->Settings();
          S.SetDIMeshCompactSupport(!S.GetDIMeshCompactSupport()); }
        emit Updated();
    }

    void ReSTIRDebugBridge::SetIndirectPrimary(int _Value) {
        if (Settings) Settings->SetIndirectPrimary(_Value);
    }
    void ReSTIRDebugBridge::SetIndirectFallback(int _Value) {
        if (Settings) Settings->SetIndirectFallback(_Value);
    }
    void ReSTIRDebugBridge::ToggleGI() {
        if (Settings) Settings->ToggleReSTIRGI();
    }
    void ReSTIRDebugBridge::ToggleGIHalfRes() {
        if (Settings) Settings->ToggleReSTIRGIHalfRes();
    }
    void ReSTIRDebugBridge::ToggleGIVisibility() {
        if (Settings) Settings->ToggleReSTIRGIVisibility();
    }
    void ReSTIRDebugBridge::ToggleGIBackfacePolicy() {
        if (Settings) Settings->ToggleGIBackfacePolicy();
    }
    void ReSTIRDebugBridge::ToggleGISourceDebug() {
        if (Settings) Settings->ToggleGISourceDebug();
    }
    void ReSTIRDebugBridge::ToggleGIMeasureTerminatorOff() {
        if (Settings) Settings->ToggleGIMeasureTerminatorOff();
    }

    void ReSTIRDebugBridge::ToggleReGIR() {
        if (Settings) Settings->ToggleReGIR();
    }
    void ReSTIRDebugBridge::ToggleRadianceCache() {
        if (Settings) Settings->ToggleRadianceCache();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheQuery() {
        if (Settings) Settings->ToggleRadianceCacheQuery();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheAutoWarmup() {
        if (Settings) Settings->ToggleRadianceCacheAutoWarmup();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheStats() {
        if (Settings) Settings->ToggleRadianceCacheStats();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheStatsDetail() {
        if (Settings) Settings->ToggleRadianceCacheStatsDetail();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheStatsSource() {
        if (Settings) Settings->ToggleRadianceCacheStatsSource();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheDedicatedUpdate() {
        if (Settings) Settings->ToggleRadianceCacheDedicatedUpdate();
    }
    void ReSTIRDebugBridge::ToggleRadianceCacheCompactUpdate() {
        if (Settings) Settings->ToggleRadianceCacheCompactUpdate();
    }
    void ReSTIRDebugBridge::ToggleRadianceCachePrevTerminal() {
        if (Settings) Settings->ToggleRadianceCachePrevTerminal();
    }
    void ReSTIRDebugBridge::SetRadianceCacheUpdateFraction(double _Value) {
        if (Settings) Settings->SetRadianceCacheUpdateFraction(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheMaxVertices(int _Value) {
        if (Settings) Settings->SetRadianceCacheMaxVertices(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheMinRoughness(double _Value) {
        if (Settings) Settings->SetRadianceCacheMinRoughness(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheMinSamples(int _Value) {
        if (Settings) Settings->SetRadianceCacheMinSamples(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheCellSize(double _Value) {
        if (Settings) Settings->SetRadianceCacheCellSize(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheLodDistance(double _Value) {
        if (Settings) Settings->SetRadianceCacheLodDistance(_Value);
    }
    void ReSTIRDebugBridge::SetRadianceCacheDebugMode(int _Value) {
        if (Settings) Settings->SetRadianceCacheDebugMode(_Value);
    }
    void ReSTIRDebugBridge::ResetRadianceCache() {
        if (Settings) Settings->ResetRadianceCache();
    }

    void ReSTIRDebugBridge::SetRayEpsilon(const QString& _Key, double _Value) {
        if (Settings) Settings->SetRayEpsilon(_Key, _Value);
    }
    void ReSTIRDebugBridge::ResetRayEpsilons() {
        if (Settings) Settings->ResetRayEpsilons();
    }

    void ReSTIRDebugBridge::ToggleSceneInspector() {
        if (DebugTargets) DebugTargets->ToggleBvhDebug();
    }
    void ReSTIRDebugBridge::SetSceneInspectorMode(int _Mode) {
        if (DebugTargets) DebugTargets->SetBvhDebugMode(_Mode);
    }
    void ReSTIRDebugBridge::ToggleRtShaderTimer() {
        if (DebugTargets) DebugTargets->ToggleRtShaderTimer();
    }
}
