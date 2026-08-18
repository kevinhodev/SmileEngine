#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RendererFrameState.h"
#include "Smile/Graphics/Renderer/RendererSceneState.h"
#include "Smile/Graphics/Backend/RenderBackend.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/Water/OceanSpectrum.h"
#include "Smile/Graphics/RayTracing/RTMasks.h" // kRTMaskShadowFull: mascara dos shadow rays de direta local
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Renderer/DepthConfig.h"
#include "Smile/Graphics/RayTracing/RayEpsilons.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <exception>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <type_traits>

namespace Smile {
    IUpscaler* Renderer::ActiveUpscaler() {
        if (Denoiser == EDenoiser::DLSS_RR)
            return (DlssRR.IsInitialized() && RRGuides.IsReady())
                ? static_cast<IUpscaler*>(&DlssRR)
                : nullptr;

        switch (Upscaler) {
            case EUpscaler::FSR:  return Fsr.IsInitialized()  ? static_cast<IUpscaler*>(&Fsr)  : nullptr;
            case EUpscaler::DLSS: return Dlss.IsInitialized() ? static_cast<IUpscaler*>(&Dlss) : nullptr;
            default:              return nullptr;
        }
    }

    void Renderer::ApplyUpscalerScale() {
        f32 Ratio = 1.0f;
        if      (Denoiser == EDenoiser::DLSS_RR) Ratio = DlssRR.RenderRatioForQuality(UpscalerQuality);
        else if (Upscaler == EUpscaler::FSR)     Ratio = Fsr.RenderRatioForQuality(UpscalerQuality);
        else if (Upscaler == EUpscaler::DLSS)    Ratio = Dlss.RenderRatioForQuality(UpscalerQuality);
        ApplyRenderScale(Ratio);
    }

    namespace {
        f32 Halton(u32 i, u32 b) {
            f32 f = 1.0f, r = 0.0f;
            while (i > 0) { f /= static_cast<f32>(b); r += f * static_cast<f32>(i % b); i /= b; }
            return r;
        }
    }

    // Resolve a politica efetiva uma vez, antes de qualquer consumidor publicar estado do frame.
    FEffectiveIndirectPolicy Renderer::ResolveIndirectPolicy() {
        const FEffectiveIndirectPolicy P = EffectiveIndirectPolicy();

        // A primeira observacao inicializa o snapshot e nao representa uma transicao.
        if (FrameState->HasPrevIndirectPolicy) {
            // As tres bordas sao independentes e podem invalidar dominios no mesmo frame.
            const bool Terminator = P.TerminatorDiffers(FrameState->PrevIndirectPolicy);
            const bool Route      = P.SurfaceRouteDiffers(FrameState->PrevIndirectPolicy);
            const bool Volumetric = P.VolumetricDiffers(FrameState->PrevIndirectPolicy);
            if (Terminator || Route || Volumetric)
                Settings().NotifyIndirectPolicyChanged(Terminator, Route, Volumetric);
        }
        // Um alvo de debug nao pode sobreviver ao produtor efetivo que o preenchia.
        Settings().DropGISourceDebugIfOrphaned();

        FrameState->PrevIndirectPolicy    = P;
        FrameState->HasPrevIndirectPolicy = true;
        return P;
    }

    // Resolve num lugar so "que passes rodam neste frame". Todos os insumos sao membros que
    // nao mudam durante a gravacao (verificado: nenhum deles e reescrito dentro do RenderFrame),
    // entao resolver de antemao e equivalente a resolver espalhado — e passavel a uma fase.
    FFrameModes Renderer::ResolveFrameModes(const FEffectiveIndirectPolicy& _Policy) {
        FFrameModes M;
        M.UpscaleActive = (ActiveUpscaler() != nullptr);
        M.TAAActive     = UseTAA && !M.UpscaleActive && TemporalAA.IsInitialized();
        // DLSS Ray Reconstruction: precisa do RR inicializado e dos guides prontos; o eval
        // acontece no bloco de upscale (ActiveUpscaler() == &DlssRR).
        M.RRMode = (Denoiser == EDenoiser::DLSS_RR) && DlssRR.IsInitialized() && RRGuides.IsReady();

        M.ReflectionsActive   = ReflectionsReady();
        // Roteamento segue a politica, incluindo trace, NRD, deferred e telemetria.
        M.ReSTIRGIActive      = _Policy.Primary == EIndirectPrimary::ReSTIR_SHaRC;
        M.ReSTIRDIActiveFrame = UseReSTIRDI && ReSTIRDI.IsReady();
        M.NrdIndirectMode     = M.ReSTIRGIActive && Nrd.IsReady() &&
                                Denoiser == EDenoiser::NRD;
        M.NrdDirectMode       = M.ReSTIRDIActiveFrame && ReSTIRDI.IsNrdReady() &&
                                NrdDirect.IsReady() && Denoiser == EDenoiser::NRD;
        M.AOWillRun           = UseAO && AO.IsReady();
        // O produtor dedicado do cache nao depende de ReSTIR GI nem de DDGI: ele traca a partir do
        // G-buffer e termina no proprio cache. Depende so de o cache estar ligado e das tabelas
        // dele montadas — que e exatamente o que UpdatePassActive() responde.
        M.RadianceCacheUpdateActive = RadianceCache.UpdatePassActive();
        const u32 CacheDebugIndex = DebugTargets::IndexOf(FRadianceCache::kDebugTargetName);
        M.RadianceCacheDebugActive = RadianceCache.CanVisualize() &&
            CacheDebugIndex != DebugTargets::kInvalid &&
            (DebugTargetIndex == CacheDebugIndex ||
             std::find(DebugSelection.begin(), DebugSelection.end(), CacheDebugIndex) !=
                 DebugSelection.end());
        M.GeometricNormalWillRun = M.AOWillRun || M.RadianceCacheDebugActive;

        M.WaterReflectionDebug =
            Water.GetDebugMode() == FWaterRenderer::EDebugMode::Reflection;
        M.DedicatedWaterReflections = M.ReflectionsActive &&
            (Water.GetDebugMode() == FWaterRenderer::EDebugMode::Off || M.WaterReflectionDebug);
        M.WaterSceneCopiesReady = Targets.SceneColorCopy && Targets.SceneDepthCopy;

        M.ReliableMotionActive = TemporalMotion.IsReady() &&
            (M.ReflectionsActive || M.ReSTIRGIActive || M.ReSTIRDIActiveFrame);
        M.MotionHistoryValidThisFrame = TemporalMotion.HasHistory();

        M.VolShaftsActive = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
        M.VolFogActive    = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
        M.FogSkyAnchor    = UseAtmosphereSky && Atmosphere.IsInitialized();
        return M;
    }

    // Camera e matrizes do frame. Hoistado do meio do RenderFrame: o segundo grupo (as
    // variantes sem translacao) morava ~280 linhas abaixo do primeiro, mas depende so do
    // mesmo View/Projection. O VP sem translacao anterior so e reescrito no FIM do frame,
    // entao juntar os dois aqui e equivalente (verificado).
    FFrameView Renderer::ResolveFrameView(const FFrameModes& _Modes, IUpscaler* _ActiveUp) {
        FFrameView V;
        V.Aspect = Backend->SwapChain.GetWidth() > 0 && Backend->SwapChain.GetHeight() > 0
                   ? static_cast<f32>(Backend->SwapChain.GetWidth()) / static_cast<f32>(Backend->SwapChain.GetHeight())
                   : 1.0f;
        V.View  = SceneState->Camera.GetViewMatrix();
        V.NearZ = 0.1f;
        V.FarZ  = UseWater ? 20000.0f : 4000.0f;
        V.FovY  = GetFovY();
        V.ProjUnjittered = kReverseZ
            ? Mat44::PerspectiveFovReverseZLH(V.FovY, V.Aspect, V.NearZ, V.FarZ)
            : Mat44::PerspectiveFovLH(V.FovY, V.Aspect, V.NearZ, V.FarZ);

        V.Projection = V.ProjUnjittered;
        if (_Modes.UpscaleActive) {
            // FSR usa a sequencia do SDK; DLSS usa Halton.
            _ActiveUp->GetJitter(FrameState->TemporalSampleIndex, V.JitterPxX, V.JitterPxY);
            V.ProjJitterYSign = -1.0f;
        } else if (_Modes.TAAActive) {
            const u32 kJitterPhases = 8;
            const u32 Idx = (FrameState->TemporalSampleIndex % kJitterPhases) + 1;
            V.JitterPxX = Halton(Idx, 2) - 0.5f;
            V.JitterPxY = Halton(Idx, 3) - 0.5f;
        }
        if (_Modes.UpscaleActive || _Modes.TAAActive) {
            V.Projection.M[2][0] += V.JitterPxX * 2.0f / static_cast<f32>(RenderWidth());
            V.Projection.M[2][1] += V.ProjJitterYSign * V.JitterPxY * 2.0f / static_cast<f32>(RenderHeight());
        }
        V.JitterUv = Vec2{ V.JitterPxX / static_cast<f32>(RenderWidth()),
                           -V.ProjJitterYSign * V.JitterPxY / static_cast<f32>(RenderHeight()) };
        V.JitterPx = Vec2{ V.JitterPxX, -V.ProjJitterYSign * V.JitterPxY };

        V.ViewProjection     = V.View * V.Projection;
        V.ViewProjUnjittered = V.View * V.ProjUnjittered;
        V.CameraPosition     = SceneState->Camera.GetPosition();

        V.ViewNoTrans = V.View;
        V.ViewNoTrans.M[3][0] = 0.0f;
        V.ViewNoTrans.M[3][1] = 0.0f;
        V.ViewNoTrans.M[3][2] = 0.0f;
        V.VPNoTrans    = V.ViewNoTrans * V.Projection;
        V.InvVPNoTrans = V.VPNoTrans.Inverse();
        V.VPNoTransUnjit    = V.ViewNoTrans * V.ProjUnjittered;
        V.SkyClipToPrevClip = V.VPNoTransUnjit.Inverse() * FrameState->PrevViewProjNoTranslation;
        V.InvViewProjFull  = V.ViewProjection.Inverse();
        V.InvViewProjUnjit = V.ViewProjUnjittered.Inverse();

        V.MipBias = (_Modes.UpscaleActive && RenderWidth() < OutputWidth())
            ? std::log2(static_cast<f32>(RenderWidth()) / static_cast<f32>(OutputWidth())) - 1.0f
            : 0.0f;

        {
            const Mat44& VP = V.ViewProjection;
            auto Col = [&](int j) { return Vec4{ VP.M[0][j], VP.M[1][j], VP.M[2][j], VP.M[3][j] }; };
            Vec4 c0 = Col(0), c1 = Col(1), c2 = Col(2), c3 = Col(3);
            V.FrustumPlanes[0] = { c3.X+c0.X, c3.Y+c0.Y, c3.Z+c0.Z, c3.W+c0.W };
            V.FrustumPlanes[1] = { c3.X-c0.X, c3.Y-c0.Y, c3.Z-c0.Z, c3.W-c0.W };
            V.FrustumPlanes[2] = { c3.X+c1.X, c3.Y+c1.Y, c3.Z+c1.Z, c3.W+c1.W };
            V.FrustumPlanes[3] = { c3.X-c1.X, c3.Y-c1.Y, c3.Z-c1.Z, c3.W-c1.W };
            V.FrustumPlanes[4] = { c2.X, c2.Y, c2.Z, c2.W };
            V.FrustumPlanes[5] = { c3.X-c2.X, c3.Y-c2.Y, c3.Z-c2.Z, c3.W-c2.W };
        }
        return V;
    }

    // Sol, lua, chuva e a luz-chave. So calculo — as publicacoes (constant buffer, parametros
    // de noite/estrelas da atmosfera) continuam no RenderFrame, lendo daqui. O MoonTint fica
    // local: e constante e so alimenta MoonLightCol e o MoonColorRaw publicado la.
    FFrameLighting Renderer::ResolveFrameLighting() {
        FFrameLighting L;
        // Redundante desde que o SunDir nasce unitario (ver a invariante no Renderer.h); fica como
        // rede contra um NaN vindo de fora, que e o que o NormalizedSafe existe para conter.
        L.SunN = SunDir.NormalizedSafe(DefaultSunDirection());

        L.RainSky    = Weather.GetDriveSky() ? Weather.GetRainAmount() : 0.0f;
        L.RainKeyDim = 1.0f - L.RainSky * 0.75f;
        L.RainAmbDim = 1.0f - L.RainSky * 0.40f;
        // Todos os traces compartilham esta atenuacao da radiancia do ceu. RainAmbDim trata
        // separadamente a irradiancia hemisferica.
        L.RainSkyDim = 1.0f - L.RainSky * 0.65f;

        L.EffectiveSunColor = SunColorRGB;
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            const Vec3 T = Atmosphere.SunTransmittance(L.SunN);
            L.EffectiveSunColor = { SunColorRGB.X * T.X, SunColorRGB.Y * T.Y, SunColorRGB.Z * T.Z };
        }
        {
            const f32 hf = std::clamp(L.SunN.Y / 0.03f, 0.0f, 1.0f);
            const f32 HorizonFade = hf * hf * (3.0f - 2.0f * hf);
            L.EffectiveSunColor = { L.EffectiveSunColor.X * HorizonFade,
                                    L.EffectiveSunColor.Y * HorizonFade,
                                    L.EffectiveSunColor.Z * HorizonFade };
        }
        L.EffectiveSunColor = { L.EffectiveSunColor.X * L.RainKeyDim,
                                L.EffectiveSunColor.Y * L.RainKeyDim,
                                L.EffectiveSunColor.Z * L.RainKeyDim }; // F4: nublado de chuva

        L.MoonN = TimeOfDay.MoonDirection();

        const f32 nf     = std::clamp((0.0f - L.SunN.Y) / 0.15f, 0.0f, 1.0f);
        L.NightFactor    = nf * nf * (3.0f - 2.0f * nf);
        const f32 MoonSunCos = L.MoonN.X * L.SunN.X + L.MoonN.Y * L.SunN.Y + L.MoonN.Z * L.SunN.Z;
        L.MoonIllum      = (1.0f - MoonSunCos) * 0.5f;
        const f32 MoonUp = std::clamp(L.MoonN.Y * 8.0f, 0.0f, 1.0f);
        L.MoonOn         = TimeOfDay.MoonEnabled;

        L.MoonTrans = (UseAtmosphereSky && Atmosphere.IsInitialized())
                    ? Atmosphere.SunTransmittance(L.MoonN) : Vec3{ 1.0f, 1.0f, 1.0f };
        const Vec3 MoonTint = { 0.6f, 0.7f, 1.0f };
        L.MoonLightCol = { MoonTint.X * L.MoonTrans.X * L.RainKeyDim,
                           MoonTint.Y * L.MoonTrans.Y * L.RainKeyDim,
                           MoonTint.Z * L.MoonTrans.Z * L.RainKeyDim }; // F4: nublado
        L.MoonW = L.MoonOn ? (TimeOfDay.MoonIntensity * L.MoonIllum * L.NightFactor * MoonUp) : 0.0f;

        // MoonDiskSize multiplica o diametro lunar medio de 0,518 grau.
        const f32 MoonHalfAngleRad = 0.5f * ToRad * TimeOfDay.MoonDiskAngularDiameterDeg();
        L.CosMoonRadius = std::cos(MoonHalfAngleRad);
        // x2 sem textura: o disco procedural branco depende do brilho pra ter presenca.
        L.MoonDiskBright = L.MoonOn
            ? TimeOfDay.MoonDiskBrightness * (Atmosphere.HasMoonTexture() ? 1.0f : 2.0f)
            : 0.0f;
        L.MoonSkyScale = L.MoonOn ? (0.05f * TimeOfDay.MoonIntensity * L.MoonIllum) : 0.0f;

        L.KeyIsMoon = (L.SunN.Y <= 0.0f);
        L.KeyDir    = L.KeyIsMoon ? L.MoonN : L.SunN;
        L.KeyColor  = L.KeyIsMoon ? L.MoonLightCol : L.EffectiveSunColor;
        L.KeyInt    = L.KeyIsMoon ? L.MoonW : SunIntensity;
        L.CloudDim  = L.KeyIsMoon ? (L.MoonW / std::max(SunIntensity, 1e-3f)) : 1.0f;
        L.KeyCloudCol = { L.KeyColor.X * L.CloudDim, L.KeyColor.Y * L.CloudDim,
                          L.KeyColor.Z * L.CloudDim };
        return L;
    }

    void Renderer::TickWorldClock() {
        // A captura reafirma hora e sol porque o painel edita TimeOfDay por referencia.
        if (Capture.SessionActive()) {
            TimeOfDay.TimeHours = CaptureSunHours;
            SunDir              = CaptureSunDir;
        } else if (TimeOfDay.Enabled) {
            TimeOfDay.Tick(FrameState->LastDeltaTime);
            SetSunDirection(TimeOfDay.SunDirection());
        }
        {
            const f32 Target = Weather.GetRainAmount();
            const f32 Tau    = (Target > Weather.GetWetness()) ? 5.0f : 30.0f;
            Weather.SetWetness(Weather.GetWetness() +
                               (Target - Weather.GetWetness()) *
                               (1.0f - std::exp(-std::max(FrameState->LastDeltaTime, 0.0f) / Tau)));
            if (Target <= 0.001f && Weather.GetWetness() < 0.005f) Weather.SetWetness(0.0f);
        }
    }

    FFrameAmbient Renderer::PublishFrameConstants(const FFrameView& _Vw,
                                                  const FFrameLighting& _Lt,
                                                  const FEffectiveIndirectPolicy& _Policy,
                                                  u32 _FrameSlot, FrameConstants* _CB) {
        // NOTA DE ORDEM (a unica reordenacao desta extracao): estas tres escritas corriam ANTES
        // do TickWorldClock. Descer para ca e inerte — o relogio so escreve SunDir e a molhadura,
        // e nenhum dos insumos abaixo (posicao da camera, HDRI, tempo decorrido, indice do frame) e
        // tocado por ele. Em troca, TODA a publicacao no constant buffer passa a viver num lugar.
        _CB->CameraPosition = { _Vw.CameraPosition.X, _Vw.CameraPosition.Y, _Vw.CameraPosition.Z, 1.0f };

        const f32 IBLEnabled = HDREnv.HasHDRLoaded() ? 1.0f : 0.0f;
        _CB->IBLParams      = { IBLIntensity, IBLRotation,
                                static_cast<f32>(FHDREnvironment::kSpecularMips - 1),
                                IBLEnabled };
        // O .z e o contador ABSOLUTO, e continua sendo — nenhum shader le Time.z hoje (so o .w,
        // gate do AODebug), entao a escolha de indice aqui nao muda imagem nenhuma. Fica no
        // FrameIndex porque, se algum dia alguem ler, "quantos frames desde o boot" e a leitura
        // que o nome sugere; quem precisar de semente tem o TemporalSampleIndex, que e passado
        // explicitamente a cada passe. Campo sem consumidor: candidato a limpeza em outro commit.
        _CB->Time           = { FrameState->ElapsedTime, FrameState->LastDeltaTime,
                                static_cast<f32>(FrameState->FrameIndex),
                                AODebug ? 1.0f : 0.0f };

        _CB->SunDirection   = { _Lt.SunN.X, _Lt.SunN.Y, _Lt.SunN.Z, SunIntensity };
        _CB->SunColor       = { _Lt.EffectiveSunColor.X, _Lt.EffectiveSunColor.Y,
                                _Lt.EffectiveSunColor.Z, 0.0f };
        // Variante CRUA (sem transmitancia, sem HorizonFade) p/ o caminho por pixel do deferred
        // e do ForwardBlend. O SunColor acima continua intacto: todos os outros consumidores
        // (fog volumetrico, nuvens, agua, sun shafts, readout do editor) seguem sem mudanca.
        _CB->SunColorRaw    = { SunColorRGB.X * _Lt.RainKeyDim, SunColorRGB.Y * _Lt.RainKeyDim,
                                SunColorRGB.Z * _Lt.RainKeyDim, 0.0f };

        _CB->MoonDirection = { _Lt.MoonN.X, _Lt.MoonN.Y, _Lt.MoonN.Z, _Lt.MoonW };
        _CB->MoonColor     = { _Lt.MoonLightCol.X, _Lt.MoonLightCol.Y, _Lt.MoonLightCol.Z, 0.0f };
        // MoonTint cru (sem transmitancia), so com o dim de chuva — mesma logica do SunColorRaw.
        _CB->MoonColorRaw  = { 0.6f * _Lt.RainKeyDim, 0.7f * _Lt.RainKeyDim,
                               1.0f * _Lt.RainKeyDim, 0.0f };

        // Transmitancia por pixel: so com o ceu procedural (o LUT descreve ESTA atmosfera).
        {
            const bool AtmoOn = UseAtmosphereSky && Atmosphere.IsInitialized();
            _CB->AtmoLightParams = {
                AtmoOn ? Atmosphere.BottomRadiusKm() : 0.0f,
                AtmoOn ? Atmosphere.TopRadiusKm()    : 0.0f,
                kKmPerWorldUnit,
                (AtmoOn && UsePerPixelAtmoTransmittance) ? 1.0f : 0.0f };
        }

        Atmosphere.SetNightParams(_Lt.MoonN, _Lt.CosMoonRadius, _Lt.MoonDiskBright,
                                  TimeOfDay.StarIntensity, _Lt.NightFactor, FrameState->ElapsedTime);
        Atmosphere.SetMoonSkyLight(_Lt.MoonSkyScale, _Lt.MoonOn ? _Lt.MoonIllum : 0.0f);

        {
            const f32  LatR  = TimeOfDay.LatitudeDeg    * ToRad;
            const f32  NoR   = TimeOfDay.NorthOffsetDeg * ToRad;
            const Vec3 Pole  = { std::cos(LatR) * std::sin(NoR), std::sin(LatR),
                                 std::cos(LatR) * std::cos(NoR) };
            // TOD usa hora sideral local (dia sideral ~23h56m); no modo manual, preserva a
            // rotacao artistica lenta que existia antes.
            const f32 Angle = TimeOfDay.Enabled ? TimeOfDay.StarRotationAngleRad()
                                                : (FrameState->ElapsedTime * 0.004f);
            Atmosphere.SetStarRotation(Pole, Angle);
        }

        FFrameAmbient Amb;
        {
            Vec3& Sky    = Amb.Sky;
            Vec3& Ground = Amb.Ground;
            const bool Physical = UseAtmosphereSky && Atmosphere.IsInitialized() &&
                                  Atmosphere.GetSkyAmbient(_FrameSlot, Sky, Ground);
            if (!Physical) {
                auto Sat = [](f32 X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); };
                const f32 SunY   = _Lt.SunN.Y;
                const f32 Day    = Sat(SunY * 4.0f + 0.2f);
                const f32 LowSun = Sat(1.0f - SunY * 2.5f);
                const Vec3 Zenith  = { 0.18f, 0.30f, 0.55f };
                const Vec3 Horizon = { 0.60f, 0.40f, 0.26f };
                Sky    = (Zenith + (Horizon - Zenith) * LowSun) * Day;
                Ground = Sky * 0.35f;
            }

            Sky    = { Sky.X * _Lt.RainAmbDim, Sky.Y * _Lt.RainAmbDim, Sky.Z * _Lt.RainAmbDim };
            Ground = { Ground.X * _Lt.RainAmbDim, Ground.Y * _Lt.RainAmbDim, Ground.Z * _Lt.RainAmbDim };
            _CB->SkyAmbientColor    = { Sky.X, Sky.Y, Sky.Z,
                                        UseAtmosphereAmbient ? 1.0f : 0.0f };
            _CB->GroundAmbientColor = { Ground.X, Ground.Y, Ground.Z, AtmoAmbientIntensity };

            // SH-L1 do MESMO integral. So vale com o ceu procedural: o fallback analitico acima
            // nao tem SH, e nesse caso o shader cai nas 2 cores chapadas.
            Vec4 SH[3]{};
            const bool HasSH = Physical && Atmosphere.GetSkyAmbientSH(_FrameSlot, SH);
            if (HasSH) {
                // O dim de chuva escurece IRRADIANCIA, entao escala a SH inteira — mesma
                // politica que as 2 cores acima recebem.
                for (u32 c = 0; c < 3; ++c)
                    SH[c] = { SH[c].X * _Lt.RainAmbDim, SH[c].Y * _Lt.RainAmbDim,
                              SH[c].Z * _Lt.RainAmbDim, SH[c].W * _Lt.RainAmbDim };
            }
            _CB->SkyAmbientSHR = SH[0];
            _CB->SkyAmbientSHG = SH[1];
            _CB->SkyAmbientSHB = SH[2];
            _CB->SkyAmbientSHParams = { (HasSH && UseSkyAmbientSH) ? 1.0f : 0.0f,
                                        0.0f, 0.0f, 0.0f };
        }

        // Superficie: estes campos sao lidos pelo DeferredLighting e pelo ForwardBlend.
        if (_Policy.DDGISurface) {
            const Vec3 GMin = DDGI.GridMin();
            const Vec3 GCnt = DDGI.GridCount();
            _CB->DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };

            _CB->DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, GIDebug ? 2.0f : 1.0f };
            _CB->DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                   DDGI.AtlasW(), DDGI.AtlasH() };

            const f32 GIFlags = (GIChebyshev ? 1.0f : 0.0f) + (GISkipInactiveProbes ? 2.0f : 0.0f)
                              + (GISkipInactiveFallback ? 4.0f : 0.0f);
            _CB->DDGIDistParams = { DDGI.DistTileSizeF(), DDGI.DistAtlasW(),
                                    DDGI.DistAtlasH(), GIFlags };
            _CB->DDGIBiasParams = { DDGI.GetSurfaceBiasScale(), DDGI.GetSurfaceBiasMax(),
                                    DDGI.GetVolumeFadeProbes(), 0.0f };
            _CB->DDGICascades   = DDGI.CascadeConstants();
        } else {
            _CB->DDGIGridMin    = { 0.0f, 0.0f, 0.0f, 1.0f };
            _CB->DDGIGridCount  = { 0.0f, 0.0f, 0.0f, 0.0f };
            _CB->DDGIParams     = { 0.0f, 6.0f, 1.0f, 1.0f };
            _CB->DDGIDistParams = { 14.0f, 1.0f, 1.0f, 0.0f };
            _CB->DDGIBiasParams = { 0.2f, 0.0f, 0.0f, 0.0f };
            // O POD neutro preserva espacamento 1 nas quatro cascatas.
            _CB->DDGICascades   = {};
        }

        return Amb;
    }

    void Renderer::PushRayTracingFrameState(const FFrameModes& _Modes,
                                            const FEffectiveIndirectPolicy& _Policy) {
        // DLSS Ray Reconstruction: denoiser neural que substitui NRD + SR. Precisa do RR inicializado
        // e dos guides prontos; o eval acontece no bloco de upscale (ActiveUpscaler() == &DlssRR).
        // Instrumentacao de timer: um gate so, empurrado todo frame. kInvalidSlot manda o passe
        // de volta p/ a PSO normal — desligado nao custa uma instrucao (ver FShaderTimer).
        TimerCaptureActive = RtShaderTimer && FShaderTimer::IsAvailable() &&
                             TimerGI.IsReady() && TimerReflections.IsReady();
        ReSTIRGI.SetTimerSlot(TimerCaptureActive ? TimerGI.UavSlot()
                                                 : FShaderTimer::kInvalidSlot);
        Reflections.SetTimerSlot(TimerCaptureActive ? TimerReflections.UavSlot()
                                                    : FShaderTimer::kInvalidSlot);

        // Perfil de epsilons: um so p/ a engine inteira, empurrado todo frame (copia barata). Sem
        // isto cada passe teria a propria copia e o sweep de calibracao mexeria em metade deles.
        ReSTIRGI.SetRayEpsilons(RayEps);
        Reflections.SetRayEpsilons(RayEps);
        Reflections.SetWaterReflectionScale(Water.GetReflectionScale());
        Reflections.SetWaterWindDirection(Water.GetWindDirection());
        DDGI.SetRayEpsilons(RayEps);
        // O produtor do cache traca os MESMOS raios de gather e de sombra dos outros tres; um
        // perfil proprio aqui seria a copia por passe que a centralizacao fechou.
        RadianceCache.SetRayEpsilons(RayEps);

        // Gather do 2o bounce (ShadeSurfaceHit): mesmo raciocinio do perfil de epsilons — um so
        // para os tres passes, empurrado todo frame. O skipMode espelha exatamente o que o
        // deferred usa, senao o bounce pesaria as sondas com uma politica e a tela com outra.
        {
            FGIHitSampling GIHit;
            GIHit.DistTile   = DDGI.DistTileSizeF();
            GIHit.DistAtlasW = DDGI.DistAtlasW();
            GIHit.DistAtlasH = DDGI.DistAtlasH();
            GIHit.SkipMode   = GISkipInactiveProbes
                             ? (GISkipInactiveFallback ? 2.0f : 1.0f) : 0.0f;
            GIHit.BiasScale  = DDGI.GetSurfaceBiasScale();
            GIHit.BiasMax    = DDGI.GetSurfaceBiasMax();
            GIHit.FadeProbes = DDGI.GetVolumeFadeProbes();
            GIHit.TerminatorOff = GIMeasureTerminatorOff; // gate de medicao (ver Renderer.h)
            // Descriptors seguem a existencia fisica; este gate por frame segue a politica
            // efetiva e impede a leitura dos recursos neutros quando DDGI nao e o fallback.
            GIHit.FallbackAvailable = _Policy.Fallback == EIndirectFallback::DDGI;
            DDGI.SetGIHitSampling(GIHit);
            Reflections.SetGIHitSampling(GIHit);
            const FDDGICascadeConstants GICasc = DDGI.CascadeConstants();
            Reflections.SetGICascades(GICasc);
            ReSTIRGI.SetGICascades(GICasc);
            ReSTIRGI.SetGIHitSampling(GIHit);
            // O produtor do cache recebe o MESMO bloco pelo unico campo que ele le: o piso de
            // roughness do secundario. As cascatas nao vao junto — ele nao amostra sonda, e a
            // cauda do cbuffer dele viaja zerada de proposito (ver RadianceCacheUpdateConstants).
            RadianceCache.SetGIHitSampling(GIHit);
        }

        ReSTIRGI.SetUseNrd(_Modes.NrdIndirectMode); // Modes.RRMode => false => ReSTIR entrega GI cru (ruidoso)
        Reflections.SetUseNrd(_Modes.NrdIndirectMode);
        Reflections.SetRawSpec(_Modes.RRMode);        // reflexao crua (Resolved direto) p/ o RR denoisar
    }

    void Renderer::UpdateAtmosphereAndVolumetrics(const FFrameModes& _Modes,
                                                  const FEffectiveIndirectPolicy& _Policy,
                                                  const FFrameView& _Vw,
                                                  const FFrameLighting& _Lt,
                                                  const FFrameAmbient& _Amb, u32 _FrameSlot,
                                                  FrameConstants* _CB) {
        Atmosphere.UpdatePerFrame(_FrameSlot, _Lt.SunN, _Vw.InvVPNoTrans, _Vw.VPNoTrans,
                                  _Vw.InvViewProjFull, _Vw.CameraPosition, kKmPerWorldUnit,
                                  static_cast<f32>(RenderWidth()), static_cast<f32>(RenderHeight()),
                                  static_cast<f32>(OutputWidth()), static_cast<f32>(OutputHeight()));

        const f32 FogDensityBase = Fog.GetDensity();
        if (_Lt.RainSky > 0.0f) Fog.SetDensity(FogDensityBase * (1.0f + _Lt.RainSky * 1.5f));

        const Vec4 ShaftsFogCollapsed = Fog.CollapsedFogParams(_Vw.CameraPosition.Y);

        Vec3 CamForwardW{ 0.0f, 0.0f, 1.0f };
        {
            const Mat44& IM = _Vw.InvViewProjUnjit;
            const f32 v[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
            f32 w[4];
            for (int j = 0; j < 4; ++j)
                w[j] = v[2] * IM.M[2][j] + v[3] * IM.M[3][j];
            if (std::fabs(w[3]) > 1e-9f) {
                const Vec3 P{ w[0] / w[3], w[1] / w[3], w[2] / w[3] };
                CamForwardW = (P - _Vw.CameraPosition).NormalizedSafe(Vec3{ 0.0f, 0.0f, 1.0f });
            }
        }

        const f32 ShaftMarchMax = _Modes.VolShaftsActive
            ? (_Modes.VolFogActive
                ? std::min(SunShafts.GetVolMaxDist(), VolumetricFog.GetMaxDistance())
                : SunShafts.GetVolMaxDist())
            : 0.0f;
        if (_Modes.VolFogActive) {
            FVolumetricFogPass::FFrameParams VF{};
            VF.InvViewProjUnjit = _Vw.InvViewProjUnjit;
            VF.ViewProjUnjit    = _Vw.ViewProjUnjittered;
            VF.FrameIndex       = FrameState->TemporalSampleIndex;
            VF.CameraPos        = _Vw.CameraPosition;
            VF.CameraForward    = CamForwardW;
            VF.DirToSun         = _Lt.KeyDir;
            VF.SunColorTimesIntensity = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                          _Lt.KeyColor.Z * _Lt.KeyInt };
            // Shafts own the high-frequency solar term only over their configured
            // near range. The froxel resumes the sun afterwards, while extinction,
            // ambient, GI and local lights remain present over the full volume.
            VF.InjectDirectionalLight = true;
            VF.DirectionalLightStartDistance = _Modes.VolShaftsActive ? ShaftMarchMax : 0.0f;
            VF.CollapsedFog     = ShaftsFogCollapsed;
            VF.SkyAmbient       = _Amb.Sky;
            VF.NearZ            = _Vw.NearZ;
            VF.RenderW          = RenderWidth();
            VF.RenderH          = RenderHeight();
            if (_Policy.DDGIVolumetric) {
                const Vec3 GMin = DDGI.GridMin();
                const Vec3 GCnt = DDGI.GridCount();
                VF.DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };
                VF.DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, 1.0f };
                VF.DDGICascades  = DDGI.CascadeConstants();
                VF.DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                     DDGI.AtlasW(), DDGI.AtlasH() };
                VF.DDGIVolumeFadeProbes = DDGI.GetVolumeFadeProbes();
            }
            VolumetricFog.UpdatePerFrame(_FrameSlot, VF);
        } else if (VolumetricFog.IsInitialized()) {
            VolumetricFog.ResetHistory(); // efeito dormiu: historia/PrevVP obsoletos
        }

        // Ancoragem do height fog na atmosfera: so faz sentido com o ceu procedural ligado (com
        // HDRI/skybox o SkyView LUT nao descreve o ceu que esta na tela). Lt.SunN, nao Lt.KeyDir: o
        // LUT e dobrado no azimute do SOL, entao a lua daria uv errado a noite.
        FFogPass::FVolumetricMatchParams FogMatch{};
        FogMatch.Enabled = _Modes.VolFogActive;
        if (_Modes.VolFogActive) {
            const Vec3 MediumAlbedo = VolumetricFog.GetAlbedo();
            const f32 AmbientIntensity = VolumetricFog.GetAmbientIntensity();
            FogMatch.Albedo = MediumAlbedo;
            FogMatch.AmbientRadiance = { _Amb.Sky.X * AmbientIntensity,
                                         _Amb.Sky.Y * AmbientIntensity,
                                         _Amb.Sky.Z * AmbientIntensity };
            FogMatch.SunRadiance = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                     _Lt.KeyColor.Z * _Lt.KeyInt };
            FogMatch.ExtinctionScale = VolumetricFog.GetExtinctionScale();
            // The froxel smoothly regains the solar term before its boundary,
            // so the far analytical continuation always matches the base volume.
            FogMatch.DirectionalPhaseG = VolumetricFog.GetPhaseG();
            FogMatch.DirectionalScatteringScale = 1.0f;
            FogMatch.AmbientTransitionDistance =
                std::max(25.0f, VolumetricFog.GetMaxDistance() * 0.5f);
        }
        Fog.UpdatePerFrame(_FrameSlot, _Vw.InvViewProjFull, _Vw.CameraPosition, kKmPerWorldUnit, _Lt.KeyDir,
                           _Vw.NearZ, _Vw.FarZ, RenderWidth(), RenderHeight(),
                           UseAerialPerspective, UseHeightFog, Atmosphere.AerialDepthKm(),
                           Atmosphere.AerialSliceCount(),
                           _Modes.VolShaftsActive, _Modes.VolFogActive, VolumetricFog.GetMaxDistance(),
                           VolumetricFog.GridZParams(), CamForwardW,
                           _Lt.SunN,
                           _Modes.FogSkyAnchor ? Atmosphere.ViewHeightKm()   : 0.0f,
                           _Modes.FogSkyAnchor ? Atmosphere.BottomRadiusKm() : 0.0f,
                           Fog.GetHeightFogSkyContribution(), FogMatch);
        Fog.SetDensity(FogDensityBase);

        const f32 CloudGroundRadius = 6360.0f + FAtmosphere::kPlanetRadiusOffsetKm;
        const f32 CloudCovBase = VolumetricClouds.GetCoverage();
        if (_Lt.RainSky > 0.0f) {
            const f32 RainCov = std::min(_Lt.RainSky * 1.4f, 1.0f) * 0.92f;
            VolumetricClouds.SetCoverage(std::max(CloudCovBase, RainCov));
        }
        VolumetricClouds.UpdatePerFrame(_FrameSlot, _Vw.InvVPNoTrans, _Vw.InvViewProjFull,
                                        _Vw.ViewProjUnjittered, _Vw.CameraPosition, kKmPerWorldUnit,
                                        CloudGroundRadius, _Lt.KeyDir, _Lt.KeyCloudCol,
                                        _Amb.Sky, _Amb.Ground, FrameState->ElapsedTime, FrameState->TemporalSampleIndex,
                                        SunIntensity);
        VolumetricClouds.SetCoverage(CloudCovBase);

        Vec4 CloudShadowP{ 0.0f, 0.0f, 0.0f, 0.0f };
        Vec4 CloudShadowP2{ 0.0f, 0.0f, 0.0f, 0.0f };
        {
            const f32 KeyY = _Lt.KeyDir.Y > 0.05f ? _Lt.KeyDir.Y : 0.05f;
            const bool CloudShadowOn = UseClouds && VolumetricClouds.IsInitialized() &&
                                       VolumetricClouds.GetShadowsEnabled() && _Lt.KeyDir.Y > 0.02f;
            CloudShadowP  = { VolumetricClouds.ShadowCenterX(),
                              VolumetricClouds.ShadowCenterZ(),
                              VolumetricClouds.ShadowInvExtent(),
                              CloudShadowOn ? VolumetricClouds.GetShadowStrength() : 0.0f };
            CloudShadowP2 = { kKmPerWorldUnit,
                              VolumetricClouds.GetBottomAltitude(),
                              _Lt.KeyDir.X / KeyY, _Lt.KeyDir.Z / KeyY };
            _CB->CloudShadowParams  = CloudShadowP;
            _CB->CloudShadowParams2 = CloudShadowP2;
            if (VolumetricClouds.IsInitialized()) {
                D3D12_CPU_DESCRIPTOR_HANDLE Dst = Backend->SRVHeap.CpuHandle(GBuffer.SRVTableStart() + 4);
                D3D12_CPU_DESCRIPTOR_HANDLE Src =
                    Backend->SRVHeap.CpuHandleStaging(VolumetricClouds.ShadowSRV());
                UINT One = 1;
                Backend->Device.Native()->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }

        if (_Modes.VolShaftsActive) {
            const Vec3 KeyColInt = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                     _Lt.KeyColor.Z * _Lt.KeyInt };
            const f32 ShaftNoiseFrame =
                (_Modes.TAAActive || _Modes.UpscaleActive || SunShafts.GetVolTemporal())
                    ? static_cast<f32>(FrameState->TemporalSampleIndex % 64u) : 0.0f;
            const Vec3 ShaftMediumAlbedo = _Modes.VolFogActive
                ? VolumetricFog.GetAlbedo() : Vec3{ 1.0f, 1.0f, 1.0f };
            const f32 ShaftExtinctionScale = _Modes.VolFogActive
                ? VolumetricFog.GetExtinctionScale() : 1.0f;
            SunShafts.UpdateVolumetric(_FrameSlot, _Lt.KeyDir, KeyColInt, ShaftsFogCollapsed,
                                       ShaftNoiseFrame, _Vw.InvViewProjFull, _Vw.CameraPosition,
                                       _Vw.ViewProjUnjittered, CloudShadowP, CloudShadowP2,
                                       ShaftMediumAlbedo, ShaftExtinctionScale,
                                       0.0f, ShaftMarchMax);
        } else if (SunShafts.IsInitialized()) {
            SunShafts.ResetHistory();
        }

        if (_Modes.VolFogActive) VolumetricFog.PatchCloudShadow(CloudShadowP, CloudShadowP2);
    }

    void Renderer::UpdateWaterAndOcean(const FFrameModes& _Modes, const FFrameView& _Vw,
                                       const FFrameLighting& _Lt, const FFrameAmbient& _Amb,
                                       u32 _FrameSlot) {
        if (!(UseWater && Water.IsInitialized())) return;

        const bool WaterAtmoRefl = UseAtmosphereSky && Atmosphere.IsInitialized();
        const f32 WaterReflIntensity =
            WaterAtmoRefl ? (1.0f - _Lt.RainSky * 0.65f) : IBLIntensity;
        // ViewProjection e a sua inversa vem do FFrameView: o `WaterInvViewProj` local era
        // `Vw.ViewProjection.Inverse()`, EXATAMENTE a expressao que produz o InvViewProjFull
        // (ver ResolveFrameView) — mesma entrada, mesma chamada. Era um inverse 4x4 por frame
        // recalculado a toa.
        Water.UpdatePerFrame(_FrameSlot, _Vw.ViewProjection, _Vw.Projection, _Vw.InvViewProjFull,
                             _Vw.ViewProjUnjittered, FrameState->PrevViewProj, _Vw.CameraPosition, _Lt.KeyDir,
                             _Lt.KeyInt, _Lt.KeyColor, _Amb.Sky, FrameState->ElapsedTime,
                             WaterAtmoRefl || HDREnv.HasHDRLoaded(), WaterReflIntensity,
                             RenderWidth(), RenderHeight(), _Vw.NearZ, _Vw.FarZ,
                             _Modes.WaterSceneCopiesReady, UseAtmosphereSky,
                             _Modes.DedicatedWaterReflections);
        for (u32 c = 0; c < kOceanCascades; ++c) {
            if (!Ocean[c].IsInitialized()) continue;
            Ocean[c].SetTime(FrameState->ElapsedTime);
            Ocean[c].SetWindDirection(Water.GetWindDirection());
            Ocean[c].SetWindSpeed(Water.GetWindSpeed());
            Ocean[c].SetSpectrumFetch(Water.GetSpectrumFetch());
            Ocean[c].SetOceanDepth(Water.GetOceanDepth());
            Ocean[c].SetSwell(Water.GetSwell());
            Ocean[c].SetAmplitude(Water.GetWavesAmount());
            Ocean[c].SetGeometryScales(Water.GetFFTDisplacementScale() *
                                       Water.GetWavesSize(),
                                       Water.GetFFTChoppyScale());
        }
    }

    FPassContext Renderer::MakePassContext(const FFrameModes& _Modes,
                                           const FEffectiveIndirectPolicy& _Policy,
                                           const FFrameView& _Vw,
                                           const FFrameLighting& _Lt, const FFrameAmbient& _Amb,
                                           u32 _FrameSlot) {
        FPassContext C;
        C.Cmd      = Backend->DirectQueue.List();
        C.Device   = Backend->Device.Native();
        C.SRVHeap  = &Backend->SRVHeap;
        C.Profiler = &Backend->DirectProfiler;

        C.FrameSlot    = _FrameSlot;
        C.RenderWidth  = RenderWidth();
        C.RenderHeight = RenderHeight();
        C.OutputWidth  = OutputWidth();
        C.OutputHeight = OutputHeight();

        C.Modes   = &_Modes;
        C.View    = &_Vw;
        C.Light   = &_Lt;
        C.Ambient = &_Amb;
        C.Policy  = &_Policy;

        C.Targets      = &Targets;
        C.FrameCB      = ConstantBuffer->GetGPUVirtualAddress() +
                         static_cast<u64>(_FrameSlot) * sizeof(FrameConstants);
        C.ObjectCBBase = ObjectCB->GetGPUVirtualAddress();
        C.DSV          = Targets.DSVHeap.CpuHandle(0);

        C.Viewport.Width    = static_cast<FLOAT>(C.RenderWidth);
        C.Viewport.Height   = static_cast<FLOAT>(C.RenderHeight);
        C.Viewport.MinDepth = 0.0f;
        C.Viewport.MaxDepth = 1.0f;
        C.Scissor.right     = static_cast<LONG>(C.RenderWidth);
        C.Scissor.bottom    = static_cast<LONG>(C.RenderHeight);
        return C;
    }

    void Renderer::RenderFrame() {
        // O editor ressubmete DebugDraw a cada tick; limpe-o em toda saida do frame.
        struct FDebugDrawClearGuard {
            FDebugDraw& Target;
            ~FDebugDrawClearGuard() { Target.Clear(); }
        } DebugDrawGuard{ DebugDraw };

        if (!Initialized) return;

        // Dirty flags da UI sao coalescidas por dominio antes de abrir o command list.
        if (SceneState->MaterialRTStateDirty) {
            SceneState->MaterialRTStateDirty = false;
            Settings().NotifyMaterialRTStateChanged();
            // Qualquer edicao pode alterar radiancia ou participacao de uma mesh emissiva.
            SceneState->MeshLightEmissiveDirty = true;
        }
        if (SceneState->IndirectLightingDirty) {
            SceneState->IndirectLightingDirty = false;
            Settings().NotifyIndirectLightingChanged();
        }
        if (SceneState->SceneContentDirty) {
            SceneState->SceneContentDirty = false;
            Settings().NotifySceneContentChanged();
        }

        // Invalide quando o novo dominio for publicado, nao quando a reconstrucao for pedida.
        if (MeshLights.ConsumeDomainPublish()) Settings().NotifyMeshDomainChanged();

        // Tasks guardam transform e radiancia extraidos. Reconcilie antes do BeginFrame e espere
        // o fim do arraste para nao invalidar a alias table a cada tick.
        if (MeshLights.IsReady() && SceneState->DraggingRenderableId == 0 &&
            (SceneState->MeshLightEmissiveDirty ||
             SceneState->Scene.TransformsVersion() != SceneState->MeshLightTransformsVersion)) {
            switch (MeshLights.RefreshTransforms(SceneState->Scene)) {
            case FMeshLights::ETransformRefresh::Busy:
                // Preserve o pedido enquanto a extracao ainda le as tasks.
                break;
            case FMeshLights::ETransformRefresh::SetChanged:
                // O conjunto emissivo mudou; a lista de tasks precisa ser reconstruida.
                RebuildMeshLights();
                break;
            case FMeshLights::ETransformRefresh::Applied:
                SceneState->MeshLightTransformsVersion = SceneState->Scene.TransformsVersion();
                SceneState->MeshLightEmissiveDirty     = false; // o Applied ja marcou sujo: a extracao roda
                break;
            case FMeshLights::ETransformRefresh::Unchanged:
                SceneState->MeshLightTransformsVersion = SceneState->Scene.TransformsVersion();
                // Ninguem se MOVEU, mas a radiancia pode ter mudado. A extracao le a cor do
                // InstanceGeo (ja reescrito pelo NotifyMaterialRTStateChanged acima), entao um
                // MarkDirty basta: refaz fluxo por triangulo e alias table sem realocar nada.
                if (SceneState->MeshLightEmissiveDirty) {
                    MeshLights.MarkDirty();
                    SceneState->MeshLightEmissiveDirty = false;
                }
                break;
            }
        }

        // Captura deterministica: abre a sessao (preset + reset) ou desfaz o preset da anterior.
        // AQUI, junto das coalescencias acima e antes do BeginFrame, pelo mesmo motivo delas —
        // trocar upscaler/render scale realoca os alvos da cena, e isso nao pode acontecer com um
        // command list aberto.
        UpdateFrameCapture();

        Backend->DirectQueue.BeginFrame();

        Backend->DirectProfiler.BeginFrame(Backend->DirectQueue.FrameIndex());
        Backend->DirectProfiler.Begin(Backend->DirectQueue.List(), "Frame (GPU)");

        ObjectPicker.Tick();

        IUpscaler* ActiveUp = ActiveUpscaler();
        // FFrameModes deriva desta unica politica efetiva do frame.
        const FEffectiveIndirectPolicy Policy = ResolveIndirectPolicy();
        const FFrameModes Modes = ResolveFrameModes(Policy);
        const FFrameView  Vw    = ResolveFrameView(Modes, ActiveUp);
        FrameState->LastViewProj = Vw.ViewProjUnjittered;

        const u32 FrameSlot = Backend->DirectQueue.FrameIndex();
        // BeginFrame acabou de esperar a fence deste slot, portanto o readback gravado na
        // utilizacao anterior do slot ja pode ser mapeado sem stall.
        CollectDebugPreviewReadback(FrameSlot);
        RadianceCache.CollectStats(FrameSlot);
        DDGIDebugPass.CollectPointDiagnostic(FrameSlot);

        // Mudancas no gate de consulta invalidam consumidores antes que publiquem seus cbuffers.
        RadianceCache.TickWarmup();
        if (RadianceCache.ConsumeQueryChange()) Settings().NotifyRadianceCacheQueryChanged();
        FrameConstants* MappedCB = reinterpret_cast<FrameConstants*>(
            MappedFrameBase + static_cast<size_t>(FrameSlot) * sizeof(FrameConstants));


        // A iluminacao consome o sol e a molhadura atualizados neste frame.
        TickWorldClock();
        const FFrameLighting Lt  = ResolveFrameLighting();
        // Os publicadores abaixo capturam a colocacao atual das cascatas.
        DDGI.PrepareCascadePlacement(Vw.CameraPosition);
        const FFrameAmbient  Amb = PublishFrameConstants(Vw, Lt, Policy, FrameSlot, MappedCB);
        PushRayTracingFrameState(Modes, Policy);

        MappedCB->ReflectionParams = { Reflections.GetMaxRoughness(), Reflections.GetRoughnessFade(),
                                       Modes.ReflectionsActive ? 1.0f : 0.0f,
                                       Modes.ReSTIRGIActive ? (Modes.NrdIndirectMode ? 2.0f : 1.0f) : 0.0f };

        MappedCB->InvViewProj  = Vw.InvViewProjFull;
        MappedCB->RenderParams = { Vw.MipBias, 0.0f, 0.0f, 0.0f };

        UpdateAtmosphereAndVolumetrics(Modes, Policy, Vw, Lt, Amb, FrameSlot, MappedCB);
        UpdateWaterAndOcean(Modes, Vw, Lt, Amb, FrameSlot);

        FPassContext Ctx = MakePassContext(Modes, Policy, Vw, Lt, Amb, FrameSlot);
        // Aliases sobre o contexto — NAO copias. Sobrevivem a migracao porque os poucos blocos que
        // continuam inline aqui embaixo sao estrutura de FRAME, nao passe: a espera do fence da
        // fila assincrona, o clear dos alvos de instrumentacao e os dois ganchos de debug. Nenhum
        // deles compoe imagem da cena, entao promove-los a fase so aumentaria a lista sem
        // aumentar a clareza.
        auto* CommandList         = Ctx.Cmd;
        const auto& DSV           = Ctx.DSV;
        D3D12_VIEWPORT& Viewport  = Ctx.Viewport;
        D3D12_RECT& ScissorRect   = Ctx.Scissor;
        const u64& GIComputeFence = Ctx.AsyncGIFence;

        BeginSceneRecording(Ctx);
        RecordSkyAndClouds(Ctx);

        PrepareIndirectLighting(Ctx);

        // Root signature + constantes do frame: os passes de compute acima trocaram o estado,
        // e tudo que grava raster daqui pra frente conta com estes dois amarrados.
        CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
        CommandList->SetGraphicsRootConstantBufferView(0, Ctx.FrameCB);

        const FLocalShadowJobs ShadowJobs = PackDirectLights(Ctx, MappedCB);

        BuildDrawLists(Ctx);

        RecordShadows(Ctx, ShadowJobs);

        RecordDepthPrepass(Ctx);

        RecordGBuffer(Ctx);

        // O update nasce do G-buffer e pode sobrepor o DDGI async porque nao o consome.
        // Restaure o depth para DEPTH_WRITE; o G-buffer rastreia o proprio estado.
        if (Modes.RadianceCacheUpdateActive) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Radiance cache (update)");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            RadianceCache.RecordUpdate(CommandList, Backend->SRVHeap);

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.Flush(CommandList);
        }

        if (GIComputeFence != 0) {
            // Meça o stall com dois timestamps da fila direta; clocks de filas diferentes nao
            // sao correlacionados. O valor inclui o overhead de segmentacao e nao mede folga.
            Backend->DirectProfiler.Begin(CommandList, "Espera do DDGI (async)");
            Backend->DirectQueue.SubmitSegmentAndContinue();
            Backend->DirectQueue.GpuWait(Backend->ComputeQueue.NativeFence(), GIComputeFence);
            Backend->DirectProfiler.End(CommandList); // ja no segmento de DEPOIS do wait
            ID3D12DescriptorHeap* Heaps[] = { Backend->SRVHeap.Native() };
            CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGI.TransitionForRead(CommandList);
        }

        // Antes dos dois traces instrumentados: pixel que sai cedo nao escreve, e sem zerar o
        // alvo ele mostraria a medida de um frame antigo — quente onde nao houve trabalho nenhum.
        if (TimerCaptureActive) {
            TimerGI.Clear(CommandList, Backend->SRVHeap);
            TimerReflections.Clear(CommandList, Backend->SRVHeap);
        }

        // Debug da BVH (GPU Zen 3, 7.3.3). Dispatch proprio, independente do resto do frame: ele
        // traca os raios PRIMARIOS que o pipeline hibrido nunca traca, entao nao le G-buffer nem
        // depth e nao entra em nenhuma cadeia de dependencia. Sem o toggle, nao custa nada.
        // Nao precisa de clear: todo pixel do dominio escreve (o miss tem cor propria).
        if (BvhDebugEnabled && BvhDebug.IsReady()) {
            BvhDebug.Render(CommandList, Backend->SRVHeap, Vw.InvViewProjFull, Vw.CameraPosition,
                            BvhDebugMode, DDGI.MaxRayDistance(), BvhDebugComplexityMax,
                            FrameSlot);
        }

        RecordSceneLighting(Ctx);

        RecordForwardAndClouds(Ctx);

        // Registrado no proprio sitio (e nao reavaliando a condicao la embaixo) porque o que importa
        // p/ o RR e se a geometria de debug FOI desenhada no HDR, nao se ela estava habilitada.
        bool DDGIDebugDrew = false;
        if (Backend->Device.RaytracingSupported() && DDGIDebugPass.GetEnabled() && DDGI.IsReady()) {
            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGIDebugPass.Render(FrameSlot, CommandList, Backend->SRVHeap, DDGI, Vw.ViewProjection, Vw.CameraPosition,
                                 FrameState->TemporalSampleIndex);
            DDGIDebugDrew = true;
        }

        RecordVolumetricsAndRain(Ctx);

        const bool RRPoisoned = RecordDebugViews(Ctx, DDGIDebugDrew);

        {
            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        const FPostInput PostSrc = RecordResolve(Ctx, ActiveUp, RRPoisoned);

        FrameState->PrevViewProj  = Vw.ViewProjUnjittered;
        FrameState->PrevCameraPosition = Vw.CameraPosition;
        // Reprojecao do background, sem translacao da camera.
        FrameState->PrevViewProjNoTranslation = Vw.VPNoTransUnjit;
        FrameState->NrdPrevProj   = Vw.ProjUnjittered;
        FrameState->NrdPrevView   = Vw.View;
        FrameState->PrevJitterUv  = Vw.JitterUv;
        FrameState->PrevJitterPx = Vw.JitterPx;

        RecordPost(Ctx, PostSrc);

        Backend->DirectProfiler.End(CommandList);
        Backend->DirectProfiler.Resolve(CommandList);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* CommandLists[] = { CommandList };

        AsyncGIRanLastFrame = (GIComputeFence != 0);
        Backend->DirectQueue.EndFrame(CommandLists, 1);

        // Avanca o aquecimento e, no frame de captura, grava PNG + manifesto. ANTES do ++ dos
        // contadores logo abaixo: o manifesto grava o TemporalSampleIndex com que este frame
        // amostrou, e ele e a prova de que o contrato "aquece 0..N-1, captura em N" valeu.
        // Recebe os Modes deste frame porque o manifesto registra o que RODOU, nao o que foi
        // pedido — ver CollectCaptureState.
        FinishFrameCapture(Modes, Policy, FrameSlot);

        // Avance no fim para que jitter e todos os passes usem a mesma semente durante o frame.
        // O reset deterministico altera apenas o indice de amostra temporal.
        ++FrameState->FrameIndex;
        ++FrameState->TemporalSampleIndex;
    }
}
