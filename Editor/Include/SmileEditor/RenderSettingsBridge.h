#pragma once

#include <QObject>
#include <QVariantList>
#include "SmileEditor/RenderThread.h"

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
    // (render scale, half-res das nuvens, troca de upscaler/denoiser) vao por job assincrono na
    // fila do ViewportWidget, que os serializa com os frames; e por isso que a bridge conhece o
    // viewport.
    class RenderSettingsBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)

        // --- Oceano / agua -------------------------------------------------------------
        Q_PROPERTY(bool oceanEnabled READ IsOceanEnabled NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanWindDirectionDegrees READ GetOceanWindDirectionDegrees NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanWindSpeed READ GetOceanWindSpeed NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanFetchKm READ GetOceanFetchKm NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanDepthM READ GetOceanDepthM NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanSwell READ GetOceanSwell NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanWavesAmount READ GetOceanWavesAmount NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanFFTDisplacement READ GetOceanFFTDisplacement NOTIFY OceanSettingsChanged)
        Q_PROPERTY(double oceanFFTChoppy READ GetOceanFFTChoppy NOTIFY OceanSettingsChanged)

        // --- Clima (FWeather) ----------------------------------------------------------
        Q_PROPERTY(double rainAmount READ GetRainAmount NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(double puddleAmount READ GetPuddleAmount NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(double puddleScale READ GetPuddleScale NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(double rippleStrength READ GetRippleStrength NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(double wetDarkening READ GetWetDarkening NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(double curtainAmount READ GetCurtainAmount NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(bool rainOcclusion READ IsRainOcclusion NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(bool rainParticles READ AreRainParticles NOTIFY WeatherSettingsChanged)
        Q_PROPERTY(bool weatherDriveSky READ IsWeatherDriveSky NOTIFY WeatherSettingsChanged)

        // --- Nuvens volumetricas -------------------------------------------------------
        Q_PROPERTY(bool cloudsEnabled READ AreCloudsEnabled NOTIFY CloudSettingsChanged)
        Q_PROPERTY(bool cloudsHalfRes READ AreCloudsHalfRes NOTIFY CloudSettingsChanged)
        Q_PROPERTY(bool cloudsTemporal READ AreCloudsTemporal NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudCoverage READ GetCloudCoverage NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudDensity READ GetCloudDensity NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudWindSpeed READ GetCloudWindSpeed NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudErosion READ GetCloudErosion NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudPhaseG READ GetCloudPhaseG NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudPowder READ GetCloudPowder NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudAmbient READ GetCloudAmbient NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudTypeBias READ GetCloudTypeBias NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudPeakVariation READ GetCloudPeakVariation NOTIFY CloudSettingsChanged)
        Q_PROPERTY(int cloudWeatherSeed READ GetCloudWeatherSeed NOTIFY CloudSettingsChanged)
        Q_PROPERTY(int cloudWeatherCells READ GetCloudWeatherCells NOTIFY CloudSettingsChanged)
        Q_PROPERTY(bool cloudWeatherAuthored READ IsCloudWeatherAuthored NOTIFY CloudSettingsChanged)
        Q_PROPERTY(bool cloudShadows READ AreCloudShadowsEnabled NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudShadowStrength READ GetCloudShadowStrength NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudBottomKm READ GetCloudBottomKm NOTIFY CloudSettingsChanged)
        Q_PROPERTY(double cloudThicknessKm READ GetCloudThicknessKm NOTIFY CloudSettingsChanged)
        Q_PROPERTY(int cloudMarchSteps READ GetCloudMarchSteps NOTIFY CloudSettingsChanged)

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

        bool   IsOceanEnabled() const;
        double GetOceanWindDirectionDegrees() const;
        double GetOceanWindSpeed() const;
        double GetOceanFetchKm() const;
        double GetOceanDepthM() const;
        double GetOceanSwell() const;
        double GetOceanWavesAmount() const;
        double GetOceanFFTDisplacement() const;
        double GetOceanFFTChoppy() const;

        Q_INVOKABLE void SetOceanEnabled(bool enabled);
        Q_INVOKABLE void SetOceanWindDirectionDegrees(double degrees);
        Q_INVOKABLE void SetOceanWindSpeed(double metresPerSecond);
        Q_INVOKABLE void SetOceanFetchKm(double kilometres);
        Q_INVOKABLE void SetOceanDepthM(double metres);
        Q_INVOKABLE void SetOceanSwell(double value);
        Q_INVOKABLE void SetOceanWavesAmount(double value);
        Q_INVOKABLE void SetOceanFFTDisplacement(double value);
        Q_INVOKABLE void SetOceanFFTChoppy(double value);

        double GetRainAmount() const;
        double GetPuddleAmount() const;
        double GetPuddleScale() const;
        double GetRippleStrength() const;
        double GetWetDarkening() const;
        double GetCurtainAmount() const;
        bool   IsRainOcclusion() const;
        bool   AreRainParticles() const;
        bool   IsWeatherDriveSky() const;

        Q_INVOKABLE void SetRainAmount(double value);
        Q_INVOKABLE void SetPuddleAmount(double value);
        Q_INVOKABLE void SetPuddleScale(double value);
        Q_INVOKABLE void SetRippleStrength(double value);
        Q_INVOKABLE void SetWetDarkening(double value);
        Q_INVOKABLE void SetCurtainAmount(double value);
        Q_INVOKABLE void SetRainOcclusion(bool enabled);
        Q_INVOKABLE void SetRainParticles(bool enabled);
        Q_INVOKABLE void SetWeatherDriveSky(bool enabled);

        bool              AreCloudsEnabled() const;
        bool              AreCloudsHalfRes() const;
        bool              AreCloudsTemporal() const;
        double            GetCloudCoverage() const;
        double            GetCloudDensity() const;
        double            GetCloudWindSpeed() const;
        double            GetCloudErosion() const;
        double            GetCloudPhaseG() const;
        double            GetCloudPowder() const;
        double            GetCloudAmbient() const;
        double            GetCloudTypeBias() const;
        double            GetCloudPeakVariation() const;
        int               GetCloudWeatherSeed() const;
        int               GetCloudWeatherCells() const;
        bool              IsCloudWeatherAuthored() const;
        bool              AreCloudShadowsEnabled() const;
        double            GetCloudShadowStrength() const;
        double            GetCloudBottomKm() const;
        double            GetCloudThicknessKm() const;
        int               GetCloudMarchSteps() const;

        Q_INVOKABLE void SetCloudsEnabled(bool enabled);
        Q_INVOKABLE void SetCloudsHalfRes(bool halfRes);
        Q_INVOKABLE void SetCloudsTemporal(bool enabled);
        Q_INVOKABLE void SetCloudCoverage(double value);
        Q_INVOKABLE void SetCloudDensity(double value);
        Q_INVOKABLE void SetCloudWindSpeed(double value);
        Q_INVOKABLE void SetCloudErosion(double value);
        Q_INVOKABLE void SetCloudPhaseG(double value);
        Q_INVOKABLE void SetCloudPowder(double value);
        Q_INVOKABLE void SetCloudAmbient(double value);
        Q_INVOKABLE void SetCloudTypeBias(double value);
        Q_INVOKABLE void SetCloudPeakVariation(double value);
        Q_INVOKABLE void SetCloudWeatherSeed(int seed);
        Q_INVOKABLE void SetCloudWeatherCells(int mult);
        Q_INVOKABLE void LoadCloudWeatherTexture();  // abre QFileDialog
        Q_INVOKABLE void ClearCloudWeatherTexture();

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
        bool              IsReGIREnabled() const;
        bool              IsRadianceCacheEnabled() const;
        bool              IsRadianceCacheQuery() const;
        bool              IsRadianceCacheAutoWarmup() const;
        QString           GetRadianceCacheWarmup() const;
        bool              IsRadianceCacheStats() const;
        bool              IsRadianceCacheStatsDetail() const;
        bool              IsRadianceCacheStatsSource() const;
        QString           GetRadianceCacheSourceBreakdown() const;
        // Regime de medicao EFETIVO (cache participando + instrumentacao-base). Regra unica das
        // linhas de telemetria; ver o corpo. Nao e Q_PROPERTY de proposito: quem decide se a linha
        // aparece e o TEXTO estar vazio, e nao o QML repetir a regra.
        bool              CacheTelemetryLive() const;
        bool              IsRadianceCacheDedicatedUpdate() const;
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
        double            GetGISurfaceBiasMax() const;
        double            GetGIVolumeFadeProbes() const;
        QVariantList      GetRayEpsilons() const;

        Q_INVOKABLE void ToggleDDGI();
        Q_INVOKABLE void ToggleReSTIRGI();
        Q_INVOKABLE void ToggleReGIR();
        Q_INVOKABLE void ToggleRadianceCache();
        Q_INVOKABLE void ToggleRadianceCacheQuery();
        Q_INVOKABLE void ToggleRadianceCacheAutoWarmup();
        Q_INVOKABLE void ToggleRadianceCacheStats();
        Q_INVOKABLE void ToggleRadianceCacheStatsDetail();
        Q_INVOKABLE void ToggleRadianceCacheStatsSource();
        Q_INVOKABLE void ToggleRadianceCacheDedicatedUpdate();
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
        Q_INVOKABLE void SetCloudShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetCloudShadowStrength(double value);
        Q_INVOKABLE void SetCloudAltitude(double bottomKm, double thicknessKm);
        Q_INVOKABLE void SetCloudMarchSteps(int steps);

    signals:
        void AvailableChanged();
        void OceanSettingsChanged();
        void WeatherSettingsChanged();
        void CloudSettingsChanged();
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
