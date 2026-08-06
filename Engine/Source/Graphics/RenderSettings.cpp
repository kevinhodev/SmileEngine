#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/Renderer.h"

// Corpos movidos do Renderer.h. Ver a nota de transicao no RenderSettings.h: neste passo os
// corpos sao MOVIDOS, nao reescritos — nem uma invalidacao a mais, nem uma a menos. As quatro
// divergencias catalogadas no Docs/KNOBS-AUDIT.md ficam para o passo seguinte, em commit
// separado, para que o A/B consiga distinguir "a invalidacao nova esta errada" de "a fachada
// quebrou um forward".
namespace Smile {

    namespace Dom = HistoryDomain;

    // Executor unico do grafo. Uma linha por alvo — o mapeamento bit -> chamada mora SO aqui,
    // que e o que permite acrescentar um acumulador novo sem varrer os setters.
    void FRenderSettings::Invalidate(EHistoryTarget _Targets) {
        using T = EHistoryTarget;
        if (HasTarget(_Targets, T::DDGIAtlas))        R.DDGI.ResetHistoryOnce();
        if (HasTarget(_Targets, T::ReGIR))            R.ReGIR.InvalidateHistory();
        if (HasTarget(_Targets, T::ReSTIRGI))         R.ReSTIRGI.InvalidateHistory();
        if (HasTarget(_Targets, T::ReSTIRDI))         R.ReSTIRDI.InvalidateHistory();
        if (HasTarget(_Targets, T::Reflections))      R.Reflections.InvalidateHistory();
        if (HasTarget(_Targets, T::NrdIndirect))      R.Nrd.InvalidateHistory();
        if (HasTarget(_Targets, T::NrdDirect))        R.NrdDirect.InvalidateHistory();
        if (HasTarget(_Targets, T::RayReconstruct))   R.RRResetPending = true;
        if (HasTarget(_Targets, T::TemporalAA))       R.TAARanLastFrame = false;
        if (HasTarget(_Targets, T::VolumetricFog))    R.VolumetricFog.ResetHistory();
        if (HasTarget(_Targets, T::VolumetricClouds)) R.VolumetricClouds.InvalidateHistory();
        if (HasTarget(_Targets, T::TemporalMotion))   R.TemporalMotion.InvalidateHistory();
        if (HasTarget(_Targets, T::HiZOcclusion))     R.HiZ.InvalidateResults();
        if (HasTarget(_Targets, T::ProbeDiagnostic))  R.RepeatDebugProbePoint();
    }

    // === Apresentacao e escala ==========================================================

    void FRenderSettings::SetVSync(bool _Enabled) { R.SwapChain.SetVSync(_Enabled); }
    bool FRenderSettings::GetVSync() const        { return R.SwapChain.GetVSync(); }

    void FRenderSettings::SetRenderScale(f32 _V) { R.SetRenderScale(_V); }
    f32  FRenderSettings::GetRenderScale() const { return R.RenderScale; }

    // === Upscaler e denoiser ============================================================

    void FRenderSettings::SetUpscaler(EUpscaler _U) {
        if (_U != EUpscaler::None && !UpscalerAvailable(_U)) _U = EUpscaler::None;
        // Acoplamento: o RR faz o upscale via DLSS. Trocar o upscaler p/ fora de DLSS desliga o
        // RR (cai p/ NRD). Vai pelo SetDenoiser em vez de atribuir Denoiser direto: a
        // atribuicao pulava Nrd.InvalidateHistory(), ReSTIRGI.InvalidateHistory() e o
        // RRResetPending, entao o NRD reaproveitava acumulacao de outro denoiser e os
        // reservoirs entravam no NRD ainda com o teto de firefly do modo cru (4 em vez de 8).
        // Nao recursa: o SetDenoiser so chama SetUpscaler quando o denoiser alvo e DLSS_RR, e
        // aqui o alvo e sempre NRD.
        const bool LeavingRR = (R.Denoiser == EDenoiser::DLSS_RR && _U != EUpscaler::DLSS);
        R.Upscaler = _U;
        Invalidate(Dom::TemporalOnly);
        if (LeavingRR) SetDenoiser(EDenoiser::NRD); // ja faz o ApplyUpscalerScale()
        else           R.ApplyUpscalerScale();
    }

    EUpscaler FRenderSettings::GetUpscaler() const { return R.Upscaler; }

    bool FRenderSettings::UpscalerAvailable(EUpscaler _U) const {
        switch (_U) {
            case EUpscaler::FSR:  return R.Fsr.Available();
            case EUpscaler::DLSS: return R.Dlss.Available();
            default:              return true; // None sempre disponivel
        }
    }

    void FRenderSettings::SetUpscalerQuality(int _Q) {
        R.UpscalerQuality = _Q < 0 ? 0 : (_Q > 4 ? 4 : _Q);
        R.ApplyUpscalerScale();
    }
    int FRenderSettings::GetUpscalerQuality() const { return R.UpscalerQuality; }

    void FRenderSettings::SetUseTAA(bool _V) { R.UseTAA = _V; Invalidate(Dom::TemporalOnly); }
    bool FRenderSettings::GetUseTAA() const  { return R.UseTAA; }

    void FRenderSettings::SetDenoiser(EDenoiser _D) {
        if (_D == EDenoiser::DLSS_RR && !R.DlssRR.Available()) _D = EDenoiser::NRD; // sem NVIDIA/RR
        if (_D == R.Denoiser) return;
        // Muda a natureza do sinal, entao reinicia acumulacao. Detalhes que o dominio carrega:
        // o teto de firefly do ReSTIR depende do denoiser e e aplicado ao Lo NA HORA DO TRACE
        // (fica gravado no reservoir, e com ValidateInterval = 0 nao ha re-shade que corrija);
        // o DI usa permutation temporal so no RR, entao nao misture reservoirs de politicas de
        // reprojecao diferentes; e NRD/RR param no Resolved cru, deixando o History[] das
        // reflexoes sem escrita — ao voltar ao caminho legado ele tem frames antigos.
        Invalidate(Dom::DenoiserSwap);
        R.Denoiser = _D;
        if (R.Denoiser == EDenoiser::DLSS_RR) // RR faz o upscale; trava o upscaler em DLSS
            SetUpscaler(EUpscaler::DLSS);
        R.ApplyUpscalerScale();
    }

    EDenoiser FRenderSettings::GetDenoiser() const { return R.Denoiser; }
    bool      FRenderSettings::RRAvailable() const { return R.DlssRR.Available(); }

    void FRenderSettings::SetUseNrdDenoise(bool _V) {
        SetDenoiser(_V ? EDenoiser::NRD : EDenoiser::None);
    }
    bool FRenderSettings::GetUseNrdDenoise() const { return R.Denoiser == EDenoiser::NRD; }

    // === Culling e geometria ============================================================

    void FRenderSettings::SetFrustumCulling(bool _Use) { R.UseFrustumCulling = _Use; }
    bool FRenderSettings::GetFrustumCulling() const    { return R.UseFrustumCulling; }

    void FRenderSettings::SetOcclusionCulling(bool _Use) {
        // Ao religar, descarta resultados velhos do readback ring — os proximos
        // kFramesInFlight frames desenham tudo ate ter teste fresco.
        if (_Use && !R.UseOcclusionCulling) Invalidate(EHistoryTarget::HiZOcclusion);
        R.UseOcclusionCulling = _Use;
    }
    bool FRenderSettings::GetOcclusionCulling() const { return R.UseOcclusionCulling; }

    void FRenderSettings::SetDepthPrepass(bool _Use) { R.UseDepthPrepass = _Use; }
    bool FRenderSettings::GetDepthPrepass() const    { return R.UseDepthPrepass; }

    void FRenderSettings::SetUseAsyncCompute(bool _V) { R.UseAsyncCompute = _V; }
    bool FRenderSettings::GetUseAsyncCompute() const  { return R.UseAsyncCompute; }

    // === Iluminacao global ==============================================================

    void FRenderSettings::SetUseGI(bool _V) { R.UseGI = _V; }
    bool FRenderSettings::GetUseGI() const  { return R.UseGI; }

    void FRenderSettings::SetUseReGIR(bool _V) {
        if (_V == R.UseReGIR) return;
        R.UseReGIR = _V;
        Invalidate(Dom::IndirectSampler);
    }
    bool FRenderSettings::GetUseReGIR() const { return R.UseReGIR; }
    bool FRenderSettings::ReGIRActive() const { return R.UseReGIR && R.ReGIR.IsReady(); }

    void FRenderSettings::SetUseReSTIRGI(bool _V) {
        if (_V == R.UseReSTIRGI) return;
        // Os reservoirs so na borda de SUBIDA: guardam radiancia do frame em que o toggle
        // desligou (sol/emissivos/DDGI antigos sobreviveriam por tempo indeterminado). Na
        // descida eles param de ser lidos, entao limpar seria custo puro.
        if (_V) Invalidate(EHistoryTarget::ReSTIRGI);
        R.UseReSTIRGI = _V;
        // Nas DUAS bordas, espelhando o SetUseReSTIRDI: ligar ou desligar o ReSTIR GI muda
        // drasticamente o sinal que o NRD acumula, que o RR reconstroi e que o TAA integra.
        // Antes daqui so o NrdIndirect caia — o RR seguia com historico neural de um sinal
        // que deixou de existir.
        Invalidate(Dom::ScreenResolve);
    }
    bool FRenderSettings::GetUseReSTIRGI() const { return R.UseReSTIRGI; }

    void FRenderSettings::SetUseReSTIRDI(bool _V) {
        if (_V == R.UseReSTIRDI) return;
        // Os reservoirs do DI so caem na borda de SUBIDA; o resto cai sempre.
        if (_V) Invalidate(EHistoryTarget::ReSTIRDI);
        R.UseReSTIRDI = _V;
        Invalidate(EHistoryTarget::NrdDirect | Dom::Resolve);
    }
    bool FRenderSettings::GetUseReSTIRDI() const { return R.UseReSTIRDI; }
    bool FRenderSettings::ReSTIRDIActive() const { return R.UseReSTIRDI && R.ReSTIRDI.IsReady(); }

    const FRayEpsilonProfile& FRenderSettings::GetRayEpsilons() const { return R.RayEps; }

    void FRenderSettings::SetRayEpsilons(const FRayEpsilonProfile& _P) {
        R.RayEps = _P;
        // Mudar geometria de raio invalida TUDO que acumula: os reservoirs guardam Lo e x2
        // medidos com os epsilons antigos, o shadow ray da direta usa o mesmo perfil, e o NRD
        // acumula sobre uns e outros. Sem o clear o A/B compara estado misturado.
        Invalidate(Dom::RayGeometry);
    }

    // Os knobs de amostragem do DDGI entram hoje no ShadeSurfaceHit — o bias desde que o 2o
    // bounce passou a usar o gather completo, o fade de borda desde que ele tambem parou de
    // extrapolar la. Por isso mudam o que fica GRAVADO, nao so o que aparece na tela.
    //
    // O preco: o reset faz a tomada passar de novo pela realizacao aleatoria de um frame (as
    // direcoes giram com o frameIndex), entao para medir A/B e preciso esperar convergir. Sem
    // o reset seria pior: estados misturados.
    void FRenderSettings::OnGIHitSamplingChanged() { Invalidate(Dom::RayVisibility); }

    f32 FRenderSettings::GetGISurfaceBiasMax() const { return R.DDGI.GetSurfaceBiasMax(); }
    void FRenderSettings::SetGISurfaceBiasMax(f32 _V) {
        if (_V == R.DDGI.GetSurfaceBiasMax()) return;
        R.DDGI.SetSurfaceBiasMax(_V);
        OnGIHitSamplingChanged();
    }

    f32 FRenderSettings::GetGISurfaceBiasScale() const { return R.DDGI.GetSurfaceBiasScale(); }
    void FRenderSettings::SetGISurfaceBiasScale(f32 _V) {
        if (_V == R.DDGI.GetSurfaceBiasScale()) return;
        R.DDGI.SetSurfaceBiasScale(_V);
        OnGIHitSamplingChanged();
    }

    f32 FRenderSettings::GetGIVolumeFadeProbes() const { return R.DDGI.GetVolumeFadeProbes(); }
    void FRenderSettings::SetGIVolumeFadeProbes(f32 _V) {
        if (_V == R.DDGI.GetVolumeFadeProbes()) return;
        R.DDGI.SetVolumeFadeProbes(_V);
        OnGIHitSamplingChanged();
    }

    bool FRenderSettings::GetGIBackfacePolicy() const { return R.ReSTIRGI.GetBackfacePolicy(); }
    void FRenderSettings::SetGIBackfacePolicy(bool _V) {
        // Passa por aqui, e nao direto no FReSTIRGI, porque o clear dos reservoirs sozinho nao
        // basta: o NRD e o RR acumulam SOBRE eles e o TAA sobre o resultado, entao um A/B feito
        // so com o clear compararia um estado misturado. DDGI e reflexoes ficam de fora de
        // proposito — a politica so toca no gather.
        if (_V == R.ReSTIRGI.GetBackfacePolicy()) return;
        R.ReSTIRGI.SetBackfacePolicy(_V); // ja marca NeedsClear nos reservoirs
        Invalidate(Dom::ScreenResolve);
    }

    // O FDDGI e a fonte da verdade na leitura, como no toggle antigo do editor.
    bool FRenderSettings::GetGIFoliageShadows() const { return R.DDGI.GetFoliageShadows(); }
    void FRenderSettings::SetGIFoliageShadows(bool _V) {
        if (_V == R.DDGI.GetFoliageShadows()) return;
        // Fan-out p/ os 3 consumidores do HitShading. Era o ViewportWidget que sabia disso.
        R.DDGI.SetFoliageShadows(_V);
        R.ReSTIRGI.SetFoliageShadows(_V);   // este ja marca NeedsClear por conta propria
        R.Reflections.SetFoliageShadows(_V);
        // O knob entra no ShadeSurfaceHit pelo ShadowRayMask: muda o Lo GRAVADO no reservoir e
        // o valor devolvido as sondas. Isso e, exatamente, a definicao do OnGIHitSamplingChanged
        // — entao reusa aquela invalidacao em vez de escrever uma lista nova. Inclui o
        // VolumetricFog (acumula inscatter que le o DDGI) e o RepeatDebugProbePoint (senao o
        // painel de diagnostico exibe os numeros do estado anterior durante o A/B).
        //
        // Antes daqui, dos tres so o FReSTIRGI se invalidava; FDDGI e FReflections eram
        // atribuicao pura, e nada derrubava NRD/RR/TAA. Com Hysteresis 0,99 no DDGI, o atlas
        // sombreado com a mascara antiga sobrevivia por centenas de updates.
        OnGIHitSamplingChanged();
    }

    bool FRenderSettings::GetGIVisibility() const { return R.ReSTIRGI.GetVisibility(); }
    void FRenderSettings::SetGIVisibility(bool _V) {
        if (_V == R.ReSTIRGI.GetVisibility()) return;
        R.ReSTIRGI.SetVisibility(_V);
        // O ReSTIRGI.h esta certo ao dizer que isto NAO toca o reservoir: o Visibility so atua
        // no Pass B e no resolve final, e o espacial nao realimenta o temporal. Por isso o
        // ReSTIRGI fica de fora — limpar reservoir aqui seria custo puro.
        //
        // Mas e modo PERSISTENTE que muda a radiancia resolvida, e o NRD acumula sobre ela (e o
        // RR/TAA sobre o resultado). Pelo mesmo criterio do SetGIBackfacePolicy — "o clear dos
        // reservoirs sozinho nao basta" — o que acumula DEPOIS do resolve precisa cair.
        Invalidate(Dom::ScreenResolve);
    }

    // === Reflexoes ======================================================================

    void FRenderSettings::SetUseReflections(bool _V) {
        if (_V == R.UseReflections) return;
        R.UseReflections = _V;
        // Com reflexoes off o spec acumula sinal zero — religar sem reset arrastaria esse
        // historico vazio pro especular real. O simetrico tambem vale: desligar deixa reflexao
        // de verdade no historico, que agora vai se misturar a zero. Por isso nas duas bordas, e
        // pelo mesmo dominio do SetReflectionsCullBackface — antes daqui faltavam o History[]
        // proprio do FReflections (com paridade de frame possivelmente velha), o RR e o TAA.
        Invalidate(Dom::Specular);
    }
    bool FRenderSettings::GetUseReflections() const { return R.UseReflections; }

    bool FRenderSettings::GetReflectionsCullBackface() const {
        return R.Reflections.GetBackfaceCull();
    }
    void FRenderSettings::SetReflectionsCullBackface(bool _V) {
        // Culling nos raios de REFLEXAO. Nao precisa de rebuild da TLAS — e parametro de
        // shader. Invalida so o que acumula reflexo: o ReSTIR e o DDGI nao veem esta chave.
        if (_V == R.Reflections.GetBackfaceCull()) return;
        R.Reflections.SetBackfaceCull(_V);
        Invalidate(Dom::Specular);
    }

    // === Oclusao de ambiente ============================================================

    void FRenderSettings::SetUseAO(bool _V) { R.UseAO = _V; }
    bool FRenderSettings::GetUseAO() const  { return R.UseAO; }
    // As duas cadeias (full e meia-res) ficam alocadas; o toggle e por frame.
    void FRenderSettings::SetAOHalfRes(bool _V) { R.AO.SetHalfRes(_V); }
    bool FRenderSettings::GetAOHalfRes() const  { return R.AO.GetHalfRes(); }

    // === Sombras do sol =================================================================

    void FRenderSettings::SetUseSunShadows(bool _Use) { R.UseSunShadows = _Use; }
    bool FRenderSettings::GetUseSunShadows() const    { return R.UseSunShadows; }

    void FRenderSettings::SetShadowMaxDistance(f32 _V) { R.SunShadows.SetMaxDistance(_V); }
    f32  FRenderSettings::GetShadowMaxDistance() const { return R.SunShadows.GetMaxDistance(); }
    void FRenderSettings::SetShadowDepthBias(f32 _T)   { R.SunShadows.SetDepthBias(_T); }
    f32  FRenderSettings::GetShadowDepthBias() const   { return R.SunShadows.GetDepthBias(); }
    void FRenderSettings::SetShadowMinCasterTexels(f32 _V) {
        R.SunShadows.SetMinCasterTexels(_V);
    }
    f32 FRenderSettings::GetShadowMinCasterTexels() const {
        return R.SunShadows.GetMinCasterTexels();
    }
    void FRenderSettings::SetShadowCascadeCache(bool _V) { R.SunShadows.SetCascadeCache(_V); }
    bool FRenderSettings::GetShadowCascadeCache() const  { return R.SunShadows.GetCascadeCache(); }
    void FRenderSettings::SetShadowDebugCascades(bool _V) { R.SunShadows.SetDebugCascades(_V); }
    bool FRenderSettings::GetShadowDebugCascades() const {
        return R.SunShadows.GetDebugCascades();
    }
    void FRenderSettings::SetShadowCascadeBiasScale(u32 _C, f32 _S) {
        R.SunShadows.SetCascadeBiasScale(_C, _S);
    }
    f32 FRenderSettings::GetShadowCascadeBiasScale(u32 _C) const {
        return R.SunShadows.GetCascadeBiasScale(_C);
    }
    void FRenderSettings::SetSunAngularSize(f32 _Deg) { R.SunShadows.SetSunAngularSize(_Deg); }
    f32  FRenderSettings::GetSunAngularSize() const   { return R.SunShadows.GetSunAngularSize(); }

    // === Sol e ceu ======================================================================

    void FRenderSettings::SetSunDirection(const Vec3& _Dir) { R.SetSunDirection(_Dir); }
    Vec3 FRenderSettings::GetSunDirection() const           { return R.SunDir; }
    void FRenderSettings::SetSunColor(const Vec3& _Color)   { R.SunColorRGB = _Color; }
    Vec3 FRenderSettings::GetSunColor() const               { return R.SunColorRGB; }
    void FRenderSettings::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        R.SetSunAzimuthElevation(_AzimuthDeg, _ElevationDeg);
    }

    void FRenderSettings::SetPerPixelAtmoTransmittance(bool _Use) {
        R.UsePerPixelAtmoTransmittance = _Use;
    }
    bool FRenderSettings::GetPerPixelAtmoTransmittance() const {
        return R.UsePerPixelAtmoTransmittance;
    }
    void FRenderSettings::SetSkyAmbientSH(bool _Use) { R.UseSkyAmbientSH = _Use; }
    bool FRenderSettings::GetSkyAmbientSH() const    { return R.UseSkyAmbientSH; }

    // === Fog e sun shafts ===============================================================

    void FRenderSettings::SetUseVolumetricFog(bool _Use) { R.UseVolumetricFog = _Use; }
    bool FRenderSettings::GetUseVolumetricFog() const    { return R.UseVolumetricFog; }

    void FRenderSettings::SetFogHeightSkyContribution(f32 _V) {
        R.Fog.SetHeightFogSkyContribution(_V);
    }
    f32 FRenderSettings::GetFogHeightSkyContribution() const {
        return R.Fog.GetHeightFogSkyContribution();
    }

    void FRenderSettings::SetVolFogMaxDistance(f32 _V) { R.VolumetricFog.SetMaxDistance(_V); }
    f32  FRenderSettings::GetVolFogMaxDistance() const { return R.VolumetricFog.GetMaxDistance(); }
    void FRenderSettings::SetVolFogTemporal(bool _V)   { R.VolumetricFog.SetTemporal(_V); }
    bool FRenderSettings::GetVolFogTemporal() const    { return R.VolumetricFog.GetTemporal(); }
    void FRenderSettings::SetVolFogConservativeDepth(bool _V) {
        R.VolumetricFog.SetConservativeDepth(_V);
    }
    bool FRenderSettings::GetVolFogConservativeDepth() const {
        return R.VolumetricFog.GetConservativeDepth();
    }
    void FRenderSettings::SetVolFogPhaseG(f32 _V) { R.VolumetricFog.SetPhaseG(_V); }
    f32  FRenderSettings::GetVolFogPhaseG() const { return R.VolumetricFog.GetPhaseG(); }
    void FRenderSettings::SetVolFogExtinctionScale(f32 _V) {
        R.VolumetricFog.SetExtinctionScale(_V);
    }
    f32 FRenderSettings::GetVolFogExtinctionScale() const {
        return R.VolumetricFog.GetExtinctionScale();
    }
    void FRenderSettings::SetVolFogAmbientIntensity(f32 _V) {
        R.VolumetricFog.SetAmbientIntensity(_V);
    }
    f32 FRenderSettings::GetVolFogAmbientIntensity() const {
        return R.VolumetricFog.GetAmbientIntensity();
    }
    void FRenderSettings::SetVolFogLightsIntensity(f32 _V) {
        R.VolumetricFog.SetLightsIntensity(_V);
    }
    f32 FRenderSettings::GetVolFogLightsIntensity() const {
        return R.VolumetricFog.GetLightsIntensity();
    }

    void FRenderSettings::SetUseSunShafts(bool _Use) { R.UseSunShafts = _Use; }
    bool FRenderSettings::GetUseSunShafts() const    { return R.UseSunShafts; }

    void FRenderSettings::SetShaftsIntensity(f32 _V) { R.SunShafts.SetVolIntensity(_V); }
    f32  FRenderSettings::GetShaftsIntensity() const { return R.SunShafts.GetVolIntensity(); }
    void FRenderSettings::SetShaftsPhaseG(f32 _V)    { R.SunShafts.SetVolPhaseG(_V); }
    f32  FRenderSettings::GetShaftsPhaseG() const    { return R.SunShafts.GetVolPhaseG(); }
    void FRenderSettings::SetShaftsSteps(f32 _V)     { R.SunShafts.SetVolSteps(_V); }
    f32  FRenderSettings::GetShaftsSteps() const     { return R.SunShafts.GetVolSteps(); }
    void FRenderSettings::SetShaftsMaxDist(f32 _V)   { R.SunShafts.SetVolMaxDist(_V); }
    f32  FRenderSettings::GetShaftsMaxDist() const   { return R.SunShafts.GetVolMaxDist(); }
    void FRenderSettings::SetShaftsDust(f32 _V)      { R.SunShafts.SetVolDust(_V); }
    f32  FRenderSettings::GetShaftsDust() const      { return R.SunShafts.GetVolDust(); }
    void FRenderSettings::SetShaftsTemporal(bool _V) { R.SunShafts.SetVolTemporal(_V); }
    bool FRenderSettings::GetShaftsTemporal() const  { return R.SunShafts.GetVolTemporal(); }

    // === Nuvens volumetricas ============================================================

    void FRenderSettings::SetUseClouds(bool _Use) {
        if (_Use && !R.UseClouds) Invalidate(EHistoryTarget::VolumetricClouds);
        R.UseClouds = _Use;
    }
    bool FRenderSettings::GetUseClouds() const { return R.UseClouds; }

    void FRenderSettings::SetCloudsHalfRes(bool _HalfRes) { R.SetCloudsHalfRes(_HalfRes); }
    bool FRenderSettings::GetCloudsHalfRes() const { return R.VolumetricClouds.GetHalfRes(); }
    void FRenderSettings::SetCloudsTemporal(bool _V) { R.VolumetricClouds.SetUseTemporal(_V); }
    bool FRenderSettings::GetCloudsTemporal() const  { return R.VolumetricClouds.GetUseTemporal(); }

    void FRenderSettings::SetCloudCoverage(f32 _V) { R.VolumetricClouds.SetCoverage(_V); }
    f32  FRenderSettings::GetCloudCoverage() const { return R.VolumetricClouds.GetCoverage(); }
    void FRenderSettings::SetCloudDensityScale(f32 _V) {
        R.VolumetricClouds.SetDensityScale(_V);
    }
    f32 FRenderSettings::GetCloudDensityScale() const {
        return R.VolumetricClouds.GetDensityScale();
    }
    void FRenderSettings::SetCloudErosion(f32 _V) { R.VolumetricClouds.SetErosion(_V); }
    f32  FRenderSettings::GetCloudErosion() const { return R.VolumetricClouds.GetErosion(); }
    void FRenderSettings::SetCloudPhaseG(f32 _V)  { R.VolumetricClouds.SetPhaseG(_V); }
    f32  FRenderSettings::GetCloudPhaseG() const  { return R.VolumetricClouds.GetPhaseG(); }
    void FRenderSettings::SetCloudPowder(f32 _V)  { R.VolumetricClouds.SetPowder(_V); }
    f32  FRenderSettings::GetCloudPowder() const  { return R.VolumetricClouds.GetPowder(); }
    void FRenderSettings::SetCloudWindSpeed(f32 _V) { R.VolumetricClouds.SetWindSpeed(_V); }
    f32  FRenderSettings::GetCloudWindSpeed() const { return R.VolumetricClouds.GetWindSpeed(); }
    void FRenderSettings::SetCloudAmbientScale(f32 _V) {
        R.VolumetricClouds.SetAmbientScale(_V);
    }
    f32 FRenderSettings::GetCloudAmbientScale() const {
        return R.VolumetricClouds.GetAmbientScale();
    }
    void FRenderSettings::SetCloudTypeBias(f32 _V) { R.VolumetricClouds.SetCloudTypeBias(_V); }
    f32  FRenderSettings::GetCloudTypeBias() const { return R.VolumetricClouds.GetCloudTypeBias(); }
    void FRenderSettings::SetCloudPeakVariation(f32 _V) {
        R.VolumetricClouds.SetPeakVariation(_V);
    }
    f32 FRenderSettings::GetCloudPeakVariation() const {
        return R.VolumetricClouds.GetPeakVariation();
    }
    void FRenderSettings::SetCloudMarchSteps(f32 _V) { R.VolumetricClouds.SetMarchSteps(_V); }
    f32  FRenderSettings::GetCloudMarchSteps() const { return R.VolumetricClouds.GetMarchSteps(); }
    void FRenderSettings::SetCloudShadowsEnabled(bool _V) {
        R.VolumetricClouds.SetShadowsEnabled(_V);
    }
    bool FRenderSettings::GetCloudShadowsEnabled() const {
        return R.VolumetricClouds.GetShadowsEnabled();
    }
    void FRenderSettings::SetCloudShadowStrength(f32 _V) {
        R.VolumetricClouds.SetShadowStrength(_V);
    }
    f32 FRenderSettings::GetCloudShadowStrength() const {
        return R.VolumetricClouds.GetShadowStrength();
    }
    void FRenderSettings::SetCloudAltitude(f32 _BottomKm, f32 _ThicknessKm) {
        R.VolumetricClouds.SetAltitude(_BottomKm, _ThicknessKm);
    }
    f32 FRenderSettings::GetCloudBottomAltitude() const {
        return R.VolumetricClouds.GetBottomAltitude();
    }
    f32 FRenderSettings::GetCloudThickness() const { return R.VolumetricClouds.GetThickness(); }

    void FRenderSettings::SetCloudWeatherSeed(u32 _Seed)  { R.SetCloudWeatherSeed(_Seed); }
    u32  FRenderSettings::GetCloudWeatherSeed() const     { return R.CloudNoise.GetSeed(); }
    void FRenderSettings::SetCloudWeatherCells(u32 _Mult) { R.SetCloudWeatherCells(_Mult); }
    u32  FRenderSettings::GetCloudWeatherCells() const    { return R.CloudNoise.GetCellMult(); }
    bool FRenderSettings::LoadCloudWeatherTexture(const std::wstring& _Path) {
        return R.LoadCloudWeatherTexture(_Path);
    }
    void FRenderSettings::ClearCloudWeatherTexture() { R.ClearCloudWeatherTexture(); }
    bool FRenderSettings::CloudWeatherAuthored() const {
        return R.CloudNoise.HasWeatherOverride();
    }

    // === Agua ===========================================================================

    void FRenderSettings::SetUseWater(bool _Use) { R.SetUseWater(_Use); }
    bool FRenderSettings::GetUseWater() const    { return R.UseWater; }

    bool FRenderSettings::GetWaterGuideInvisible() const { return R.Water.GetGuideInvisible(); }
    void FRenderSettings::SetWaterGuideInvisible(bool _V) {
        if (_V == R.Water.GetGuideInvisible()) return;
        R.Water.SetGuideInvisible(_V);
        Invalidate(Dom::Guides); // muda os guides do RR: historico neural velho mente
    }

    void FRenderSettings::SetWaterWindSpeed(f32 _V) { R.Water.SetWindSpeed(_V); }
    f32  FRenderSettings::GetWaterWindSpeed() const { return R.Water.GetWindSpeed(); }
    void FRenderSettings::SetWaterWindDirection(f32 _Rad) { R.Water.SetWindDirection(_Rad); }
    f32  FRenderSettings::GetWaterWindDirection() const   { return R.Water.GetWindDirection(); }
    void FRenderSettings::SetWaterWavesAmount(f32 _V) { R.Water.SetWavesAmount(_V); }
    f32  FRenderSettings::GetWaterWavesAmount() const { return R.Water.GetWavesAmount(); }
    void FRenderSettings::SetWaterSwell(f32 _V) { R.Water.SetSwell(_V); }
    f32  FRenderSettings::GetWaterSwell() const { return R.Water.GetSwell(); }
    void FRenderSettings::SetWaterSpectrumFetch(f32 _Km) { R.Water.SetSpectrumFetch(_Km); }
    f32  FRenderSettings::GetWaterSpectrumFetch() const  { return R.Water.GetSpectrumFetch(); }
    void FRenderSettings::SetWaterOceanDepth(f32 _M) { R.Water.SetOceanDepth(_M); }
    f32  FRenderSettings::GetWaterOceanDepth() const { return R.Water.GetOceanDepth(); }
    void FRenderSettings::SetWaterFFTDisplacementScale(f32 _V) {
        R.Water.SetFFTDisplacementScale(_V);
    }
    f32 FRenderSettings::GetWaterFFTDisplacementScale() const {
        return R.Water.GetFFTDisplacementScale();
    }
    void FRenderSettings::SetWaterFFTChoppyScale(f32 _V) { R.Water.SetFFTChoppyScale(_V); }
    f32  FRenderSettings::GetWaterFFTChoppyScale() const { return R.Water.GetFFTChoppyScale(); }

    // === Terreno ========================================================================

    void FRenderSettings::SetUseTerrain(bool _Use) { R.UseTerrain = _Use; }
    bool FRenderSettings::GetUseTerrain() const    { return R.UseTerrain; }

    // === Clima ==========================================================================

    // RainAmount e DriveSky sao os dois unicos knobs de clima que entram em sinal GRAVADO. Os
    // dois alimentam o RainSky do RenderFrame, e dele saem tres coisas que voltam para
    // historico de vida longa:
    //   RainSkyDim -> DDGI.SetSkyIntensity, reflexoes e ReSTIR GI (a radiancia do ceu vista
    //                 por um raio);
    //   RainKeyDim -> EffectiveSunColor/MoonLightCol, que viram o SunColor dos tres passes
    //                 de trace (a cor da luz-chave no hit);
    //   RainAmbDim -> ambiente hemisferico e SH.
    // O DDGI tem Hysteresis 0,99: sem o reset, 99% do atlas de tempo seco sobrevive POR
    // UPDATE, e a chuva fica calibrada contra energia que o usuario ja mandou remover. Os
    // reservoirs do ReSTIR sao piores ainda — com ValidateInterval = 0 nao ha re-shade que
    // corrija o Lo gravado.
    //
    // A politica NAO e nova: e a mesma de NotifyIndirectLightingChanged, que ja existe para
    // "a energia que alimenta o indireto mudou" (hoje FLight::RTWeight). Chuva mexendo no ceu
    // e na luz-chave e o mesmo caso, entao reusa em vez de virar a decima quinta lista escrita
    // a mao. Pela versao COALESCIDA porque RainAmount e slider: sem isso, cada tick do arraste
    // custaria um reset completo e o GI ficaria em reset permanente durante o gesto.
    void FRenderSettings::SetRainAmount(f32 _V) {
        if (_V == R.Weather.GetRainAmount()) return;
        // Com DriveSky off, RainSky e 0 nos dois estados: o knob so mexe em wetness (G-buffer,
        // screen-space) e na cortina. Nada de vida longa cai, entao nao invalide.
        const bool DrivesSky = R.Weather.GetDriveSky();
        R.Weather.SetRainAmount(_V);
        if (DrivesSky) MarkIndirectLightingDirty();
    }
    f32 FRenderSettings::GetRainAmount() const { return R.Weather.GetRainAmount(); }

    void FRenderSettings::SetRainDriveSky(bool _V) {
        if (_V == R.Weather.GetDriveSky()) return;
        R.Weather.SetDriveSky(_V);
        // Sem chuva, RainSky e 0 dos dois lados do toggle — nada mudou no ceu.
        if (R.Weather.Raining()) MarkIndirectLightingDirty();
    }
    bool FRenderSettings::GetRainDriveSky() const { return R.Weather.GetDriveSky(); }
    // Knobs de WETNESS. O RainWetness.ps.hlsl le copias de GBufferA/B e REESCREVE os originais:
    // BaseColor no SV_Target0 e normal + roughness no SV_Target1. Ou seja, mexem exatamente nos
    // guides que o Ray Reconstruction consome e no G-buffer de onde as reflexoes tiram a
    // rugosidade que escolhe o lobo especular. Quem acumulou com a rugosidade antiga precisa
    // cair — mesmo raciocinio do SetWaterGuideInvisible, mais o especular (aquele mexe em
    // depth/velocity, este mexe em roughness).
    //
    // Tier B, nao C: wetness e screen-space, aplicada DEPOIS do geometry pass, e os hits de RT
    // leem o material do InstanceGeo. Os reservoirs nao a enxergam — por isso nem ReSTIR nem o
    // atlas do DDGI entram aqui.
    //
    // Sem coalescer, apesar de serem sliders: o dominio e so escrita de flag (nenhum
    // CommandQueue.Flush, ao contrario do NotifyMaterialRTStateChanged) e os historicos sao
    // curtos (Reflections MaxFrames = 12). Durante o arraste o especular fica reconvergindo, o
    // que e a leitura honesta do que esta acontecendo. Se virar incomodo, a generalizacao certa
    // e coalescer POR DOMINIO, nao uma flag ad-hoc por knob.
    void FRenderSettings::SetRainPuddleAmount(f32 _V) {
        if (_V == R.Weather.GetPuddleAmount()) return;
        R.Weather.SetPuddleAmount(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainPuddleAmount() const { return R.Weather.GetPuddleAmount(); }

    void FRenderSettings::SetRainPuddleScale(f32 _V) {
        if (_V == R.Weather.GetPuddleScale()) return;
        R.Weather.SetPuddleScale(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainPuddleScale() const { return R.Weather.GetPuddleScale(); }

    void FRenderSettings::SetRainRippleStrength(f32 _V) {
        if (_V == R.Weather.GetRippleStrength()) return;
        R.Weather.SetRippleStrength(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainRippleStrength() const { return R.Weather.GetRippleStrength(); }

    void FRenderSettings::SetRainWetDarkening(f32 _V) {
        if (_V == R.Weather.GetWetDarkening()) return;
        R.Weather.SetWetDarkening(_V);
        Invalidate(Dom::Specular);
    }
    f32 FRenderSettings::GetRainWetDarkening() const { return R.Weather.GetWetDarkening(); }

    void FRenderSettings::SetRainOcclusion(bool _V) {
        if (_V == R.Weather.GetRainOcclusion()) return;
        R.Weather.SetRainOcclusion(_V);
        Invalidate(Dom::Specular);
    }
    bool FRenderSettings::GetRainOcclusion() const { return R.Weather.GetRainOcclusion(); }
    void FRenderSettings::SetRainCurtainAmount(f32 _V) { R.Weather.SetCurtainAmount(_V); }
    f32  FRenderSettings::GetRainCurtainAmount() const { return R.Weather.GetCurtainAmount(); }
    void FRenderSettings::SetRainParticles(bool _V) { R.Weather.SetRainParticles(_V); }
    bool FRenderSettings::GetRainParticles() const  { return R.Weather.GetRainParticles(); }

    // === Notificacoes de edicao =========================================================

    void FRenderSettings::MarkMaterialRTStateDirty()  { R.MaterialRTStateDirty  = true; }
    void FRenderSettings::MarkIndirectLightingDirty() { R.IndirectLightingDirty = true; }

    // Reservoirs guardam Lo medido com a luz antiga; o atlas do DDGI, idem.
    void FRenderSettings::NotifyIndirectLightingChanged() { Invalidate(Dom::SkyRadiance); }

    void FRenderSettings::NotifySceneStructureChanged() { Invalidate(Dom::SceneStructure); }

    void FRenderSettings::NotifyCameraCut() { Invalidate(Dom::CameraCut); }

    void FRenderSettings::NotifyMaterialRTStateChanged() {
        // REFRESH (nao e invalidacao — e trabalho a refazer, nao memoria a descartar). O Flush
        // e necessario: o InstanceGeo e um upload heap sem versao por frame em voo, entao
        // reescrever com frames voando corromperia o que eles leem. Custa um stall, mas isto so
        // dispara em edicao manual de material.
        R.CommandQueue.Flush();
        R.DDGI.RefreshInstanceGeo(R.Scene);
        R.TlasFlagsDirty = true; // mask/FORCE_NON_OPAQUE/culling saem do material
        // E os historicos acumulados sobre a aparencia antiga.
        Invalidate(Dom::MaterialRTState);
    }
}
