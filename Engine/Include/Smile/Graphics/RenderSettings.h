#pragma once

#include <string>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Upscaler.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/HistoryDomain.h"

// Fachada unica dos knobs de render.
//
// Antes disto o editor alcancava o mesmo tipo de estado por QUATRO caminhos diferentes
// (ver Docs/KNOBS-AUDIT.md): metodo no Renderer, Renderer->GetSubsistema().SetX(),
// referencia local ligada ao subsistema, e — no FWeather — atribuicao direta em campo
// publico. O criterio que separava "sobe pro Renderer" de "fica no subsistema" era real
// (o knob sobe quando tem invalidacao de historico junto) mas nunca esteve escrito, e ja
// vazou em quatro pontos: knob que passou a entrar no sinal gravado sem o setter acompanhar.
//
// Aqui existe UM caminho. O valor continua morando no subsistema — esta classe nao duplica
// estado, so roteia e carrega a politica de invalidacao. Fora daqui ficam de proposito:
//   - acesso ESTRUTURAL (GetScene, GetMaterials, GetDevice, GetGpuProfiler, GetSRVHeap,
//     GetDebugDraw), que nao e knob;
//   - selecao, picking, camera e carga de cena, que sao comandos do editor;
//   - o visualizador de render targets e a instrumentacao (BVH debug, timer de RT,
//     GBufferDebugMode, flicker), que sao ferramenta e merecem fachada propria.
//
// NOTA DE TRANSICAO. Esta classe guarda uma referencia ao Renderer e e friend dele, em vez
// de possuir os proprios membros. E deliberado e temporario: o passo 1 do desmembramento
// exige que os corpos sejam movidos BIT A BIT (nao ha teste que pegue regressao visual, so
// dois executaveis de matematica isolada), entao mover a POSSE do estado no mesmo commit
// tiraria o criterio de aceite binario "se a imagem mudou, e bug de digitacao". Com todos os
// acessos passando por aqui, mover as flags de modo para dentro desta classe vira uma
// mudanca local — e o proximo passo.
namespace Smile {

    class Renderer;

    class FRenderSettings {
    public:
        explicit FRenderSettings(Renderer& Owner) : R(Owner) {}

        FRenderSettings(const FRenderSettings&)            = delete;
        FRenderSettings& operator=(const FRenderSettings&) = delete;

        // === Apresentacao e escala ======================================================
        void SetVSync(bool Enabled);
        bool GetVSync() const;
        // SSAA manual: >1.0 supersampla. IGNORADO com o denoiser em DLSS_RR (a resolucao de
        // entrada e ditada pelo modo de qualidade; ver Renderer::SetRenderScale).
        void SetRenderScale(f32 V);
        f32  GetRenderScale() const;

        // === Upscaler e denoiser ========================================================
        // Eixos independentes {None,FSR,DLSS} e {None,NRD,DLSS_RR}, com um acoplamento: o RR
        // faz denoise E upscale num eval so, entao selecionar RR forca o upscaler p/ DLSS, e
        // sair de DLSS derruba o RR de volta p/ NRD.
        void      SetUpscaler(EUpscaler U);
        EUpscaler GetUpscaler() const;
        bool      UpscalerAvailable(EUpscaler U) const;
        void      SetUpscalerQuality(int Q);
        int       GetUpscalerQuality() const;
        void      SetUseTAA(bool V);
        bool      GetUseTAA() const;
        void      SetDenoiser(EDenoiser D);
        EDenoiser GetDenoiser() const;
        bool      RRAvailable() const;
        // Compat: o toggle NRD antigo do viewport mapeia p/ o eixo {None, NRD}.
        void SetUseNrdDenoise(bool V);
        bool GetUseNrdDenoise() const;

        // === Culling e geometria ========================================================
        void SetFrustumCulling(bool Use);
        bool GetFrustumCulling() const;
        void SetOcclusionCulling(bool Use);
        bool GetOcclusionCulling() const;
        void SetDepthPrepass(bool Use);
        bool GetDepthPrepass() const;
        void SetUseAsyncCompute(bool V);
        bool GetUseAsyncCompute() const;

        // === Iluminacao global ==========================================================
        void SetUseGI(bool V);
        bool GetUseGI() const;
        void SetUseReGIR(bool V);
        bool GetUseReGIR() const;
        bool ReGIRActive() const;
        void SetUseReSTIRGI(bool V);
        bool GetUseReSTIRGI() const;
        void SetUseReSTIRDI(bool V);
        bool GetUseReSTIRDI() const;
        bool ReSTIRDIActive() const;

        // Perfil unico de epsilons de raio. Invalidacao PESADA: os reservoirs guardam Lo/x2
        // medidos com o epsilon velho e o NRD acumula sobre eles.
        const FRayEpsilonProfile& GetRayEpsilons() const;
        void SetRayEpsilons(const FRayEpsilonProfile& P);

        // Knobs de amostragem do DDGI. Todos entram no ShadeSurfaceHit, portanto mudam o que
        // fica GRAVADO (atlas, reservoirs, historico de reflexao, inscatter do fog), nao so o
        // que aparece na tela — ver OnGIHitSamplingChanged.
        f32  GetGISurfaceBiasMax() const;
        void SetGISurfaceBiasMax(f32 V);
        f32  GetGISurfaceBiasScale() const;
        void SetGISurfaceBiasScale(f32 V);
        f32  GetGIVolumeFadeProbes() const;
        void SetGIVolumeFadeProbes(f32 V);
        void OnGIHitSamplingChanged();

        bool GetGIBackfacePolicy() const;
        void SetGIBackfacePolicy(bool V);

        // Alpha-test nos raios do GI (folhagem sombreia em vez de bloquear como opaca).
        // Fan-out para os TRES consumidores do HitShading — era o editor que sabia disso.
        // O FDDGI e a fonte da verdade na leitura, como antes.
        bool GetGIFoliageShadows() const;
        void SetGIFoliageShadows(bool V);

        // Reuso de visibilidade do ReSTIR GI. Nao toca o reservoir (so o Pass B e o resolve),
        // por isso nao limpa reservoir — ver ReSTIRGI.h.
        bool GetGIVisibility() const;
        void SetGIVisibility(bool V);

        // === Reflexoes ==================================================================
        void SetUseReflections(bool V);
        bool GetUseReflections() const;
        bool GetReflectionsCullBackface() const;
        void SetReflectionsCullBackface(bool V);

        // === Oclusao de ambiente ========================================================
        void SetUseAO(bool V);
        bool GetUseAO() const;
        void SetAOHalfRes(bool V);
        bool GetAOHalfRes() const;

        // === Sombras do sol =============================================================
        void SetUseSunShadows(bool Use);
        bool GetUseSunShadows() const;
        void SetShadowMaxDistance(f32 V);
        f32  GetShadowMaxDistance() const;
        void SetShadowDepthBias(f32 Texels);
        f32  GetShadowDepthBias() const;
        void SetShadowMinCasterTexels(f32 V);
        f32  GetShadowMinCasterTexels() const;
        void SetShadowCascadeCache(bool V);
        bool GetShadowCascadeCache() const;
        void SetShadowDebugCascades(bool V);
        bool GetShadowDebugCascades() const;
        void SetShadowCascadeBiasScale(u32 Cascade, f32 Scale);
        f32  GetShadowCascadeBiasScale(u32 Cascade) const;
        void SetSunAngularSize(f32 Degrees);
        f32  GetSunAngularSize() const;

        // === Sol e ceu ==================================================================
        void SetSunDirection(const Vec3& Dir);
        Vec3 GetSunDirection() const;
        void SetSunColor(const Vec3& Color);
        Vec3 GetSunColor() const;
        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);
        void SetPerPixelAtmoTransmittance(bool Use);
        bool GetPerPixelAtmoTransmittance() const;
        void SetSkyAmbientSH(bool Use);
        bool GetSkyAmbientSH() const;

        // === Fog e sun shafts ===========================================================
        void SetUseVolumetricFog(bool Use);
        bool GetUseVolumetricFog() const;
        void SetFogHeightSkyContribution(f32 V);
        f32  GetFogHeightSkyContribution() const;
        // Froxel: MaxDistance/Temporal/ConservativeDepth resetam o proprio historico porque
        // mudam o slicing ou o conteudo de froxel oculto, ou seja, invalidam a REPROJECAO.
        // Os demais nao resetam de proposito: o blend exponencial dilui o valor velho.
        void SetVolFogMaxDistance(f32 V);
        f32  GetVolFogMaxDistance() const;
        void SetVolFogTemporal(bool V);
        bool GetVolFogTemporal() const;
        void SetVolFogConservativeDepth(bool V);
        bool GetVolFogConservativeDepth() const;
        void SetVolFogPhaseG(f32 V);
        f32  GetVolFogPhaseG() const;
        void SetVolFogExtinctionScale(f32 V);
        f32  GetVolFogExtinctionScale() const;
        void SetVolFogAmbientIntensity(f32 V);
        f32  GetVolFogAmbientIntensity() const;
        void SetVolFogLightsIntensity(f32 V);
        f32  GetVolFogLightsIntensity() const;

        void SetUseSunShafts(bool Use);
        bool GetUseSunShafts() const;
        void SetShaftsIntensity(f32 V);
        f32  GetShaftsIntensity() const;
        void SetShaftsPhaseG(f32 V);
        f32  GetShaftsPhaseG() const;
        void SetShaftsSteps(f32 V);
        f32  GetShaftsSteps() const;
        void SetShaftsMaxDist(f32 V);
        f32  GetShaftsMaxDist() const;
        void SetShaftsDust(f32 V);
        f32  GetShaftsDust() const;
        void SetShaftsTemporal(bool V);
        bool GetShaftsTemporal() const;

        // === Nuvens volumetricas ========================================================
        void SetUseClouds(bool Use);
        bool GetUseClouds() const;
        void SetCloudsHalfRes(bool HalfRes); // recria o RT (flush da fila)
        bool GetCloudsHalfRes() const;
        void SetCloudsTemporal(bool V);
        bool GetCloudsTemporal() const;
        void SetCloudCoverage(f32 V);
        f32  GetCloudCoverage() const;
        void SetCloudDensityScale(f32 V);
        f32  GetCloudDensityScale() const;
        void SetCloudErosion(f32 V);
        f32  GetCloudErosion() const;
        void SetCloudPhaseG(f32 V);
        f32  GetCloudPhaseG() const;
        void SetCloudPowder(f32 V);
        f32  GetCloudPowder() const;
        void SetCloudWindSpeed(f32 V);
        f32  GetCloudWindSpeed() const;
        void SetCloudAmbientScale(f32 V);
        f32  GetCloudAmbientScale() const;
        void SetCloudTypeBias(f32 V);
        f32  GetCloudTypeBias() const;
        void SetCloudPeakVariation(f32 V);
        f32  GetCloudPeakVariation() const;
        void SetCloudMarchSteps(f32 V);
        f32  GetCloudMarchSteps() const;
        void SetCloudShadowsEnabled(bool V);
        bool GetCloudShadowsEnabled() const;
        void SetCloudShadowStrength(f32 V);
        f32  GetCloudShadowStrength() const;
        void SetCloudAltitude(f32 BottomKm, f32 ThicknessKm);
        f32  GetCloudBottomAltitude() const;
        f32  GetCloudThickness() const;
        // Weather map: bake procedural (re-bakeia na hora — flush + dispatch sincrono) ou
        // textura autorada.
        void SetCloudWeatherSeed(u32 Seed);
        u32  GetCloudWeatherSeed() const;
        void SetCloudWeatherCells(u32 Mult);
        u32  GetCloudWeatherCells() const;
        bool LoadCloudWeatherTexture(const std::wstring& Path);
        void ClearCloudWeatherTexture();
        bool CloudWeatherAuthored() const;

        // === Agua =======================================================================
        void SetUseWater(bool Use);
        bool GetUseWater() const;
        // NAO e knob de look: sem escrita de depth a agua deixa de ocluir quem le o depth.
        // Ver Renderer.h para o A/B que ele serve sob Ray Reconstruction.
        void SetWaterGuideInvisible(bool V);
        bool GetWaterGuideInvisible() const;
        void SetWaterWindSpeed(f32 V);
        f32  GetWaterWindSpeed() const;
        void SetWaterWindDirection(f32 Rad);
        f32  GetWaterWindDirection() const;
        void SetWaterWavesAmount(f32 V);
        f32  GetWaterWavesAmount() const;
        void SetWaterSwell(f32 V);
        f32  GetWaterSwell() const;
        void SetWaterSpectrumFetch(f32 Kilometres);
        f32  GetWaterSpectrumFetch() const;
        void SetWaterOceanDepth(f32 Metres);
        f32  GetWaterOceanDepth() const;
        void SetWaterFFTDisplacementScale(f32 V);
        f32  GetWaterFFTDisplacementScale() const;
        void SetWaterFFTChoppyScale(f32 V);
        f32  GetWaterFFTChoppyScale() const;

        // === Terreno ====================================================================
        void SetUseTerrain(bool Use);
        bool GetUseTerrain() const;

        // === Clima ======================================================================
        // RainAmount e DriveSky mudam a RADIANCIA DO CEU e a cor da luz-chave vistas pelos
        // passes de trace (ver Renderer.cpp: DDGI.SetSkyIntensity, reflexoes, ReSTIR GI). Sao
        // tier C — entram no atlas do DDGI, que tem hysteresis 0,99 — e por isso invalidam
        // (coalescido; ver o .cpp). Os de wetness reescrevem BaseColor/normal/roughness no
        // G-buffer, ou seja, os guides do RR: sao tier B e derrubam reflexoes/NRD/RR/TAA.
        void SetRainAmount(f32 V);
        f32  GetRainAmount() const;
        void SetRainDriveSky(bool V);
        bool GetRainDriveSky() const;
        void SetRainPuddleAmount(f32 V);
        f32  GetRainPuddleAmount() const;
        void SetRainPuddleScale(f32 V);
        f32  GetRainPuddleScale() const;
        void SetRainRippleStrength(f32 V);
        f32  GetRainRippleStrength() const;
        void SetRainWetDarkening(f32 V);
        f32  GetRainWetDarkening() const;
        void SetRainOcclusion(bool V);
        bool GetRainOcclusion() const;
        void SetRainCurtainAmount(f32 V);
        f32  GetRainCurtainAmount() const;
        void SetRainParticles(bool V);
        bool GetRainParticles() const;

        // === Notificacoes de edicao =====================================================
        // Versao COALESCIDA: use estas nos setters do editor. Arrastar um slider dispara o
        // setter a cada tick, e cada notificacao imediata custa um Flush da fila + reset de
        // todos os historicos.
        void MarkMaterialRTStateDirty();
        void MarkIndirectLightingDirty();
        // Imediatas. Chame so fora do caminho de arraste.
        void NotifyIndirectLightingChanged();
        void NotifyMaterialRTStateChanged();
        // A lista de renderaveis mudou de tamanho. So a parte de HISTORICO: o refresh (capacidades
        // de GPU, snapshot do InstanceGeo, TLAS, reancoragem da selecao) e lifecycle de cena e mora
        // no Renderer::OnSceneStructureChanged, que e quem chama isto no fim.
        void NotifySceneStructureChanged();
        // A camera SALTOU (foco do editor, carga de cena). Unico caminho que reseta os filtros de
        // tela — ver HistoryDomain::CameraCut. Nao chame em mudanca de conteudo: era isso que
        // fazia a cena piscar a cada knob.
        void NotifyCameraCut();

    private:
        // Executor UNICO do grafo de invalidacao. Recebe uma mascara de HistoryDomain (ou uma
        // combinacao ad-hoc de EHistoryTarget, para o caso genuinamente pontual) e derruba
        // exatamente esses historicos. Todo setter que invalida passa por aqui — se aparecer
        // uma lista escrita a mao num setter novo, e regressao.
        void Invalidate(EHistoryTarget Targets);

        Renderer& R;
    };
}
