#pragma once

#include <QObject>
#include <QVariantList>
#include "SmileEditor/Viewport/RenderThread.h"

namespace Smile {
    // So o parametro por referencia precisa do tipo aqui; a definicao chega pelo Renderer.h no
    // .cpp. Mesmo criterio do FRenderSettings, que declara os irmaos dele da mesma forma.
    struct FRadianceCacheSnapshot;
}

namespace SmileEditor {
    class ViewportWidget;

    // Ponte C++<->QML dos KNOBS DE RENDER (paginas do SettingsWindow e parte do
    // ViewportToolbar). Espelha, do lado do editor, o que o FRenderSettings fez do lado do
    // motor: um caminho unico para parametro de render.
    //
    // Antes disto os ~92 knobs eram Q_PROPERTY do ViewportWidget, que acumulava host do HWND,
    // input, telemetria, visualizador de debug E a bridge de render — o espelho do god object
    // (ver §13 do Docs/ARCHITECTURE.md). Ficam de proposito no ViewportWidget, porque nao sao
    // knobs: view mode, visualizador de render targets, instrumentacao (BVH/timer de RT) e toda
    // a telemetria (fps, contagens, VRAM, timings de GPU).
    //
    // Leitura e escrita sincronas passam pelo RendererHandle, que serializa com a thread de
    // render — mesmo padrao do TimeOfDayBridge. Os poucos knobs que precisam recriar recursos
    // (render scale e troca de upscaler/denoiser) vao por job assincrono na
    // fila do ViewportWidget, que os serializa com os frames; e por isso que a bridge conhece o
    // viewport.
    class RenderSettingsBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)

        // --- Sun shafts e fog volumetrico -----------------------------------------------
        Q_PROPERTY(bool sunShaftsEnabled READ AreSunShaftsEnabled NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(double sunShaftsIntensity READ GetSunShaftsIntensity NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(double sunShaftsDust READ GetSunShaftsDust NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(double sunShaftsPhaseG READ GetSunShaftsPhaseG NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(int sunShaftsSteps READ GetSunShaftsSteps NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(double sunShaftsRange READ GetSunShaftsRange NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(bool sunShaftsTemporal READ AreSunShaftsTemporal NOTIFY SunShaftsSettingsChanged)
        Q_PROPERTY(bool volFogEnabled READ IsVolFogEnabled NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double volFogDistance READ GetVolFogDistance NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double volFogPhaseG READ GetVolFogPhaseG NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double volFogDensity READ GetVolFogDensity NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double volFogAmbient READ GetVolFogAmbient NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(bool volFogTemporal READ IsVolFogTemporal NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double volFogLights READ GetVolFogLights NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(bool volFogConsDepth READ IsVolFogConsDepth NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(double heightFogSkyContribution READ GetHeightFogSkyContribution NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(bool perPixelAtmoTransmittance READ IsPerPixelAtmoTransmittance NOTIFY VolFogSettingsChanged)
        Q_PROPERTY(bool skyAmbientSH READ IsSkyAmbientSH NOTIFY VolFogSettingsChanged)

        // --- Sombras do sol -------------------------------------------------------------
        Q_PROPERTY(bool sunShadowsEnabled READ AreSunShadowsEnabled NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(bool shadowCacheEnabled READ IsShadowCacheEnabled NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(bool shadowDebugCascades READ IsShadowDebugCascades NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(double shadowMaxDistance READ GetShadowMaxDistance NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(double shadowDepthBias READ GetShadowDepthBias NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(double shadowNormalOffset READ GetShadowNormalOffset NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(double shadowMinCasterTexels READ GetShadowMinCasterTexels NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(QVariantList shadowCascadeBias READ GetShadowCascadeBias NOTIFY ShadowSettingsChanged)
        Q_PROPERTY(double shadowSunAngle READ GetShadowSunAngle NOTIFY ShadowSettingsChanged)

        // --- Iluminacao global ----------------------------------------------------------
        Q_PROPERTY(bool ddgiEnabled READ IsDDGIEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool restirGIEnabled READ IsReSTIRGIEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool restirGIHalfRes READ IsReSTIRGIHalfRes NOTIFY GISettingsChanged)
        Q_PROPERTY(bool reGIREnabled READ IsReGIREnabled NOTIFY GISettingsChanged)
        // World radiance cache. As tres primeiras sao knobs; as cinco ultimas, telemetria do
        // readback (ver FRadianceCacheStats) — por isso so tem READ.
        Q_PROPERTY(bool radianceCacheEnabled READ IsRadianceCacheEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool radianceCacheQuery READ IsRadianceCacheQuery NOTIFY GISettingsChanged)
        // Aquecimento automatico: knob (bool) + o estado em que ele esta, que e telemetria e muda
        // sozinho — por isso o estado sai por StatsChanged e nao por GISettingsChanged.
        Q_PROPERTY(bool radianceCacheAutoWarmup READ IsRadianceCacheAutoWarmup
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(QString radianceCacheWarmup READ GetRadianceCacheWarmup NOTIFY StatsChanged)
        Q_PROPERTY(bool radianceCacheStats READ IsRadianceCacheStats NOTIFY GISettingsChanged)
        Q_PROPERTY(bool radianceCacheStatsDetail READ IsRadianceCacheStatsDetail
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(bool radianceCacheStatsSource READ IsRadianceCacheStatsSource
                       NOTIFY GISettingsChanged)
        // Mapa por pixel da fonte do candidato tracado. Alvo proprio na janela de debug — o
        // contador diz QUANTO, este diz ONDE.
        Q_PROPERTY(bool giSourceDebug READ IsGISourceDebug NOTIFY GISettingsChanged)
        // Decomposicao da FONTE do terminal — texto pronto, como o breakdown de miss acima e pelo
        // mesmo motivo: a regra de "zero medido x nao medido" mora do lado do C++, onde o knob esta.
        Q_PROPERTY(QString radianceCacheSourceBreakdown READ GetRadianceCacheSourceBreakdown
                       NOTIFY StatsChanged)
        // Decomposicao dos misses e saude do hash — texto pronto, e nao dez propriedades: o painel
        // nao tem o que fazer com os numeros crus alem de imprimi-los lado a lado, e a regra de
        // "zero medido x nao medido" mora do lado do C++, onde o knob esta.
        Q_PROPERTY(QString radianceCacheMissBreakdown READ GetRadianceCacheMissBreakdown
                       NOTIFY StatsChanged)
        Q_PROPERTY(bool radianceCacheDedicatedUpdate READ IsRadianceCacheDedicatedUpdate
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(bool radianceCacheCompactUpdate READ IsRadianceCacheCompactUpdate
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(double radianceCacheUpdateFraction READ GetRadianceCacheUpdateFraction
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(bool radianceCachePrevTerminal READ IsRadianceCachePrevTerminal
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(int radianceCacheMaxVertices READ GetRadianceCacheMaxVertices
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(double radianceCacheMinRoughness READ GetRadianceCacheMinRoughness
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(int radianceCacheMinSamples READ GetRadianceCacheMinSamples
                       NOTIFY GISettingsChanged)
        Q_PROPERTY(double radianceCacheCellSize READ GetRadianceCacheCellSize NOTIFY GISettingsChanged)
        Q_PROPERTY(double radianceCacheLodDistance READ GetRadianceCacheLodDistance NOTIFY GISettingsChanged)
        Q_PROPERTY(int radianceCacheDebugMode READ GetRadianceCacheDebugMode NOTIFY GISettingsChanged)
        Q_PROPERTY(double radianceCacheOccupancy READ GetRadianceCacheOccupancy NOTIFY StatsChanged)
        Q_PROPERTY(double radianceCacheHitRate READ GetRadianceCacheHitRate NOTIFY StatsChanged)
        Q_PROPERTY(double radianceCacheConvergence READ GetRadianceCacheConvergence NOTIFY StatsChanged)
        Q_PROPERTY(double radianceCacheMemoryMB READ GetRadianceCacheMemoryMB NOTIFY GISettingsChanged)
        Q_PROPERTY(QString radianceCacheSummary READ GetRadianceCacheSummary NOTIFY StatsChanged)
        // Politica do indireto: primario e fallback, PEDIDO e EFETIVO. Sai por GISettingsChanged
        // porque o pedido e knob; o efetivo tambem muda com disponibilidade do volume, e ai quem
        // atualiza e o Timer do painel.
        Q_PROPERTY(QString indirectPolicySummary READ GetIndirectPolicySummary
                       NOTIFY GISettingsChanged)
        // O PEDIDO, que e o que o combo edita. O efetivo nao vira propriedade separada de
        // proposito: ele so significa alguma coisa ao lado do pedido, e e o summary acima que
        // mostra os dois com a seta na divergencia. Publicar o efetivo sozinho convidaria a UI a
        // exibi-lo como se fosse a selecao — e ai selecionar SHaRC sem o passe pronto pareceria um
        // combo que volta sozinho.
        // 0 = ReSTIR+SHaRC, 1 = DDGI, 2 = Off  (ordem de EIndirectPrimary)
        Q_PROPERTY(int indirectPrimary READ GetIndirectPrimary NOTIFY GISettingsChanged)
        // 0 = DDGI, 1 = Environment, 2 = Black  (ordem de EIndirectFallback)
        Q_PROPERTY(int indirectFallback READ GetIndirectFallback NOTIFY GISettingsChanged)
        Q_PROPERTY(bool restirGIVisibilityEnabled READ IsReSTIRGIVisibilityEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool giFoliageShadows READ AreGIFoliageShadowsEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool reflectionsCullBackface READ IsReflectionsCullBackfaceEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool giBackfacePolicy READ IsGIBackfacePolicyEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool giAdaptiveHysteresis READ IsGIAdaptiveHysteresisEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(bool giAdaptiveRays READ IsGIAdaptiveRaysEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(int giCascadeCount READ GetGICascadeCount NOTIFY GISettingsChanged)
        Q_PROPERTY(bool giMeasureTerminatorOff READ IsGIMeasureTerminatorOff NOTIFY GISettingsChanged)
        Q_PROPERTY(bool reSTIRDI READ IsReSTIRDIEnabled NOTIFY GISettingsChanged)
        Q_PROPERTY(double giSurfaceBiasMax READ GetGISurfaceBiasMax NOTIFY GISettingsChanged)
        Q_PROPERTY(double giVolumeFadeProbes READ GetGIVolumeFadeProbes NOTIFY GISettingsChanged)
        Q_PROPERTY(QVariantList rayEpsilons READ GetRayEpsilons NOTIFY GISettingsChanged)

        // --- Render, upscaler e denoiser -------------------------------------------------
        Q_PROPERTY(bool gtaoEnabled READ IsGTAOEnabled NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool gtaoHalfRes READ IsGTAOHalfRes NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool reflectionsEnabled READ AreReflectionsEnabled NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool nrdEnabled READ IsNrdEnabled NOTIFY RenderSettingsChanged)
        Q_PROPERTY(int denoiserMode READ GetDenoiserMode NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool rrAvailable READ IsRRAvailable NOTIFY RendererInitialized)
        Q_PROPERTY(int upscalerMode READ GetUpscalerMode NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool fsrAvailable READ IsFsrAvailable NOTIFY RendererInitialized)
        Q_PROPERTY(bool dlssAvailable READ IsDlssAvailable NOTIFY RendererInitialized)
        Q_PROPERTY(int upscalerQuality READ GetUpscalerQuality NOTIFY RenderSettingsChanged)
        Q_PROPERTY(int recommendedUpscalerMode READ GetRecommendedUpscalerMode NOTIFY RendererInitialized)
        Q_PROPERTY(QString recommendedUpscalerName READ GetRecommendedUpscalerName NOTIFY RendererInitialized)
        Q_PROPERTY(double renderScale READ GetRenderScale NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool frustumCullingEnabled READ IsFrustumCullingEnabled NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool occlusionCullingEnabled READ IsOcclusionCullingEnabled NOTIFY RenderSettingsChanged)
        Q_PROPERTY(bool depthPrepassEnabled READ IsDepthPrepassEnabled NOTIFY RenderSettingsChanged)

    public:
        explicit RenderSettingsBridge(QObject* parent = nullptr);

        // MainWindow chama no RendererInitialized (mesmo ponto do TimeOfDayBridge).
        void SetRenderer(RendererHandle R);
        // Fila de jobs serializada com os frames, para os knobs que recriam recursos.
        void SetViewport(ViewportWidget* V) { Viewport = V; }

        bool Available() const { return static_cast<bool>(Renderer); }

        bool              AreSunShaftsEnabled() const;
        double            GetSunShaftsIntensity() const;
        double            GetSunShaftsDust() const;
        double            GetSunShaftsPhaseG() const;
        int               GetSunShaftsSteps() const;
        double            GetSunShaftsRange() const;
        bool              AreSunShaftsTemporal() const;
        bool              IsVolFogEnabled() const;
        double            GetVolFogDistance() const;
        double            GetVolFogPhaseG() const;
        double            GetVolFogDensity() const;
        double            GetVolFogAmbient() const;
        double            GetHeightFogSkyContribution() const;
        bool              IsPerPixelAtmoTransmittance() const;
        bool              IsSkyAmbientSH() const;
        bool              IsVolFogTemporal() const;
        double            GetVolFogLights() const;
        bool              IsVolFogConsDepth() const;

        Q_INVOKABLE void SetSunShaftsEnabled(bool enabled);
        Q_INVOKABLE void SetSunShaftsIntensity(double value);
        Q_INVOKABLE void SetSunShaftsDust(double value);
        Q_INVOKABLE void SetSunShaftsPhaseG(double value);
        Q_INVOKABLE void SetSunShaftsSteps(int value);
        Q_INVOKABLE void SetSunShaftsRange(double value);
        Q_INVOKABLE void SetSunShaftsTemporal(bool enabled);
        Q_INVOKABLE void SetVolFogEnabled(bool enabled);
        Q_INVOKABLE void SetVolFogDistance(double value);
        Q_INVOKABLE void SetVolFogPhaseG(double value);
        Q_INVOKABLE void SetVolFogDensity(double value);
        Q_INVOKABLE void SetVolFogAmbient(double value);
        Q_INVOKABLE void SetHeightFogSkyContribution(double value);
        Q_INVOKABLE void SetPerPixelAtmoTransmittance(bool enabled);
        Q_INVOKABLE void SetSkyAmbientSH(bool enabled);

        bool              AreSunShadowsEnabled() const;
        bool              IsShadowCacheEnabled() const;
        bool              IsShadowDebugCascades() const;
        double            GetShadowMaxDistance() const;
        double            GetShadowDepthBias() const;
        double            GetShadowNormalOffset() const;
        double            GetShadowMinCasterTexels() const;
        QVariantList      GetShadowCascadeBias() const;
        double            GetShadowSunAngle() const;

        Q_INVOKABLE void SetSunShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetShadowCacheEnabled(bool enabled);
        Q_INVOKABLE void SetShadowDebugCascades(bool enabled);
        Q_INVOKABLE void SetShadowMaxDistance(double distance);
        Q_INVOKABLE void SetShadowDepthBias(double bias);
        Q_INVOKABLE void SetShadowNormalOffset(double texels);
        Q_INVOKABLE void SetShadowMinCasterTexels(double texels);
        Q_INVOKABLE void SetShadowCascadeBiasScale(int cascade, double scale);
        Q_INVOKABLE void SetShadowSunAngle(double degrees);

        bool              IsDDGIEnabled() const;
        bool              IsReSTIRGIEnabled() const;
        bool              IsReSTIRGIHalfRes() const;
        bool              IsReGIREnabled() const;
        bool              IsRadianceCacheEnabled() const;
        bool              IsRadianceCacheQuery() const;
        bool              IsRadianceCacheAutoWarmup() const;
        QString           GetRadianceCacheWarmup() const;
        bool              IsRadianceCacheStats() const;
        bool              IsRadianceCacheStatsDetail() const;
        bool              IsRadianceCacheStatsSource() const;
        bool              IsGISourceDebug() const;
        QString           GetRadianceCacheSourceBreakdown() const;
        // Contadores + meta + capacidade numa leitura SO, por valor e sob um lock so. Falso quando
        // nao ha snapshot valido. Todo getter de telemetria passa por aqui: pedir as partes em
        // chamadas separadas atravessa o lock temporario do RendererHandle e mistura frames.
        // Nao e Q_PROPERTY de proposito — quem decide se a linha aparece e o TEXTO estar vazio.
        bool              CacheSnapshot(Smile::FRadianceCacheSnapshot& Out) const;
        bool              IsRadianceCacheDedicatedUpdate() const;
        bool              IsRadianceCacheCompactUpdate() const;
        double            GetRadianceCacheUpdateFraction() const; // 0..1
        bool              IsRadianceCachePrevTerminal() const;
        int               GetRadianceCacheMaxVertices() const;
        double            GetRadianceCacheMinRoughness() const;
        int               GetRadianceCacheMinSamples() const;
        double            GetRadianceCacheCellSize() const;
        double            GetRadianceCacheLodDistance() const;
        int               GetRadianceCacheDebugMode() const;
        double            GetRadianceCacheOccupancy() const;    // % da capacidade
        double            GetRadianceCacheHitRate() const;      // % de acerto das consultas
        double            GetRadianceCacheConvergence() const;  // amostras/celula, 0..kMaxAccum
        double            GetRadianceCacheMemoryMB() const;
        QString           GetRadianceCacheSummary() const;
        QString           GetIndirectPolicySummary() const;
        QString           GetRadianceCacheMissBreakdown() const;
        bool              IsReSTIRGIVisibilityEnabled() const;
        bool              AreGIFoliageShadowsEnabled() const;
        bool              IsReflectionsCullBackfaceEnabled() const;
        bool              IsGIBackfacePolicyEnabled() const;
        bool              IsGIAdaptiveHysteresisEnabled() const;
        bool              IsGIAdaptiveRaysEnabled() const;
        int               GetGICascadeCount() const;
        bool              IsGIMeasureTerminatorOff() const;
        bool              IsReSTIRDIEnabled() const;
        int               GetIndirectPrimary() const;
        int               GetIndirectFallback() const;
        double            GetGISurfaceBiasMax() const;
        double            GetGIVolumeFadeProbes() const;
        QVariantList      GetRayEpsilons() const;

        // Politica do indireto. Indices de EIndirectPrimary/EIndirectFallback; fora da faixa nao
        // faz nada, em vez de cair num default silencioso.
        Q_INVOKABLE void SetIndirectPrimary(int V);
        Q_INVOKABLE void SetIndirectFallback(int V);

        Q_INVOKABLE void ToggleDDGI();
        Q_INVOKABLE void ToggleReSTIRGI();
        Q_INVOKABLE void ToggleReSTIRGIHalfRes();
        Q_INVOKABLE void ToggleReGIR();
        Q_INVOKABLE void ToggleRadianceCache();
        Q_INVOKABLE void ToggleRadianceCacheQuery();
        Q_INVOKABLE void ToggleRadianceCacheAutoWarmup();
        Q_INVOKABLE void ToggleRadianceCacheStats();
        Q_INVOKABLE void ToggleRadianceCacheStatsDetail();
        Q_INVOKABLE void ToggleRadianceCacheStatsSource();
        Q_INVOKABLE void ToggleGISourceDebug();
        Q_INVOKABLE void ToggleRadianceCacheDedicatedUpdate();
        Q_INVOKABLE void ToggleRadianceCacheCompactUpdate();
        Q_INVOKABLE void SetRadianceCacheUpdateFraction(double V);
        Q_INVOKABLE void ToggleRadianceCachePrevTerminal();
        Q_INVOKABLE void SetRadianceCacheMaxVertices(int V);
        Q_INVOKABLE void SetRadianceCacheMinRoughness(double V);
        Q_INVOKABLE void SetRadianceCacheMinSamples(int V);
        Q_INVOKABLE void SetRadianceCacheCellSize(double V);
        Q_INVOKABLE void SetRadianceCacheLodDistance(double V);
        Q_INVOKABLE void SetRadianceCacheDebugMode(int V);
        Q_INVOKABLE void ResetRadianceCache();
        // Puxada pelo Timer do card enquanto ele esta visivel — ver o sinal StatsChanged.
        Q_INVOKABLE void RefreshRadianceCacheStats();
        Q_INVOKABLE void ToggleReSTIRGIVisibility();
        Q_INVOKABLE void ToggleGIFoliageShadows();
        Q_INVOKABLE void ToggleReflectionsCullBackface();
        Q_INVOKABLE void ToggleGIBackfacePolicy();
        Q_INVOKABLE void ToggleGIAdaptiveHysteresis();
        Q_INVOKABLE void ToggleGIAdaptiveRays();
        Q_INVOKABLE void SetGICascadeCount(int V);
        Q_INVOKABLE void ToggleGIMeasureTerminatorOff();
        Q_INVOKABLE void ToggleReSTIRDI();
        Q_INVOKABLE void SetGISurfaceBiasMax(double meters);
        Q_INVOKABLE void SetGIVolumeFadeProbes(double probes);
        Q_INVOKABLE void SetRayEpsilon(const QString& key, double uiValue);
        Q_INVOKABLE void ResetRayEpsilons();

        bool              IsGTAOEnabled() const;
        bool              IsGTAOHalfRes() const;
        bool              AreReflectionsEnabled() const;
        bool              IsNrdEnabled() const;
        int               GetDenoiserMode() const;   // 0=Nenhum 1=NRD 2=DLSS Ray Reconstruction
        bool              IsRRAvailable() const;      // DLSS RR suportado (NVIDIA RTX + SDK)
        int               GetUpscalerMode() const;
        bool              IsFsrAvailable() const;
        bool              IsDlssAvailable() const;
        int               GetUpscalerQuality() const;
        int               GetRecommendedUpscalerMode() const;
        QString           GetRecommendedUpscalerName() const;
        double            GetRenderScale() const;
        bool              IsTAAEnabled() const;
        bool              IsFrustumCullingEnabled() const;
        bool              IsOcclusionCullingEnabled() const;
        bool              IsDepthPrepassEnabled() const;

        Q_INVOKABLE void ToggleGTAO();
        Q_INVOKABLE void ToggleGTAOHalfRes();
        Q_INVOKABLE void ToggleReflections();
        Q_INVOKABLE void ToggleNrd();
        Q_INVOKABLE void SetDenoiserMode(int mode);   // 0=Nenhum 1=NRD 2=DLSS RR (RR acopla upscaler=DLSS)
        Q_INVOKABLE void SetUpscalerMode(int mode);
        Q_INVOKABLE void SetUpscalerQuality(int quality);
        Q_INVOKABLE void SetRenderScale(double scale);
        Q_INVOKABLE void SetTAAEnabled(bool enabled);
        Q_INVOKABLE void SetFrustumCullingEnabled(bool enabled);
        Q_INVOKABLE void SetOcclusionCullingEnabled(bool enabled);
        Q_INVOKABLE void SetDepthPrepassEnabled(bool enabled);
        Q_INVOKABLE void ResetRenderSettings();
        Q_INVOKABLE void SetVolFogTemporal(bool enabled);
        Q_INVOKABLE void SetVolFogLights(double value);
        Q_INVOKABLE void SetVolFogConsDepth(bool enabled);
    signals:
        void AvailableChanged();
        void SunShaftsSettingsChanged();
        void VolFogSettingsChanged();
        void ShadowSettingsChanged();
        void GISettingsChanged();
        // Telemetria que muda TODO frame (readback do radiance cache). Nao e emitido pelo
        // renderer: o card do painel puxa com um Timer proprio enquanto esta visivel, entao
        // formatar QString a 5 Hz so acontece com alguem olhando.
        void StatsChanged();
        void RenderSettingsChanged();
        // Capacidades de upscaler/denoiser: so mudam quando o renderer nasce.
        void RendererInitialized();

    private:
        RendererHandle  Renderer;
        ViewportWidget* Viewport = nullptr;
    };
}
