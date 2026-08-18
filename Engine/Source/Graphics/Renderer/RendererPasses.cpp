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
    // Abre a gravacao da cena: limpa cor/mascaras/profundidade e amarra viewport, scissor e o
    // heap de descriptors que valem pelo frame inteiro.
    void Renderer::BeginSceneRecording(FPassContext& _Ctx) {
        auto* CommandList = _Ctx.Cmd;
        const auto& DSV   = _Ctx.DSV;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        {
            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.UpscaleReactiveMask.Get(), Targets.UpscaleReactiveState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.UpscaleCompositionMask.Get(), Targets.UpscaleCompositionState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto HDR_RTV = Targets.HDRRTVHeap.CpuHandle(0);
            const FLOAT MaskClear[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            CommandList->OMSetRenderTargets(1, &HDR_RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(HDR_RTV, ClearColor, 0, nullptr);
            CommandList->ClearRenderTargetView(
                Targets.UpscaleMaskRTVHeap.CpuHandle(0), MaskClear, 0, nullptr);
            CommandList->ClearRenderTargetView(
                Targets.UpscaleMaskRTVHeap.CpuHandle(1), MaskClear, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);
        }

        CommandList->RSSetViewports(1, &_Ctx.Viewport);
        CommandList->RSSetScissorRects(1, &_Ctx.Scissor);

        ID3D12DescriptorHeap* DescriptorHeaps[] = { Backend->SRVHeap.Native() };
        CommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);
    }

    // Simulacao da agua na GPU, ceu/atmosfera e o shadow map das nuvens. Tudo isto escreve em
    // alvos PROPRIOS dos subsistemas (LUTs, cascatas de FFT, mapa de sombra) e no HDR ainda
    // vazio — nao depende de geometria nenhuma, por isso abre o frame.
    void Renderer::RecordSkyAndClouds(FPassContext& _Ctx) {
        auto* CommandList        = _Ctx.Cmd;
        const FFrameView& Vw     = *_Ctx.View;
        const FFrameLighting& Lt = *_Ctx.Light;
        const u32 FrameSlot      = _Ctx.FrameSlot;

        if (UseWater && Ocean[0].IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Água — FFT");
            for (u32 c = 0; c < kOceanCascades; ++c)
                Ocean[c].RecordCompute(FrameSlot, CommandList, Backend->SRVHeap);
        }

        Backend->DirectProfiler.Begin(CommandList, "Céu e atmosfera");
        // Antes do sky-view: se algum parametro fisico mudou (MarkDirty), transmitancia e
        // multiscatter sao re-bakeadas na command list deste frame. No-op no caso comum.
        if (Atmosphere.IsInitialized()) Atmosphere.RecordBakeIfDirty(CommandList);
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            Atmosphere.RecordSkyViewBake(CommandList);
            Atmosphere.RecordSkyAmbientIntegration(CommandList);
            if (UseWater && Water.IsInitialized())
                Atmosphere.RecordSkyReflectionBake(CommandList);
            Atmosphere.RenderSky(CommandList, Backend->SRVHeap);
            if (Lt.NightFactor > 0.001f && TimeOfDay.StarIntensity > 0.0f)
                Atmosphere.RenderStars(CommandList, Backend->SRVHeap);
        } else if (ShowSkybox && HDREnv.HasHDRLoaded()) {
            Skybox.Render(FrameSlot, CommandList, Backend->SRVHeap, HDREnv.EnvCubeSRV(),
                          Vw.InvVPNoTrans, IBLIntensity, IBLRotation);
        }

        if (Atmosphere.IsInitialized()) {
            Atmosphere.RecordAerialPerspectiveBake(CommandList);
        }
        Backend->DirectProfiler.End(CommandList); // Céu e atmosfera

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Sombra das nuvens");
            VolumetricClouds.RecordShadowMap(CommandList, Backend->SRVHeap);
        }
    }

    // Empacota as luzes puntuais para o MUNDO INDIRETO e grava os caches que os raios leem:
    // extracao de mesh lights, build do ReGIR e o passe do DDGI (fila assincrona quando da).
    // Termina empurrando o estado por-frame para reflexoes e ReSTIR GI, que e onde a contagem
    // de luzes empacotada acima e consumida — por isso os tres andam juntos.
    void Renderer::PrepareIndirectLighting(FPassContext& _Ctx) {
        auto* CommandList             = _Ctx.Cmd;
        const FFrameModes& Modes      = *_Ctx.Modes;
        const FEffectiveIndirectPolicy& Policy = *_Ctx.Policy;
        const FFrameView& Vw          = *_Ctx.View;
        const FFrameLighting& Lt      = *_Ctx.Light;
        const u32 FrameSlot           = _Ctx.FrameSlot;
        const auto& DSV               = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        u64& GIComputeFence           = _Ctx.AsyncGIFence; // sai daqui e e esperado la na frente

        u32 GILightCount = 0;
        u64 GILightSetSignature = 1469598103934665603ull; // FNV-1a dos IDs na ordem compacta
        {
            FGPULightGI* Dst = reinterpret_cast<FGPULightGI*>(
                MappedGILightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULightGI));
            for (FLight& L : SceneState->Scene.Lights()) {
                // O caminho direto atribui a identidade mais adiante, mas o ReGIR e construido
                // antes dele. Atribuir aqui garante que o historico nunca use indice como ID.
                if (L.Id == 0) L.Id = SceneState->Scene.AllocObjectId();
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                // Peso de RT: com 0 a luz sai da lista do indireto por completo (nao so escurece —
                // some do hit, economizando o shadow ray dela). E o caso da luz que so existia p/
                // representar uma malha emissiva que agora ilumina sozinha. O raster nao ve isto.
                if (L.RTWeight <= 0.0f) continue;
                if (GILightCount >= kMaxLights) break;

                const f32 RTW = L.RTWeight;
                FGPULightGI G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity * RTW, L.Color.Y * L.Intensity * RTW,
                                        L.Color.Z * L.Intensity * RTW,
                                        std::max(L.SourceRadius, 0.01f) };
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter = std::cos(OuterDeg * ToRad);
                    const f32 CosInner = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      0.0f, 0.0f, 0.0f };
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f };
                    G.SpotParams  = { 0.0f, 0.0f, 0.0f, 0.0f };
                }
                Dst[GILightCount++] = G;
                GILightSetSignature ^= L.Id;
                GILightSetSignature *= 1099511628211ull;
            }
        }
        GILightSetSignature ^= static_cast<u64>(GILightCount);

        // TlasFlagsDirty: mask/FORCE_NON_OPAQUE/two-sided de uma instancia mudaram (edicao de
        // material no editor) sem a cena se mexer, entao a versao de transforms sozinha nao
        // pediria rebuild.
        if (RaytracingScene.IsBuilt() &&
            (SceneState->Scene.TransformsVersion() != SceneState->TlasTransformsVersion ||
             SceneState->TlasFlagsDirty)) {
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> TlasCL;
            if (SUCCEEDED(CommandList->QueryInterface(IID_PPV_ARGS(&TlasCL))) &&
                RaytracingScene.RecordTlasRebuild(TlasCL.Get(), SceneState->Scene, FrameSlot)) {
                SceneState->TlasTransformsVersion = SceneState->Scene.TransformsVersion();
                SceneState->TlasFlagsDirty        = false;
            }
        }

        // ReGIR e produzido antes da bifurcacao async. Construa-o apenas quando algum trace,
        // inclusive o update dedicado do cache, consumir iluminacao local secundaria.
        const bool HasReGIRConsumer = Policy.VolumeLive ||
                                      Modes.ReflectionsActive || Modes.ReSTIRGIActive ||
                                      Modes.RadianceCacheUpdateActive;
        // Extracao dos triangulos emissivos. Sai de graca no caso comum: a geometria emissiva e
        // estatica, entao o Record so faz trabalho no primeiro frame apos carregar a cena.
        if (MeshLights.IsReady()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "MeshLights (extract)");
            MeshLights.Record(CommandList, Backend->SRVHeap);
        }

        const bool ReGIROn = Settings().ReGIRActive() && HasReGIRConsumer && GILightCount > 0;
        // O gate real do ReGIR e este, e nao o toggle: sem consumidor ou sem luz na cena a grade
        // nem chega a ser construida. O manifesto da captura le daqui — reconstituir a condicao
        // por fora e o comeco de uma divergencia.
        ReGIRRanThisFrame     = ReGIROn;
        GILightCountThisFrame = GILightCount;
        if (ReGIROn) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "ReGIR (build)");
            ReGIR.UpdatePerFrame(FrameSlot, FrameState->TemporalSampleIndex, GILightCount, GILightSetSignature);
            ReGIR.RecordBuild(CommandList, Backend->SRVHeap);
        }
        const FReGIRShaderParams ReGIRCB = ReGIR.ShaderParams(ReGIROn);
        DDGI.SetReGIRParams(ReGIRCB);
        Reflections.SetReGIRParams(ReGIRCB);
        ReSTIRGI.SetReGIRParams(ReGIRCB);

        // World radiance cache — mesmo padrao do ReGIR acima. A camera entra porque o nivel do
        // hash sai da distancia ate ela.
        RadianceCache.UpdatePerFrame(FrameSlot, Vw.CameraPosition);
        RadianceCache.SetReGIRParams(ReGIRCB);
        // A politica escolhe um unico produtor; nunca caia silenciosamente no legado.
        const bool LegacyProducer = !RadianceCache.GetDedicatedUpdate();
        // O segundo argumento e "este consumidor vai tracar neste frame". Os params sao montados
        // para os tres de qualquer jeito (custa nada, e o passe pode nem rodar), mas so quem roda
        // entra no registro que o manifesto le — senao a captura afirmaria cache ativo num frame
        // em que nenhum trace o consultou.
        const bool DDGIWillTrace = Policy.VolumeLive;
        DDGI.SetRadianceCacheParams(RadianceCache.ShaderParams(LegacyProducer, DDGIWillTrace));
        ReSTIRGI.SetRadianceCacheParams(
            RadianceCache.ShaderParams(LegacyProducer, Modes.ReSTIRGIActive));
        Reflections.SetRadianceCacheParams(
            RadianceCache.ShaderParams(false, Modes.ReflectionsActive));
        // Os tres buffers precisam estar em UAV antes de QUALQUER trace: a escrita e a leitura
        // acontecem dentro do ShadeSurfaceHit, que os cinco shaders de trace compartilham.
        RadianceCache.TransitionForTrace(CommandList);

        // Raster e traces compartilham a parametrizacao da sky-view; a altura segue a camera.
        const f32 SkyViewHeightKm = Atmosphere.ViewHeightKm();
        const f32 SkyBottomRKm    = Atmosphere.BottomRadiusKm();
        DDGI.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        Reflections.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        ReSTIRGI.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        RadianceCache.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        DDGI.SetSkyIntensity(Lt.RainSkyDim); // reflexoes e ReSTIR recebem no proprio UpdatePerFrame

        if (Modes.ReliableMotionActive) {
            TemporalMotion.UpdatePerFrame(FrameSlot, SceneState->Scene, Vw.InvViewProjFull,
                                          Vw.ViewProjUnjittered, FrameState->PrevViewProj,
                                          Vw.CameraPosition);
        } else {
            // O historico de superficie nao e atualizado enquanto nenhum consumidor existe;
            // ao religar, o primeiro frame deve cair no velocity raster em vez de ler pose velha.
            TemporalMotion.InvalidateHistory();
        }

        GIComputeFence = 0;
        if (Policy.VolumeLive) {
            DDGI.SetPunctualLightsSRV(Backend->Device.Native(), Backend->SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            DDGI.UpdatePerFrame(FrameSlot, Lt.KeyDir, Lt.KeyInt, Lt.KeyColor,
                                FrameState->TemporalSampleIndex, GILightCount);
            if (UseAsyncCompute && DDGI.CanRunAsync()) {
                DDGI.TransitionForUpdate(CommandList);
                const u64 S1 = Backend->DirectQueue.SubmitSegmentAndContinue();

                ID3D12GraphicsCommandList* CCL = Backend->ComputeQueue.Begin();
                Backend->ComputeProfiler.BeginFrame(Backend->ComputeQueue.CurrentSlot());
                ID3D12DescriptorHeap* CHeaps[] = { Backend->SRVHeap.Native() };
                CCL->SetDescriptorHeaps(_countof(CHeaps), CHeaps);
                {
                    FGpuScope Scope(Backend->ComputeProfiler, CCL, "DDGI (async)");
                    DDGI.RecordUpdate(CCL, Backend->SRVHeap);
                }
                Backend->ComputeProfiler.Resolve(CCL);
                GIComputeFence = Backend->ComputeQueue.SubmitAfter(Backend->DirectQueue.NativeFence(), S1);

                ID3D12DescriptorHeap* Heaps[] = { Backend->SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            } else {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "DDGI");
                DDGI.TransitionForUpdate(CommandList);
                DDGI.RecordUpdate(CommandList, Backend->SRVHeap);
                DDGI.TransitionForRead(CommandList);
            }
        }

        if (Modes.ReflectionsActive) {
            Reflections.SetPunctualLightsSRV(Backend->Device.Native(), Backend->SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            const f32 ReflSkyIntensity = Lt.RainSkyDim;
            const bool WaterUsesAtmosphere = UseAtmosphereSky && Atmosphere.IsInitialized();
            const f32 WaterEnvironmentIntensity =
                WaterUsesAtmosphere ? ReflSkyIntensity : IBLIntensity;
            Reflections.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.ViewProjection,
                                       FrameState->PrevViewProj, Vw.CameraPosition,
                                       FrameState->PrevCameraPosition,
                                       RenderWidth(), RenderHeight(), Lt.KeyDir, Lt.KeyInt,
                                       Lt.KeyColor, FrameState->TemporalSampleIndex, ReflSkyIntensity, Vw.View,
                                       WaterUsesAtmosphere, WaterEnvironmentIntensity, GILightCount,
                                       TemporalMotion.InstanceCount(), Modes.MotionHistoryValidThisFrame);
        }

        if (Modes.ReSTIRGIActive) {
            ReSTIRGI.SetPunctualLightsSRV(Backend->Device.Native(), Backend->SRVHeap, GILightSRVSlot[FrameSlot]);
            // O x1 do reuso temporal vem do historico de superficie do frame ANTERIOR (o reservoir
            // deixou de guardar o ponto visivel). Mesma convencao do ReSTIR DI e das reflexoes:
            // FrameSlot e o corrente, 1 - FrameSlot e o anterior. Sem motion confiavel neste frame
            // o slot vai invalido e o FReSTIRGI derruba so o reuso temporal.
            const u32 PrevSurfaceSlot = (Modes.ReliableMotionActive && Modes.MotionHistoryValidThisFrame)
                                      ? TemporalMotion.SurfaceSRV(1u - FrameSlot) : 0xFFFFFFFFu;
            ReSTIRGI.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.CameraPosition,
                                    RenderWidth(), RenderHeight(), Lt.KeyDir, Lt.KeyInt, Lt.KeyColor,
                                    FrameState->TemporalSampleIndex, Lt.RainSkyDim, Vw.View,
                                    FrameState->PrevJitterUv - Vw.JitterUv, PrevSurfaceSlot, GILightCount);
        }

        // Cache e ReSTIR GI compartilham a mascara de sombra para produzir a mesma radiancia.
        if (Modes.RadianceCacheUpdateActive) {
            RadianceCache.SetUpdatePunctualLightsSRV(Backend->Device.Native(), Backend->SRVHeap,
                                                     GILightSRVSlot[FrameSlot], FrameSlot);
            RadianceCache.UpdatePassPerFrame(
                FrameSlot, Vw.InvViewProjFull, Vw.CameraPosition,
                Lt.KeyDir, Lt.KeyInt, Lt.KeyColor,
                ReSTIRGI.GetFoliageShadows() ? kRTMaskShadowFull : kRTMaskShadowFast,
                FrameState->TemporalSampleIndex, Lt.RainSkyDim, DDGI.MaxRayDistance(), GILightCount,
                ReSTIRGI.GetBackfacePolicy());
        }
    }

    // TAA e upscaler sao exclusivos. Retorna a imagem HDR em render-res ou display-res.
    // A fase de debug informa quando o HDR nao pode alimentar o RR.
    FPostInput Renderer::RecordResolve(FPassContext& _Ctx, IUpscaler* _ActiveUp, bool _RRPoisoned) {
        auto* CommandList        = _Ctx.Cmd;
        const FFrameModes& Modes = *_Ctx.Modes;
        const FFrameView& Vw     = *_Ctx.View;
        const u32 FrameSlot      = _Ctx.FrameSlot;
        IUpscaler* ActiveUp      = _ActiveUp;
        const bool RRPoisoned    = _RRPoisoned;

        // Zerado no topo e marcado SO no caminho que chega ao Dispatch — mesmo padrao do
        // ReGIRRanThisFrame. Esta funcao roda uma vez por frame e sem retorno antecipado antes
        // daqui, entao o zero vale para o frame inteiro.
        RRRanThisFrame = false;

        ID3D12Resource* PostInput    = Targets.HDRColorBuffer.Get();
        u32             PostInputSRV = Targets.HDRSRVSlot;
        if (Modes.TAAActive) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "TAA");
                TemporalAA.Execute(CommandList, Backend->SRVHeap, FrameSlot, Vw.InvViewProjUnjit,
                                   FrameState->PrevViewProj, TAAHistoryBlend,
                                   FrameState->TAARanLastFrame, TAAVarianceGamma, TAASharpness,
                                   TAAMotionBlend, TAAAntiFlicker, TAAStationaryMargin, Vw.CameraPosition,
                                   Vw.NearZ, Vw.FarZ, TAADebugMode);
            }

            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);

            PostInput    = TemporalAA.DisplayOutputResource();
            PostInputSRV = TemporalAA.DisplayOutputSRVSlot();
            FrameState->TAARanLastFrame = true;
        } else if (Modes.UpscaleActive && !RRPoisoned) {
            // Modes.RRMode: o passe ativo e o proprio RR (ActiveUp == &DlssRR); ele denoisa a cor RUIDOSA
            // (GI+reflexao pre-denoise) e faz o upscale num eval so, guiado pelos buffers de material.
            // MESMO predicado do bloco de sinal (Modes.RRMode, ja em escopo): recalcular so pelo Denoiser
            // divergia dele — os guides podiam nao estar prontos e este bloco tentava o eval do RR
            // enquanto o GI/reflexao ja tinham sido configurados como crus.
            const bool IsRR = Modes.RRMode;

            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.VelocityBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.UpscaleReactiveMask.Get(), Targets.UpscaleReactiveState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.UpscaleCompositionMask.Get(), Targets.UpscaleCompositionState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (IsRR) GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            if (IsRR) {
                // Guides de material do RR (albedo difuso/especular, normal-roughness) do G-buffer
                // [A,B,C,Depth] (mesma tabela contigua do deferred) + FrameCB. O specHitDist ja foi
                // extraido no bloco da reflexao (ou fica zerado sem reflexoes). Deixa tudo em NON_PIXEL.
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "DLSS-RR guides");
                RRGuides.RecordGuides(CommandList, Backend->SRVHeap, Backend->SRVHeap.GpuHandle(GBuffer.SRVTableStart()),
                                      _Ctx.FrameCB);
                if (!Modes.ReflectionsActive) RRGuides.ClearSpecHitDist(CommandList, Backend->SRVHeap);
                RRGuides.TransitionForRR(CommandList);
            }

            FUpscaleParams UpParams{};
            UpParams.Color        = Targets.HDRColorBuffer.Get();
            UpParams.Depth        = Targets.DepthBuffer.Get();
            UpParams.Velocity     = Targets.VelocityBuffer.Get();
            UpParams.Reactive     = Targets.UpscaleReactiveMask.Get();
            UpParams.TransparencyAndComposition = Targets.UpscaleCompositionMask.Get();
            UpParams.JitterX      = Vw.JitterPxX;
            UpParams.JitterY      = Vw.JitterPxY;
            UpParams.NearZ        = Vw.NearZ;
            UpParams.FarZ         = Vw.FarZ;
            UpParams.FovYRadians  = Vw.FovY;
            UpParams.AspectRatio  = Vw.Aspect;
            UpParams.DeltaTimeSec = FrameState->LastDeltaTime;
            UpParams.Quality      = UpscalerQuality;
            // Reset one-shot: descarta o historico temporal em troca de modo/denoiser/scene/resize (senao o
            // RR/DLSS reusa acumulacao velha => ghosting). Limpo logo apos o Dispatch.
            UpParams.Reset        = RRResetPending;
            // Matrizes p/ o DLSS (o FSR ignora): projecao unjittered + reprojecao (clip atual -> anterior).
            // O VP anterior ainda nao foi atualizado neste ponto do frame.
            UpParams.ViewToClip     = Vw.ProjUnjittered;
            UpParams.ClipToPrevClip =
                Vw.ViewProjUnjittered.Inverse() * FrameState->PrevViewProj;
            {
                const Mat44 InvView = Vw.View.Inverse();   // view->world: linhas = base da camera em mundo
                UpParams.CamRight = { InvView.M[0][0], InvView.M[0][1], InvView.M[0][2] };
                UpParams.CamUp    = { InvView.M[1][0], InvView.M[1][1], InvView.M[1][2] };
                UpParams.CamFwd   = { InvView.M[2][0], InvView.M[2][1], InvView.M[2][2] };
                UpParams.CamPos   = { InvView.M[3][0], InvView.M[3][1], InvView.M[3][2] };
            }
            if (IsRR) {
                // Guides que so o RR consome (o FSR/DLSS-SR ignoram). WorldToView (row-major, sem jitter)
                // = Vw.View: o RR deriva os specular motion vectors do specHitDist + matrizes world<->view.
                UpParams.DiffuseAlbedo   = RRGuides.DiffuseAlbedo();
                UpParams.SpecularAlbedo  = RRGuides.SpecularAlbedo();
                UpParams.NormalRoughness = RRGuides.NormalRoughness();
                UpParams.SpecHitDist     = RRGuides.SpecHitDist();
                UpParams.WorldToView     = Vw.View;
            }

            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList,
                                IsRR ? "DLSS-RR" : (Upscaler == EUpscaler::DLSS ? "DLSS-SR" : "FSR"));
                ActiveUp->Dispatch(CommandList, UpParams);
            }
            // Marque RR somente depois de um dispatch efetivo.
            RRRanThisFrame = IsRR;
            RRResetPending = false;   // reset consumido

            // Streamline com state tracking manual pode trocar heaps. Passes seguintes definem
            // root signature e PSO proprios, mas dependem deste rebind dos descriptor heaps.
            {
                ID3D12DescriptorHeap* Heaps[] = { Backend->SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            }

            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Transition(Targets.VelocityBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (IsRR) GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            PostInput    = ActiveUp->OutputResource();
            PostInputSRV = ActiveUp->OutputSRVSlot();
            FrameState->TAARanLastFrame = false;
            RRSkipLogged    = false; // voltou a avaliar: rearma o aviso p/ a proxima vez
        } else {
            // Eval pulado por debug na cena: arma o reset p/ o proximo eval real nao reprojetar de
            // um historico interrompido (o RR ficou N frames sem ver a cena).
            if (RRPoisoned) {
                RRResetPending = true;
                if (!RRSkipLogged) {
                    LogWarning("Ray Reconstruction pausado: ha debug desenhado na cena "
                               "(visualizador de alvo ou sondas do DDGI). A imagem sai em resolucao "
                               "de render ate o debug sair — desligue-o p/ voltar a avaliar o RR");
                    RRSkipLogged = true;
                }
            }
            FrameState->TAARanLastFrame = false;
        }
        return { PostInput, PostInputSRV };
    }

    // Heatmap de flicker (opcional, troca a imagem), tonemap para o backbuffer, contorno da
    // selecao e a geometria de ferramenta do editor. Fecha o frame no backbuffer.
    void Renderer::RecordPost(FPassContext& _Ctx, FPostInput _In) {
        auto* CommandList     = _Ctx.Cmd;
        const FFrameView& Vw  = *_Ctx.View;
        const u32 FrameSlot   = _Ctx.FrameSlot;
        ID3D12Resource* PostInput = _In.Texture;
        u32 PostInputSRV          = _In.SRVSlot;
        const FSelectionDraw& Sel = _Ctx.Selection;

        if (FlickerMode > 0 && Flicker.IsInitialized()) {
            Flicker.Execute(CommandList, Backend->SRVHeap, PostInputSRV, static_cast<f32>(FlickerMode),
                            FlickerScale, FlickerAlpha, FlickerResetPending,
                            RenderWidth(), RenderHeight());
            FlickerResetPending = false;
            PostInput    = Flicker.OutputResource();
            PostInputSRV = Flicker.OutputSRVSlot();
        }

        FBarrierBatch BackBatch;
        BackBatch.Transition(Backend->SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        BackBatch.Flush(CommandList);

        {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Pós (bloom+tonemap)");
            PostProcessor.Execute(CommandList, Backend->SRVHeap, PostInput, Backend->SwapChain.CurrentRTV(),
                                  PostInputSRV, FrameSlot, Backend->SwapChain.GetWidth(), Backend->SwapChain.GetHeight());
        }

        // Captura: DEPOIS do tonemap e ANTES dos overlays. O contorno de selecao e os gizmos que
        // vem abaixo sao a ferramenta, nao a imagem — um PNG de regressao com a seta do gizmo
        // atravessada muda pixels que nao tem nada a ver com o estimador.
        if (Capture.ShouldShoot()) {
            Capture.RecordCopy(Backend->Device.Native(), CommandList, Backend->SwapChain.CurrentBackBuffer(),
                               Backend->SwapChain.GetWidth(), Backend->SwapChain.GetHeight());
            // Aqui tambem estamos depois do RecordResolve do cache, entao esta copia pega a
            // ocupacao que a varredura DESTE frame escreveu — a do anel, feita antes do dispatch,
            // ainda e do frame anterior.
            RadianceCache.RecordStatsCopy(CommandList);
        }

        if (Sel.Renderable >= 0 && Sel.Slot != FSelectionDraw::kNoSlot && Sel.Mesh
            && SelectionOutline.IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Contorno da seleção");
            FSelectionOutline::FDrawItem Item{ Sel.Mesh, Sel.Model * Vw.ViewProjUnjittered };
            SelectionOutline.RecordMask(CommandList, &Item, 1, FrameSlot);
            auto BackRTV = Backend->SwapChain.CurrentRTV();
            SelectionOutline.RecordOutline(CommandList, Backend->SRVHeap, BackRTV,
                                           Backend->SwapChain.GetWidth(), Backend->SwapChain.GetHeight(), FrameSlot);
        }

        if (DebugDraw.IsInitialized() && !DebugDraw.Empty()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Debug draw (gizmo/ícones)");
            const bool WantDepth = DebugDraw.NeedsSceneDepth() && Targets.DepthSRVSlot != kInvalidSlot;
            FBarrierBatch Batch;
            if (WantDepth) {
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
            }
            // Eixos da camera em mundo (colunas da view row-vector) p/ os billboards de icone.
            const Vec3 CamRight{ Vw.View.M[0][0], Vw.View.M[1][0], Vw.View.M[2][0] };
            const Vec3 CamUp   { Vw.View.M[0][1], Vw.View.M[1][1], Vw.View.M[2][1] };
            DebugDraw.Render(CommandList, FrameSlot, Vw.ViewProjUnjittered, Backend->SwapChain.CurrentRTV(),
                             Backend->SwapChain.GetWidth(), Backend->SwapChain.GetHeight(), CamRight, CamUp,
                             WantDepth ? Backend->SRVHeap.GpuHandle(Targets.DepthSRVSlot)
                                       : D3D12_GPU_DESCRIPTOR_HANDLE{});
            if (WantDepth) {
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);
            }
        }
        // (o Clear e da FDebugDrawClearGuard no topo do metodo)

        BackBatch.Transition(Backend->SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
        BackBatch.Flush(CommandList);
    }

    // Fog de altura + perspectiva aerea, sun shafts e a cortina/gotas de chuva. Tudo isto
    // compoe SOBRE o HDR ja iluminado, lendo o depth da cena — por isso vem depois do forward
    // e antes de qualquer resolve temporal.
    void Renderer::RecordVolumetricsAndRain(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FEffectiveIndirectPolicy& Policy = *_Ctx.Policy;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;

        if ((UseHeightFog || UseAerialPerspective) && Fog.IsInitialized()) {
            const D3D12_RESOURCE_STATES FogDepthRead =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, FogDepthRead);
            Batch.Flush(CommandList);

            const bool VolFogOn = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
            if (VolFogOn) {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Volumetric fog");
                SunShadows.EnsureReadableCompute(CommandList);
                LocalShadows.EnsureReadableCompute(CommandList);
                VolumetricFog.Execute(CommandList, Backend->SRVHeap, SunShadows.ConstantsAddress(),
                                      SunShadows.ShadowSRVSlot(),
                                      Policy.DDGIVolumetric ? DDGI.IrradianceAtlasSRV()
                                                            : Targets.DepthSRVSlot,
                                      LightBuffer->GetGPUVirtualAddress() +
                                          static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight),
                                      LocalShadows.ShadowSRVSlot(),
                                      VolumetricClouds.IsInitialized()
                                          ? VolumetricClouds.ShadowSRV() : Targets.DepthSRVSlot,
                                      Targets.DepthSRVSlot);
            }

            const bool VolShaftsOn = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
            if (VolShaftsOn) {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Sun shafts");
                SunShadows.EnsureReadable(CommandList);
                SunShafts.RecordVolumetric(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot,
                                           SunShadows.ConstantsAddress(),
                                           SunShadows.ShadowSRVSlot(),
                                           VolumetricClouds.IsInitialized()
                                               ? VolumetricClouds.ShadowSRV()
                                               : Targets.DepthSRVSlot);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            }

            Backend->DirectProfiler.Begin(CommandList, "Fog");
            auto Fog_RTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &Fog_RTV, FALSE, nullptr);
            Fog.Execute(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot, Atmosphere.AerialVolumeSRV(),
                        SunShafts.IsInitialized() ? SunShafts.VolumetricSRVSlot()
                                                  : Targets.DepthSRVSlot,
                        VolFogOn ? VolumetricFog.IntegratedSRVSlot() : Targets.DepthSRVSlot,
                        Atmosphere.IsInitialized() ? Atmosphere.SkyViewSRV() : Targets.DepthSRVSlot);
            Backend->DirectProfiler.End(CommandList); // Fog

            Batch.Transition(Targets.DepthBuffer.Get(), FogDepthRead,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (Weather.Raining() && RainWetness.IsInitialized() &&
            (Weather.GetCurtainAmount() > 0.001f || Weather.GetRainParticles())) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            Backend->DirectProfiler.Begin(CommandList, "Chuva — cortina/gotas");
            auto RainRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &RainRTV, FALSE, nullptr);
            if (Weather.GetCurtainAmount() > 0.001f)
                RainWetness.ExecuteCurtain(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot,
                                           RenderWidth(), RenderHeight());
            if (Weather.GetRainParticles())
                RainWetness.ExecuteParticles(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot,
                                             RenderWidth(), RenderHeight());
            Backend->DirectProfiler.End(CommandList); // Chuva — cortina/gotas

            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

    }

    // Iluminacao da cena: ReSTIR GI e o trace de reflexao, o denoise do indireto, ReSTIR DI com
    // o denoiser dedicado da direta, e o deferred lighting fullscreen que soma tudo sobre o
    // emissivo que o G-buffer ja deixou no HDR.
    void Renderer::RecordSceneLighting(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FEffectiveIndirectPolicy& Policy = *_Ctx.Policy;
        const FFrameView& Vw           = *_Ctx.View;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx; // sites que ja liam Ctx.FrameCB
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        if (Modes.ReSTIRGIActive) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "ReSTIR GI");
                ReSTIRGI.RecordTrace(CommandList, Backend->SRVHeap, &Backend->DirectProfiler);
            }

            if (Modes.NrdIndirectMode) {
                if (Modes.ReflectionsActive) {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "Reflexos (trace)");
                    Reflections.RecordTrace(CommandList, Backend->SRVHeap, &Backend->DirectProfiler);
                }
                Backend->DirectProfiler.Begin(CommandList, "NRD denoise");
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "Pack GI + reflexos");
                    Nrd.TransitionInputsToWrite(CommandList);
                    ReSTIRGI.RecordNrdPack(CommandList, Backend->SRVHeap);
                    if (Modes.ReflectionsActive) Reflections.RecordNrdPack(CommandList, Backend->SRVHeap);
                    else                   Reflections.RecordNrdSpecZero(CommandList, Backend->SRVHeap);
                }
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "RELAX indireto");
                    Nrd.SetFrame(Vw.ProjUnjittered, FrameState->NrdPrevProj, Vw.View,
                                 FrameState->NrdPrevView, Vw.JitterPx,
                                 FrameState->PrevJitterPx, FrameState->TemporalSampleIndex);
                    Nrd.Denoise(CommandList);
                }
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "Saida NRD indireta");
                    Nrd.TransitionOutputToRead(CommandList);
                }
                Backend->DirectProfiler.End(CommandList); // NRD denoise
                ID3D12DescriptorHeap* ReHeaps[] = { Backend->SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
            }

            // Velocity volta p/ PIXEL porque o TAA a le num passe grafico. Depth e G-buffer NAO
            // voltam p/ DEPTH_WRITE / RENDER_TARGET: ninguem escreve neles daqui ate o deferred
            // logo abaixo, que os quer em leitura combinada. O round-trip custava 2 barreiras por
            // MRT + 2 no depth por frame sem nenhum escritor no meio, e o DEPTH_WRITE ainda podia
            // disparar decompress/resummarize em alguns drivers.
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        {
            constexpr D3D12_RESOURCE_STATES DeferredReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            const bool ReSTIRDIOn = Modes.ReSTIRDIActiveFrame;
            // O G-buffer rastreia o proprio estado (AppendTransitions so recebe o alvo); o depth
            // nao, entao o estado de origem depende de o bloco do ReSTIR ter rodado — ele deixa o
            // depth em NON_PIXEL de proposito, em vez de devolver p/ DEPTH_WRITE.
            const D3D12_RESOURCE_STATES DepthBefore =
                Modes.ReSTIRGIActive ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                               : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, DeferredReadState);
            Batch.Transition(Targets.DepthBuffer.Get(), DepthBefore, DeferredReadState);
            // O PS deferred nao usa velocity, mas o Pass A do ReSTIR DI usa em compute. So amplie
            // o estado quando esse consumidor existir, evitando uma barreira no caminho legado.
            if (ReSTIRDIOn)
                Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState, DeferredReadState);
            Batch.Flush(CommandList);

            // ReSTIR DI roda ANTES do deferred. DeferredReadState inclui NON_PIXEL, exigido para
            // G-buffer/depth, e o alvo precisa estar pronto quando o PS o somar. Um unico modo
            // manda no dispatch e no gate do shader: divergir dobra ou apaga a luz.
            if (ReSTIRDIOn) {
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "ReSTIR DI");
                    ReSTIRDI.SetRayEpsilons(RayEps);
                    ReSTIRDI.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.View,
                                            FrameState->PrevViewProj,
                                            Vw.CameraPosition,
                                            RenderWidth(), RenderHeight(), FrameState->TemporalSampleIndex,
                                            FrameLightCount, kRTMaskShadowFull,
                                            /*EnableTemporalPermutation=*/Modes.RRMode,
                                            TemporalMotion.InstanceCount(),
                                            Modes.MotionHistoryValidThisFrame,
                                            MeshLights.LightCount());
                    ReSTIRDI.Record(CommandList, Backend->SRVHeap, &Backend->DirectProfiler);
                }

                if (Modes.NrdDirectMode) {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "NRD direta");
                    {
                        FGpuScope ChildScope(Backend->DirectProfiler, CommandList, "Pack direto");
                        NrdDirect.TransitionInputsToWrite(CommandList);
                        ReSTIRDI.RecordNrdPack(CommandList, Backend->SRVHeap);
                    }
                    {
                        FGpuScope ChildScope(Backend->DirectProfiler, CommandList, "RELAX direto");
                        NrdDirect.SetFrame(Vw.ProjUnjittered, FrameState->NrdPrevProj, Vw.View,
                                           FrameState->NrdPrevView, Vw.JitterPx,
                                           FrameState->PrevJitterPx,
                                           FrameState->TemporalSampleIndex);
                        NrdDirect.Denoise(CommandList);
                    }
                    {
                        FGpuScope ChildScope(Backend->DirectProfiler, CommandList, "Composite direto");
                        NrdDirect.TransitionOutputToRead(CommandList);
                        // O NRD liga seu heap privado; o composite pertence ao heap da engine.
                        ID3D12DescriptorHeap* ReHeaps[] = { Backend->SRVHeap.Native() };
                        CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
                        ReSTIRDI.RecordNrdComposite(CommandList, Backend->SRVHeap);
                    }
                }

                // Os consumidores posteriores historicamente partem de PIXEL (inclusive o bloco
                // de upscale, que usa barreira explicita). Nao deixe o estado combinado vazar.
                FBarrierBatch RestoreVelocity;
                RestoreVelocity.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                RestoreVelocity.Flush(CommandList);
            }
            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootDescriptorTable(2, Backend->SRVHeap.GpuHandle(GBuffer.SRVTableStart()));
            CommandList->SetGraphicsRootDescriptorTable(3, Backend->SRVHeap.GpuHandle(IBLTableStart));
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            {
                const u32 GITable = Policy.DDGISurface ? DDGI.SceneGITableStart()
                                                       : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(7, Backend->SRVHeap.GpuHandle(GITable));
            }
            {
                const u32 AOTable = AO.IsReady() ? AO.AOSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(8, Backend->SRVHeap.GpuHandle(AOTable));
            }
            {
                const u32 ReSTIRTable = Modes.ReSTIRGIActive ? ReSTIRGI.GITexSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(9, Backend->SRVHeap.GpuHandle(ReSTIRTable));
            }
            {
                // t20. Fallback no IBLTableStart quando inativo — descritor VALIDO, exatamente como
                // o t16 do ReSTIR e o t14 do AO fazem: a root sig exige a tabela ligada mesmo que o
                // shader nao a leia (LightParams.w = 0 fecha a leitura).
                const u32 DirectLocalTable = ReSTIRDIOn ? ReSTIRDI.OutputSRVSlot()
                                                        : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(12, Backend->SRVHeap.GpuHandle(DirectLocalTable));
            }
            {
                // t21. Mesmo padrao: descritor VALIDO sempre; AtmoLightParams.w fecha a leitura
                // quando a atmosfera esta off ou o caminho por pixel esta desligado.
                const u32 AtmoTable = Atmosphere.IsInitialized()
                    ? Atmosphere.TransmittanceSRV() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(13, Backend->SRVHeap.GpuHandle(AtmoTable));
            }
            CommandList->SetGraphicsRootShaderResourceView(
                10, LightBuffer->GetGPUVirtualAddress() +
                    static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            {
                const u32 LocalShadowTable = LocalShadows.IsInitialized()
                    ? LocalShadows.ShadowSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(11, Backend->SRVHeap.GpuHandle(LocalShadowTable));
            }
            Backend->DirectProfiler.Begin(CommandList, "Deferred lighting");
            // Aditivo (soma sobre o emissivo do geometry pass); nas views de debug SSAO/GI o
            // shader retorna a visualizacao inteira -> PSO opaco p/ substituir a tela.
            const bool DeferredDebugView = AODebug || (Policy.DDGISurface && GIDebug);
            CommandList->SetPipelineState(DeferredDebugView
                ? PipelineState.PSODeferredLightingDebug()
                : PipelineState.PSODeferredLighting());
            CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CommandList->IASetVertexBuffers(0, 0, nullptr);
            CommandList->IASetIndexBuffer(nullptr);
            CommandList->DrawInstanced(3, 1, 0, 0);
            Backend->DirectProfiler.End(CommandList);

            const u32 PointGIFlags =
                (GIChebyshev ? 1u : 0u) |
                (GISkipInactiveProbes ? 2u : 0u) |
                (GISkipInactiveFallback ? 4u : 0u);
            DDGIDebugPass.RecordPointDiagnostic(
                FrameSlot, CommandList, Backend->SRVHeap, DDGI,
                Vw.InvViewProjFull, Vw.CameraPosition,
                RenderWidth(), RenderHeight(), PointGIFlags);

            Batch.Transition(Targets.DepthBuffer.Get(), DeferredReadState,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
        }

        if (ObjectPicker.HasPendingRequest()) {
            {
                std::vector<FObjectPicker::FDrawItem> PickItems;
                PickItems.reserve(VisibleScratch.size());
                for (const FVisibleItem& V : VisibleScratch)
                    PickItems.push_back({ V.R->Mesh,
                                          ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants),
                                          V.SceneIndex + 1 });
                ObjectPicker.RecordIDPass(CommandList, DSV, PickItems.data(), PickItems.size(),
                                          RenderWidth(), RenderHeight());

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            }
        }

        // Visualizador ANTES do resolve: ele le a tabela como os traces a deixaram neste frame.
        // Depois do resolve mostraria o estado ja envelhecido/despejado, que nao e o que as
        // consultas do frame enxergaram.
        if (Modes.RadianceCacheDebugActive) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Radiance cache (debug)");

            // O deferred devolve depth para DEPTH_WRITE e a normal pode estar em RENDER_TARGET
            // quando GTAO esta off. O compute precisa dos dois em NON_PIXEL; restaure exatamente
            // os estados de entrada porque os passes seguintes partem desse contrato.
            const D3D12_RESOURCE_STATES NormalBefore = Targets.NormalBufferState;
            FBarrierBatch Inputs;
            Inputs.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Inputs.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Inputs.Flush(CommandList);

            RadianceCache.RecordDebug(CommandList, Backend->SRVHeap, Vw.InvViewProjFull,
                                      Vw.CameraPosition, FrameSlot);

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                      NormalBefore);
            Restore.Flush(CommandList);
        }

        // Resolve do world radiance cache: DEPOIS de todos os traces do frame, porque e aqui que
        // a acumulacao vira o valor que o proximo frame vai consultar. Rodar antes leria um
        // acumulador vazio e, pior, zeraria o que os traces acabaram de escrever.
        if (RadianceCache.IsActive(Modes)) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Radiance cache (resolve)");
            RadianceCache.RecordResolve(CommandList, Backend->SRVHeap);
        }
    }

    // Tudo que compoe SOBRE a cena ja iluminada e ainda le/escreve o HDR em resolucao de render:
    // composite das reflexoes, superficie da agua (com as copias de cor/depth e a cadeia de mips
    // antes), translucidos em forward blend e as nuvens volumetricas.
    void Renderer::RecordForwardAndClouds(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FEffectiveIndirectPolicy& Policy = *_Ctx.Policy;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        if (Modes.ReflectionsActive) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

            const D3D12_RESOURCE_STATES ReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
            GBuffer.AppendTransitions(Batch, ReadState);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Reflexos (composite)");
                if (!Modes.NrdIndirectMode) Reflections.RecordTrace(CommandList, Backend->SRVHeap);
                // RR: extrai o hitDist especular do Resolved ENQUANTO ele esta NON_PIXEL (o composite
                // cru abaixo o transiciona p/ PIXEL). O RecordTrace acima parou no Resolved (RawSpec).
                if (Modes.RRMode)
                    RRGuides.RecordSpecHitDist(CommandList, Backend->SRVHeap,
                                               Backend->SRVHeap.GpuHandle(Reflections.GetResolvedSRVSlot()),
                                               Ctx.FrameCB);
                Reflections.RecordComposite(CommandList, Backend->SRVHeap, Targets.HDRRTVHeap.CpuHandle(0),
                                            RenderWidth(), RenderHeight());
            }

            Batch.Transition(Targets.DepthBuffer.Get(), ReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        if (UseWater && Water.IsInitialized() && Modes.WaterSceneCopiesReady) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[0],
                                    D3D12_RESOURCE_STATE_COPY_DEST, 0);
            Batch.TransitionTracked(Targets.SceneDepthCopy.Get(), Targets.SceneDepthCopyState,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
            Batch.Flush(CommandList);

            D3D12_TEXTURE_COPY_LOCATION ColorDst{};
            ColorDst.pResource        = Targets.SceneColorCopy.Get();
            ColorDst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ColorDst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ColorSrc{};
            ColorSrc.pResource        = Targets.HDRColorBuffer.Get();
            ColorSrc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ColorSrc.SubresourceIndex = 0;
            CommandList->CopyTextureRegion(&ColorDst, 0, 0, 0, &ColorSrc, nullptr);
            CommandList->CopyResource(Targets.SceneDepthCopy.Get(), Targets.DepthBuffer.Get());

            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            constexpr D3D12_RESOURCE_STATES SceneCopyRead =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[0],
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
            Batch.TransitionTracked(Targets.SceneDepthCopy.Get(), Targets.SceneDepthCopyState, SceneCopyRead);
            Batch.Flush(CommandList);

            if (Targets.SceneColorMipCount > 1) {
                Targets.SceneColorMipPSO.Bind(CommandList);
                const u32 Constants[8] = {
                    RenderWidth(), RenderHeight(), 0u, 0u, 0u, 0u, 0u, 0u };
                CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

                for (u32 Mip = 1; Mip < Targets.SceneColorMipCount; ++Mip) {
                    Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, Mip);
                    Batch.Flush(CommandList);

                    CommandList->SetComputeRootDescriptorTable(
                        1, Backend->SRVHeap.GpuHandle(Targets.SceneColorMipSRVStart + Mip - 1));
                    CommandList->SetComputeRootDescriptorTable(
                        2, Backend->SRVHeap.GpuHandle(Targets.SceneColorMipUAVStart + Mip));
                    const u32 MipWidth  = std::max(1u, RenderWidth() >> Mip);
                    const u32 MipHeight = std::max(1u, RenderHeight() >> Mip);
                    CommandList->Dispatch((MipWidth + 7u) / 8u, (MipHeight + 7u) / 8u, 1);

                    Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Mip);
                    Batch.Flush(CommandList);
                }
            }

            // O water base usa a mesma copia para refracao no PS, enquanto o trace de reflexo
            // usa a piramide no compute. Encerrar todos os subrecursos no estado combinado evita
            // uma transicao do recurso inteiro sobre estados divergentes.
            for (u32 Mip = 0; Mip < Targets.SceneColorMipCount; ++Mip)
                Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                        SceneCopyRead, Mip);
            Batch.Flush(CommandList);

            auto HDR_RTV_Rebind = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV_Rebind, FALSE, &DSV);
        }

        if (UseWater && Water.IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Água — superfície");

            FBarrierBatch WaterBatch;
            WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
            WaterBatch.Flush(CommandList);
            const bool WaterReflectionsActive = Modes.DedicatedWaterReflections;
            const bool ForceFullWaterOutputs = Modes.RRMode || Water.GetGuideInvisible() ||
                Water.GetDebugMode() == FWaterRenderer::EDebugMode::Wireframe;
            FWaterRenderer::EOutputMode WaterOutputMode;
            if (Modes.RRMode) {
                WaterOutputMode = FWaterRenderer::EOutputMode::RayReconstruction;
            } else if (WaterReflectionsActive) {
                WaterOutputMode = FWaterRenderer::EOutputMode::ReflectionGuides;
            } else {
                WaterOutputMode = ForceFullWaterOutputs
                    ? FWaterRenderer::EOutputMode::RayReconstruction
                    : (Modes.UpscaleActive ? FWaterRenderer::EOutputMode::TemporalMasks
                                     : FWaterRenderer::EOutputMode::Base);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE WaterRTVs[8] = {
                Targets.HDRRTVHeap.CpuHandle(0), Targets.VelocityRTVHeap.CpuHandle(0) };
            u32 WaterRTVCount = 2;
            if (WaterOutputMode == FWaterRenderer::EOutputMode::TemporalMasks) {
                WaterRTVs[2] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[3] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVCount = 4;
            } else if (WaterOutputMode == FWaterRenderer::EOutputMode::RayReconstruction) {
                RRGuides.PrepareSpecHitForWater(CommandList);
                WaterRTVs[2] = GBuffer.RTVHandle(0);
                WaterRTVs[3] = GBuffer.RTVHandle(1);
                WaterRTVs[4] = GBuffer.RTVHandle(2);
                WaterRTVs[5] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[6] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVs[7] = RRGuides.SpecHitRTV();
                WaterRTVCount = 8;
            } else if (WaterOutputMode == FWaterRenderer::EOutputMode::ReflectionGuides) {
                WaterRTVs[2] = GBuffer.RTVHandle(0);
                WaterRTVs[3] = GBuffer.RTVHandle(1);
                WaterRTVs[4] = GBuffer.RTVHandle(2);
                WaterRTVs[5] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[6] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVCount = 7;
            }
            CommandList->OMSetRenderTargets(WaterRTVCount, WaterRTVs, FALSE, &DSV);

            const u32 WaterReflCube =
                (UseAtmosphereSky && Atmosphere.IsInitialized())
                    ? Atmosphere.SkyReflectionSRV()
                    : HDREnv.SpecularSRV();
            const u32 OceanDispSlots[kOceanCascades] = {
                Ocean[0].SRVSlot(), Ocean[1].SRVSlot(), Ocean[2].SRVSlot() };
            const u32 OceanPreviousDispSlots[kOceanCascades] = {
                Ocean[0].PreviousSRVSlot(), Ocean[1].PreviousSRVSlot(),
                Ocean[2].PreviousSRVSlot() };
            const u32 OceanNormalSlots[kOceanCascades] = {
                Ocean[0].NormalSRVSlot(), Ocean[1].NormalSRVSlot(), Ocean[2].NormalSRVSlot() };
            Water.RenderSurface(CommandList, Backend->SRVHeap, WaterReflCube, OceanDispSlots,
                                OceanPreviousDispSlots, OceanNormalSlots, Targets.SceneCopyTableStart,
                                SunShadows.ConstantsAddress(),
                                SunShadows.ShadowSRVSlot(), WaterOutputMode);

            if (WaterReflectionsActive) {
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                WaterBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                GBuffer.AppendTransitions(WaterBatch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);

                Reflections.RecordWaterTrace(CommandList, Backend->SRVHeap, &Backend->DirectProfiler);
                if (Modes.RRMode) {
                    RRGuides.RecordWaterSpecHitDist(
                        CommandList, Backend->SRVHeap,
                        Backend->SRVHeap.GpuHandle(Reflections.GetWaterSpecHitTable()),
                        Ctx.FrameCB);
                }
                Reflections.RecordWaterComposite(CommandList, Backend->SRVHeap, Targets.HDRRTVHeap.CpuHandle(0),
                                                 RenderWidth(), RenderHeight());

                WaterBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_DEPTH_WRITE);
                GBuffer.AppendTransitions(WaterBatch, D3D12_RESOURCE_STATE_RENDER_TARGET);
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);
            } else {
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);
            }
            auto PostWaterRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &PostWaterRTV, FALSE, &DSV);
        }

        // Transparencias foreground sao compostas depois da agua: nao contaminam a copia usada
        // pela refracao e passam a testar contra a profundidade real da superficie.
        {
            bool AnyBlend = false;
            for (const FVisibleItem& V : VisibleScratch)
                if (V.Mat->Blend) { AnyBlend = true; break; }
            if (AnyBlend) {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Translúcidos");
                D3D12_CPU_DESCRIPTOR_HANDLE BlendRTVs[3] = {
                    Targets.HDRRTVHeap.CpuHandle(0), Targets.UpscaleMaskRTVHeap.CpuHandle(0),
                    Targets.UpscaleMaskRTVHeap.CpuHandle(1) };
                CommandList->OMSetRenderTargets(_countof(BlendRTVs), BlendRTVs, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootDescriptorTable(3, Backend->SRVHeap.GpuHandle(IBLTableStart));
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(
                    6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
                {
                    // Translucidos: ambiente difuso do ForwardBlend, que nao recebe a textura do
                    // ReSTIR GI — por isso superficie, e nao volumetrico.
                    const u32 GITable = Policy.DDGISurface
                        ? DDGI.SceneGITableStart() : IBLTableStart;
                    CommandList->SetGraphicsRootDescriptorTable(7, Backend->SRVHeap.GpuHandle(GITable));
                }
                {
                    const u32 AtmoTable = Atmosphere.IsInitialized()
                        ? Atmosphere.TransmittanceSRV() : IBLTableStart;
                    CommandList->SetGraphicsRootDescriptorTable(13, Backend->SRVHeap.GpuHandle(AtmoTable));
                }
                CommandList->SetPipelineState(PipelineState.PSOForwardBlend());
                CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                FDrawSubmitCache Submit;
                for (auto It = VisibleScratch.rbegin(); It != VisibleScratch.rend(); ++It) {
                    if (!It->Mat->Blend) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(It->Slot) * sizeof(ObjectConstants));
                    Submit.BindMaterial(CommandList, Backend->SRVHeap, It->Mat);
                    Submit.DrawMesh(CommandList, It->R->Mesh);
                }
            }
        }

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Nuvens");

            const D3D12_RESOURCE_STATES CloudReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, CloudReadState);
            Batch.Flush(CommandList);

            VolumetricClouds.RecordRaymarch(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot);

            auto CloudRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &CloudRTV, FALSE, nullptr); // sem DSV: depth e SRV
            VolumetricClouds.Composite(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot);

            Batch.Transition(Targets.DepthBuffer.Get(), CloudReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }
    }

    // CSM do sol (com o cache de casters estaticos) e as sombras locais de spot/point.
    void Renderer::RecordShadows(FPassContext& _Ctx, const FLocalShadowJobs& _Jobs) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& AllItems           = _Ctx.All;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;
        const auto& LocalShadowJobs    = _Jobs.Spot;
        const auto& LocalCubeJobs      = _Jobs.Cube;

        {
            const f32 ShadowNoiseFrame = (Modes.TAAActive || Modes.UpscaleActive)
                ? static_cast<f32>(FrameState->TemporalSampleIndex % 64u) : 0.0f;
            SunShadows.UpdatePerFrame(FrameSlot, UseSunShadows, Vw.View, Vw.CameraPosition,
                                      Vw.FovY, Vw.Aspect, Lt.KeyDir, Vw.NearZ, ShadowNoiseFrame,
                                      SceneState->Scene.StaticCastersVersion());
            if (UseSunShadows) {
                std::vector<FSunShadows::FShadowDrawItem> Casters;
                Casters.reserve(AllItems.size());
                for (const FDrawItem& A : AllItems) {
                    // Translucido nao projeta sombra opaca (vidro deixa o sol entrar).
                    if (A.Mat && A.Mat->Blend) continue;
                    // Durante o arraste, trate o objeto como dinamico para preservar o cache CSM.
                    const bool Dyn = A.R->Mobility == EMobility::Dynamic ||
                                     (SceneState->DraggingRenderableId != 0 &&
                                      A.R->Id == SceneState->DraggingRenderableId);
                    Casters.push_back({ A.R->Mesh, A.Mat,
                                        ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                        A.R->AABBMin, A.R->AABBMax, Dyn });
                }
                // Mobilidade separa as metades cacheada/dinamica; material reduz trocas de PSO.
                std::sort(Casters.begin(), Casters.end(),
                          [](const FSunShadows::FShadowDrawItem& a,
                             const FSunShadows::FShadowDrawItem& b) {
                              if (a.Dynamic != b.Dynamic) return !a.Dynamic;
                              const bool am = a.Mat && a.Mat->Constants.AlphaTest != 0;
                              const bool bm = b.Mat && b.Mat->Constants.AlphaTest != 0;
                              if (am != bm) return !am;
                              if (a.Mat != b.Mat) return a.Mat < b.Mat;
                              return a.Mesh < b.Mesh;
                          });
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "Sombras — sol (CSM)");
                    FSunShadows::FExtraCascadeDraw TerrainCasters;
                    if (UseTerrain && Terrain.IsLoaded())
                        TerrainCasters = [this](ID3D12GraphicsCommandList* Cmd, u32,
                                                D3D12_GPU_VIRTUAL_ADDRESS CascadeCB,
                                                const Mat44& CascadeVP) {
                            Terrain.RenderShadowCascade(Cmd, Backend->SRVHeap, CascadeCB, CascadeVP);
                        };
                    SunShadows.RecordDepthPass(CommandList, Backend->SRVHeap, Casters.data(), Casters.size(),
                                               TerrainCasters, &Backend->DirectProfiler);
                }

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
            } else {
                SunShadows.EnsureReadable(CommandList);
            }

            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        if (!LocalShadowJobs.empty() || !LocalCubeJobs.empty()) {
            std::vector<FLocalShadows::FShadowDrawItem> LocalCasters;
            LocalCasters.reserve(AllItems.size());
            for (const FDrawItem& A : AllItems) {
                if (A.Mat && A.Mat->Blend) continue; // vidro nao projeta sombra opaca
                LocalCasters.push_back({ A.R->Mesh, A.Mat,
                                         ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                         A.R->AABBMin, A.R->AABBMax });
            }
            // Mesma chave do CSM (sem mobilidade): alpha-test agrupado e Bind/IA
            // adjacentes. A broad-phase por luz nao depende da ordem.
            std::sort(LocalCasters.begin(), LocalCasters.end(),
                      [](const FLocalShadows::FShadowDrawItem& a,
                         const FLocalShadows::FShadowDrawItem& b) {
                          const bool am = a.Mat && a.Mat->Constants.AlphaTest != 0;
                          const bool bm = b.Mat && b.Mat->Constants.AlphaTest != 0;
                          if (am != bm) return !am;
                          if (a.Mat != b.Mat) return a.Mat < b.Mat;
                          return a.Mesh < b.Mesh;
                      });
            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Sombras — locais");
                // Terreno tambem projeta nas luzes locais. Sem isto o terreno era iluminado
                // por point/spot (escreve G-buffer) mas nao aparecia nos shadow maps delas:
                // poste atravessava a colina, e a volumetrica — que le os MESMOS t18/t19 —
                // mostrava o god ray passando pelo terreno.
                FLocalShadows::FExtraLocalDraw TerrainCasters;
                if (UseTerrain && Terrain.IsLoaded())
                    TerrainCasters = [this](ID3D12GraphicsCommandList* Cmd,
                                            D3D12_GPU_VIRTUAL_ADDRESS SliceCB,
                                            const Mat44& SliceVP,
                                            const Vec3& LightPos, f32 Radius) {
                        // Perspectiva com depth clip (o near corta de verdade, ao contrario da
                        // cascata ortho do sol) + a esfera da luz, o mesmo filtro que os meshes
                        // recebem na broad phase.
                        Terrain.RenderShadowCascade(Cmd, Backend->SRVHeap, SliceCB, SliceVP, true,
                                                    LightPos, Radius);
                    };
                LocalShadows.RecordDepthPass(CommandList, Backend->SRVHeap, FrameSlot,
                                             LocalCasters.data(), LocalCasters.size(),
                                             LocalShadowJobs.data(),
                                             static_cast<u32>(LocalShadowJobs.size()),
                                             LocalCubeJobs.data(),
                                             static_cast<u32>(LocalCubeJobs.size()),
                                             TerrainCasters);
            }

            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        } else if (LocalShadows.IsInitialized()) {
            LocalShadows.EnsureReadable(CommandList);
        }
    }

    // Z-prepass (opacos, mascarados, terreno), build+teste do HZB e o GTAO. Tudo consome so
    // profundidade e a normal geometrica — o G-buffer ainda nem foi escrito.
    void Renderer::RecordDepthPrepass(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const auto& Ctx                = _Ctx;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        const bool DoPrepass = true;
        if (DoPrepass) {
            Backend->DirectProfiler.Begin(CommandList, "Z-prepass");
            if (Modes.GeometricNormalWillRun) {
                FBarrierBatch Batch;
                Batch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                Batch.Flush(CommandList);
                auto NormalRTV = Targets.NormalRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &NormalRTV, FALSE, &DSV);
                const FLOAT NeutralN[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
                CommandList->ClearRenderTargetView(NormalRTV, NeutralN, 0, nullptr);
                CommandList->SetPipelineState(PipelineState.PSODepthNormal());
            } else {
                CommandList->SetPipelineState(PipelineState.PSODepthOnly());
            }
            CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Z - opacos");
                FDrawSubmitCache Submit;
                for (const FVisibleItem& V : VisibleScratch) {
                    if (V.Mat->TwoSided || V.Mat->Constants.AlphaTest || V.Mat->Blend) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                    Submit.DrawMesh(CommandList, V.R->Mesh);
                }
            }

            CommandList->SetPipelineState(Modes.GeometricNormalWillRun
                                              ? PipelineState.PSODepthNormalMasked()
                                              : PipelineState.PSODepthOnlyMasked());
            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Z - mascarados");
                FDrawSubmitCache Submit;
                for (const FVisibleItem& V : VisibleScratch) {
                    if (V.Mat->Blend) continue;
                    if (!V.Mat->TwoSided && !V.Mat->Constants.AlphaTest) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                    Submit.BindMaterial(CommandList, Backend->SRVHeap, V.Mat);
                    Submit.DrawMesh(CommandList, V.R->Mesh);
                }
            }

            if (UseTerrain && Terrain.IsLoaded()) {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "Z - terreno");
                Terrain.RenderDepthPrepass(CommandList, Backend->SRVHeap, Modes.GeometricNormalWillRun);
            }
            Backend->DirectProfiler.End(CommandList); // Z-prepass
        }

        // HZB do depth do prepass (min-reduce reverse-Z) + teste dos AABBs com a VP
        // sem jitter DESTE frame; o resultado volta pela readback ring e e consumido
        // kFramesInFlight frames depois no filtro do VisibleScratch (acima).
        if (HiZ.IsReady() && UseOcclusionCulling) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "HZB");
                HiZ.RecordBuild(CommandList, Backend->SRVHeap, Targets.DepthSRVSlot);
                HiZ.RecordTest(CommandList, Backend->SRVHeap, FrameSlot,
                               static_cast<u32>(SceneState->Scene.Renderables().size()),
                               Vw.ViewProjUnjittered);
            }
            Batch.Transition(Targets.DepthBuffer.Get(),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (AO.IsReady()) {
            if (Modes.AOWillRun) {
                CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

                const f32 TanHalf = std::tan(0.5f * Vw.FovY);
                const f32 M11 = 1.0f / TanHalf;
                const f32 M00 = M11 / Vw.Aspect;
                const f32 M22 = Vw.Projection.M[2][2];
                const f32 M32 = Vw.Projection.M[3][2];
                AO.UpdatePerFrame(FrameSlot, M00, M11, M22, M32, Vw.View,
                                  RenderWidth(), RenderHeight(), FrameState->TemporalSampleIndex);

                FBarrierBatch Batch;
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
                {
                    FGpuScope Scope(Backend->DirectProfiler, CommandList, "GTAO");
                    AO.Execute(CommandList, Backend->SRVHeap);
                }

                Batch.Transition(Targets.DepthBuffer.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, Backend->SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            } else {
                AO.ClearToWhite(CommandList, Backend->SRVHeap);
            }
        }
    }

    // Geometry pass (G-buffer + velocity + emissivo no HDR), velocity do background,
    // wetness da chuva sobre o G-buffer e o vetor de movimento temporal confiavel.
    void Renderer::RecordGBuffer(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const FFrameAmbient& Amb       = *_Ctx.Ambient; // wetness da chuva integra o ambiente
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& AllItems           = _Ctx.All;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        {
            Backend->DirectProfiler.Begin(CommandList, "G-buffer (geometria)");
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
            // 5o MRT = SceneColor HDR: o emissivo e escrito direto nele (dieta do G-buffer).
            // O ceu ja esta la (desenhado antes do prepass); a geometria opaca sobrescreve so
            // os proprios pixels e o deferred lighting depois SOMA a luz (blend aditivo).
            D3D12_CPU_DESCRIPTOR_HANDLE GBufRTVs[FGBuffer::kTargetCount + 2] = {
                GBuffer.RTVHandle(0), GBuffer.RTVHandle(1), GBuffer.RTVHandle(2),
                Targets.VelocityRTVHeap.CpuHandle(0), Targets.HDRRTVHeap.CpuHandle(0) };
            CommandList->OMSetRenderTargets(FGBuffer::kTargetCount + 2, GBufRTVs, FALSE, &DSV);
            GBuffer.Clear(CommandList);
            const FLOAT VelClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            CommandList->ClearRenderTargetView(Targets.VelocityRTVHeap.CpuHandle(0), VelClear, 0, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);

            {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "G-buffer - meshes");
                // Front-to-back serve o Z-prepass (Hi-Z). Depois do EQUAL, ordem de
                // profundidade nao reduz overdraw — so espalha PSO/material/IA. Agrupar
                // como o CSM ja faz: PSO (two-sided) -> material -> mesh.
                std::vector<const FVisibleItem*> GBufferOrder;
                GBufferOrder.reserve(VisibleScratch.size());
                for (const FVisibleItem& V : VisibleScratch) {
                    if (!V.Mat->Blend) GBufferOrder.push_back(&V);
                }
                std::sort(GBufferOrder.begin(), GBufferOrder.end(),
                          [](const FVisibleItem* a, const FVisibleItem* b) {
                              const bool at = a->Mat->IsTwoSidedForRT();
                              const bool bt = b->Mat->IsTwoSidedForRT();
                              if (at != bt) return !at;
                              if (a->Mat != b->Mat) return a->Mat < b->Mat;
                              return a->R->Mesh < b->R->Mesh;
                          });

                CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D12PipelineState* CurGeomPSO = nullptr;
                FDrawSubmitCache Submit;
                for (const FVisibleItem* V : GBufferOrder) {
                    FMaterial* Mat = V->Mat;
                    const bool TwoSided = Mat->IsTwoSidedForRT();
                    ID3D12PipelineState* Want = TwoSided ? PipelineState.PSOGBufferTwoSided()
                                                         : PipelineState.PSOGBuffer();
                    if (Want != CurGeomPSO) { CommandList->SetPipelineState(Want); CurGeomPSO = Want; }
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V->Slot) * sizeof(ObjectConstants));
                    Submit.BindMaterial(CommandList, Backend->SRVHeap, Mat);
                    Submit.DrawMesh(CommandList, V->R->Mesh);
                }
            }

            if (UseTerrain && Terrain.IsLoaded()) {
                FGpuScope Scope(Backend->DirectProfiler, CommandList, "G-buffer - terreno");
                Terrain.RenderGBuffer(CommandList, Backend->SRVHeap);
            }
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            Backend->DirectProfiler.End(CommandList); // G-buffer (geometria)
        }

        // Motion vector do BACKGROUND: o G-buffer so escreveu velocity p/ geometria+terreno; ceu/nuvens/fog
        // ficaram ZERO (clear acima). Preenche o velocity do ceu (reproj rotacao-only, sem parallax) p/ o
        // DLSS/RR/TAA nao arrastar o historico do ceu ao girar a camera. So roda com consumidor temporal.
        if ((Modes.UpscaleActive || Modes.TAAActive) && BgVelocity.IsReady()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Velocity do background");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Batch.Flush(CommandList);

            BgVelocity.Record(CommandList, FrameSlot, Backend->SRVHeap.GpuHandle(Targets.DepthSRVSlot),
                              Backend->SRVHeap.GpuHandle(Targets.VelocityUavSlot), Vw.SkyClipToPrevClip,
                              RenderWidth(), RenderHeight());

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Restore.Flush(CommandList);
        }

        if (Weather.Active() && RainWetness.IsInitialized()) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Chuva — wetness");
            if (Weather.GetRainOcclusion()) {
                std::vector<FRainWetness::FOccluderItem> RainOccluders;
                RainOccluders.reserve(AllItems.size());
                for (const FDrawItem& A : AllItems)
                    RainOccluders.push_back({ A.R->Mesh, A.Mat,
                                              ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                              A.R->AABBMin, A.R->AABBMax });
                RainWetness.RecordOcclusionMap(CommandList, Backend->SRVHeap, FrameSlot, Vw.CameraPosition,
                                               RainOccluders.data(), RainOccluders.size());
            }
            const Vec3 KeyColorInt = { Lt.KeyColor.X * Lt.KeyInt, Lt.KeyColor.Y * Lt.KeyInt,
                                       Lt.KeyColor.Z * Lt.KeyInt };
            RainWetness.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.ViewProjection,
                                       Vw.CameraPosition, FrameState->ElapsedTime, Weather, Lt.KeyDir,
                                       KeyColorInt, Amb.Sky);
            RainWetness.Execute(CommandList, Backend->SRVHeap, GBuffer, Targets.DepthBuffer.Get(), Targets.DepthSRVSlot,
                                RenderWidth(), RenderHeight());
        }

        if (Modes.ReliableMotionActive) {
            FGpuScope Scope(Backend->DirectProfiler, CommandList, "Motion temporal confiavel");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            TemporalMotion.Record(CommandList, Backend->SRVHeap, &Backend->DirectProfiler);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }
    }

    // Empacota as luzes puntuais da DIRETA (FGPULight, root SRV t17 do deferred): cull pelo
    // frustum, prioriza quem ganha slot de sombra e monta as matrizes. Devolve os jobs de sombra
    // local para o passe de sombras — eles saem daqui porque e aqui que o cull ja aconteceu.
    FLocalShadowJobs Renderer::PackDirectLights(FPassContext& _Ctx, FrameConstants* MappedCB) {
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const u32 FrameSlot            = _Ctx.FrameSlot;

        const Vec4* Planes = Vw.FrustumPlanes; // resolvido no ResolveFrameView

        FLocalShadowJobs ShadowJobs;
        auto& LocalShadowJobs = ShadowJobs.Spot;
        auto& LocalCubeJobs   = ShadowJobs.Cube;
        {
            Vec4 NPlanes[6];
            for (int i = 0; i < 6; ++i) {
                const Vec4& p   = Planes[i];
                const f32   len = std::sqrt(p.X*p.X + p.Y*p.Y + p.Z*p.Z);
                const f32   inv = len > 1e-6f ? 1.0f / len : 0.0f;
                NPlanes[i] = { p.X*inv, p.Y*inv, p.Z*inv, p.W*inv };
            }

            FGPULight* DstLights = reinterpret_cast<FGPULight*>(
                MappedLightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            u32 NumLights = 0;
            u64 LightSetSignature = 1469598103934665603ull; // FNV-1a sobre IDs na ordem do buffer

            struct ShadowCand { u32 Gpu; u32 LightIdx; u64 Id; f32 Key; };
            std::vector<ShadowCand> ShadowCands;
            std::vector<ShadowCand> CubeCands;

            // Prioridade combina luminancia e fracao angular, sem singularidade dentro do raio.
            auto ShadowScore = [&](const FLight& L) -> f32 {
                const f32 Energy = L.Intensity * (L.Color.X * 0.2126f + L.Color.Y * 0.7152f +
                                                  L.Color.Z * 0.0722f);
                const Vec3 ToCam = L.Position - Vw.CameraPosition;
                const f32 R2 = L.AttenuationRadius * L.AttenuationRadius;
                return Energy * R2 / (ToCam.LengthSq() + R2);
            };

            auto& SceneLights = SceneState->Scene.Lights();
            for (u32 li = 0; li < static_cast<u32>(SceneLights.size()); ++li) {
                FLight& L = SceneLights[li];
                // Identidade estavel na primeira vez que vemos a luz. O editor faz push_back
                // direto em Lights(), entao a atribuicao mora aqui e nao no AddLight.
                if (L.Id == 0) L.Id = SceneState->Scene.AllocObjectId();
                Vec3 PreviousLightPos = L.Position;
                if (const auto It = FrameState->PreviousDirectLightPositions.find(L.Id);
                    It != FrameState->PreviousDirectLightPositions.end())
                    PreviousLightPos = It->second;
                FrameState->PreviousDirectLightPositions[L.Id] = L.Position;
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                if (NumLights >= kMaxLights) break;

                bool Outside = false;
                for (int i = 0; i < 6 && !Outside; ++i) {
                    const Vec4& p = NPlanes[i];
                    Outside = (p.X*L.Position.X + p.Y*L.Position.Y + p.Z*L.Position.Z + p.W)
                              < -L.AttenuationRadius;
                }
                if (Outside) continue;

                FGPULight G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity, L.Color.Y * L.Intensity,
                                        L.Color.Z * L.Intensity,
                                        std::max(L.SourceRadius, 0.01f) };
                G.ShadowMatrix      = Mat44::Identity();
                G.PrevPosInvRadius  = { PreviousLightPos.X, PreviousLightPos.Y,
                                        PreviousLightPos.Z, 1.0f / L.AttenuationRadius };
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg  = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg  = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter  = std::cos(OuterDeg * ToRad);
                    const f32 CosInner  = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    // w preserva a intencao do artista independentemente do orcamento de slices.
                    // O raster usa y/z; o ReSTIR DI usa w para decidir se traca shadow ray.
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      -1.0f, 0.0f, L.CastShadows ? 1.0f : 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        // Histerese: quem ja tem o slot vale mais no ranking, entao o novato
                        // precisa ser sensivelmente melhor pra tomar. Sem isso duas luzes de
                        // importancia parecida trocam de slice todo frame.
                        const f32 Bias = LocalShadows.SpotOwns(L.Id)
                                       ? FLocalShadows::kHysteresis : 1.0f;
                        ShadowCands.push_back({ NumLights, li, L.Id, ShadowScore(L) * Bias });
                    }
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f }; // -2 = sem mascara de cone
                    G.SpotParams  = { 0.0f, -1.0f, 0.0f, L.CastShadows ? 1.0f : 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        const f32 Bias = LocalShadows.CubeOwns(L.Id)
                                       ? FLocalShadows::kHysteresis : 1.0f;
                        CubeCands.push_back({ NumLights, li, L.Id, ShadowScore(L) * Bias });
                    }
                }
                DstLights[NumLights++] = G;
                LightSetSignature ^= L.Id;
                LightSetSignature *= 1099511628211ull;
            }

            // Matrizes do slice de spot. Usadas por todo mundo que emite job — inclusive quem
            // esta em fade-out, que segue sendo redesenhado da pose atual.
            auto SpotShadowVP = [&](const FLight& L, Mat44& OutLVP, f32& OutFar) {
                const Vec3 D  = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                const Vec3 Up = std::fabs(D.Y) > 0.99f ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                       : Vec3{ 0.0f, 1.0f, 0.0f };
                const f32 OuterRad = std::clamp(L.OuterConeDeg, 1.0f, 89.0f) * ToRad;
                const f32 NearP    = FLocalShadows::kPointNear;
                OutFar = std::max(L.AttenuationRadius, NearP * 2.0f);
                OutLVP = Mat44::LookAtLH(L.Position, L.Position + D, Up) *
                         Mat44::PerspectiveFovLH(2.0f * OuterRad, 1.0f, NearP, OutFar);
            };
            auto ToShadowUV = [](const Mat44& LVP) {
                Mat44 BiasUV = Mat44::Identity();
                BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
                BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;
                return LVP * BiasUV;
            };

            // Id como desempate (mesma ideia do hash do ID no SortLights do Flax): std::sort nao
            // e estavel, entao scores empatados poderiam trocar de ordem entre frames e mudar
            // quem fica com o slot livre.
            auto ByScore = [](const ShadowCand& a, const ShadowCand& b) {
                return a.Key != b.Key ? a.Key > b.Key : a.Id < b.Id;
            };
            std::sort(ShadowCands.begin(), ShadowCands.end(), ByScore);
            const u32 NumShadowed = std::min<u32>(static_cast<u32>(ShadowCands.size()),
                                                  FLocalShadows::kActiveShadows);
            // Slots seguem a identidade da luz e sao adquiridos em lote para evitar remapeamento.
            u64 SpotIds[FLocalShadows::kActiveShadows];
            u32 SpotSlot[FLocalShadows::kActiveShadows];
            for (u32 s = 0; s < NumShadowed; ++s)
                SpotIds[s] = SceneLights[ShadowCands[s].LightIdx].Id;
            LocalShadows.AcquireSpotSlots(SpotIds, NumShadowed, SpotSlot);
            LocalShadows.UpdateSpotFades(SpotIds, NumShadowed, FrameState->LastDeltaTime);

            for (u32 s = 0; s < NumShadowed; ++s) {
                const u32 Slice = SpotSlot[s];
                if (Slice == FLocalShadows::kNoSlot) continue;

                const FLight& L = SceneLights[ShadowCands[s].LightIdx];
                Mat44 LVP; f32 FarP;
                SpotShadowVP(L, LVP, FarP);

                FGPULight& G   = DstLights[ShadowCands[s].Gpu];
                G.ShadowMatrix = ToShadowUV(LVP);
                G.SpotParams.Y = static_cast<f32>(Slice);
                G.SpotParams.Z = LocalShadows.SpotFadeAt(Slice);
                LocalShadowJobs.push_back({ LVP, L.Position, FarP, Slice });
            }

            // Slices em fade-out continuam sendo redesenhados enquanto o slot existir.
            for (u32 i = 0; i < FLocalShadows::kMaxShadows; ++i) {
                const u64 Owner = LocalShadows.SpotOwnerAt(i);
                if (Owner == 0 || LocalShadows.SpotFadeAt(i) <= 0.0f) continue;
                bool Active = false;
                for (u32 s = 0; s < NumShadowed && !Active; ++s) Active = (SpotIds[s] == Owner);
                if (Active) continue;
                // Precisa seguir visivel (ter entrada em DstLights) p/ valer o redesenho.
                for (const ShadowCand& C : ShadowCands) {
                    if (C.Id != Owner) continue;
                    const FLight& Lr = SceneLights[C.LightIdx];
                    Mat44 LVP; f32 FarP;
                    SpotShadowVP(Lr, LVP, FarP);
                    FGPULight& G   = DstLights[C.Gpu];
                    G.ShadowMatrix = ToShadowUV(LVP);
                    G.SpotParams.Y = static_cast<f32>(i);
                    G.SpotParams.Z = LocalShadows.SpotFadeAt(i);
                    LocalShadowJobs.push_back({ LVP, Lr.Position, FarP, i });
                    break;
                }
            }

            std::sort(CubeCands.begin(), CubeCands.end(), ByScore);
            const u32 NumCubes = std::min<u32>(static_cast<u32>(CubeCands.size()),
                                               FLocalShadows::kActiveCubes);
            u64 CubeIds[FLocalShadows::kActiveCubes];
            u32 CubeSlot[FLocalShadows::kActiveCubes];
            for (u32 c = 0; c < NumCubes; ++c)
                CubeIds[c] = SceneLights[CubeCands[c].LightIdx].Id;
            LocalShadows.AcquireCubeSlots(CubeIds, NumCubes, CubeSlot);
            LocalShadows.UpdateCubeFades(CubeIds, NumCubes, FrameState->LastDeltaTime);

            for (u32 c = 0; c < NumCubes; ++c) {
                const u32 Cube = CubeSlot[c];
                if (Cube == FLocalShadows::kNoSlot) continue;

                const FLight& L = SceneLights[CubeCands[c].LightIdx];
                FGPULight& G   = DstLights[CubeCands[c].Gpu];
                G.SpotParams.Y = static_cast<f32>(Cube);
                G.SpotParams.Z = LocalShadows.CubeFadeAt(Cube);
                LocalCubeJobs.push_back({ L.Position, L.AttenuationRadius, Cube });
            }

            // Cubos em fade-out (mesma logica dos spots; o point nem matriz precisa).
            for (u32 i = 0; i < FLocalShadows::kMaxCubeShadows; ++i) {
                const u64 Owner = LocalShadows.CubeOwnerAt(i);
                if (Owner == 0 || LocalShadows.CubeFadeAt(i) <= 0.0f) continue;
                bool Active = false;
                for (u32 c = 0; c < NumCubes && !Active; ++c) Active = (CubeIds[c] == Owner);
                if (Active) continue;
                for (const ShadowCand& C : CubeCands) {
                    if (C.Id != Owner) continue;
                    const FLight& Lr = SceneLights[C.LightIdx];
                    FGPULight& G   = DstLights[C.Gpu];
                    G.SpotParams.Y = static_cast<f32>(i);
                    G.SpotParams.Z = LocalShadows.CubeFadeAt(i);
                    LocalCubeJobs.push_back({ Lr.Position, Lr.AttenuationRadius, i });
                    break;
                }
            }

            FrameLightCount = NumLights; // consumido pelos dispatches de direta local, mais adiante
            const u64 NewLightSetSignature = LightSetSignature ^ static_cast<u64>(NumLights);
            if (NewLightSetSignature != FrameLightSetSignature)
                NrdDirect.InvalidateHistory();
            FrameLightSetSignature = NewLightSetSignature;
            ReSTIRDI.SetLightSetSignature(FrameLightSetSignature);
            // Um bit escolhe o produtor da direta local: raster ou ReSTIR DI. O mesmo predicado
            // manda no dispatch e no deferred; divergir aqui apagaria ou dobraria luz.
            const f32 DirectLocalMode = Modes.ReSTIRDIActiveFrame ? 1.0f : 0.0f;
            MappedCB->LightParams  = { static_cast<f32>(NumLights),
                                       1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                       LocalShadows.GetDepthBias(),
                                       DirectLocalMode };
            MappedCB->LightParams2 = { 1.0f / static_cast<f32>(FLocalShadows::kCubeResolution),
                                       FLocalShadows::kPointNear, 0.0f, 0.0f };

            if (Modes.VolFogActive)
                VolumetricFog.PatchLights(NumLights,
                                          1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                          LocalShadows.GetDepthBias(),
                                          FLocalShadows::kPointNear);
        }
        return ShadowJobs;
    }

    // Monta as listas de draw do frame: escreve o ObjectConstants de cada renderavel, resolve a
    // selecao, aplica frustum + oclusao HZB e ordena front-to-back. E a 2a etapa de preenchimento
    // do FPassContext — daqui em diante Ctx.All / Ctx.Visible / Ctx.Selection sao validos.
    void Renderer::BuildDrawLists(FPassContext& _Ctx) {
        const FFrameView& Vw     = *_Ctx.View;
        const u32 FrameSlot      = _Ctx.FrameSlot;
        // Antes havia uma leitura propria da camera; e a MESMA que produziu o
        // Vw.CameraPosition no ResolveFrameView, e nada move a camera dentro do frame.
        const Vec3& CamPos       = _Ctx.View->CameraPosition;

        const u32 FrameObjectBase = FrameSlot * MaxObjects;

        auto& AllItems = _Ctx.All;

        u32             SelectedSlot  = kInvalidSlot;
        const FGpuMesh* SelectedMesh  = nullptr;
        Mat44           SelectedModel = Mat44::Identity();
        // Hasteado do loop: a selecao virou uma consulta (mesh OU luz), e o loop abaixo roda
        // por renderavel da cena.
        const int       SelectedRenderable = GetSelectedObject();
        {
            const std::vector<FRenderable>& RList = SceneState->Scene.Renderables();
            AllItems.reserve(RList.size());
            const size_t PrevCount = SceneState->PreviousModels.size();
            SceneState->PreviousModels.resize(RList.size(), Mat44::Identity());
            const bool WriteOcclusionBounds = UseOcclusionCulling && HiZ.ObjectsReady();
            for (size_t si = 0; si < RList.size(); ++si) {
                const FRenderable& R = RList[si];
                if (WriteOcclusionBounds)
                    HiZ.WriteBounds(FrameSlot, static_cast<u32>(si), R.AABBMin, R.AABBMax);
                if (!R.Visible || R.RaytracingOnly || !R.Mesh || !R.Mesh->IsValid()) continue;
                if (AllItems.size() >= MaxObjects) break;
                FMaterial* Mat = (R.Material && R.Material->IsFinalized()) ? R.Material : ActiveMaterial;
                const u32 Slot = FrameObjectBase + static_cast<u32>(AllItems.size());
                const Mat44 Model = R.Transform.Matrix();
                const Mat44 PrevModel = (si < PrevCount) ? SceneState->PreviousModels[si] : Model;
                ObjectConstants OC;
                OC.MVP            = Model * Vw.ViewProjection;
                OC.ModelMatrix    = Model;
                OC.CurMVPNoJitter = Model * Vw.ViewProjUnjittered;
                OC.PrevMVP        = PrevModel * FrameState->PrevViewProj;
                std::memcpy(MappedObjectCB + static_cast<size_t>(Slot) * sizeof(ObjectConstants),
                            &OC, sizeof(ObjectConstants));
                if (static_cast<int>(si) == SelectedRenderable) {
                    SelectedSlot = Slot; SelectedMesh = R.Mesh; SelectedModel = Model;
                }
                AllItems.push_back({ &R, Mat, Slot, static_cast<u32>(si) });
                SceneState->PreviousModels[si] = Model;
            }
        }
        // A selecao entra no contexto AQUI, no unico laco que ja varre a cena: o contorno a
        // consome ~1400 linhas abaixo, e recompor la exigiria varrer tudo de novo.
        _Ctx.Selection = { SelectedSlot, SelectedMesh, SelectedModel, SelectedRenderable };

        // Resultado do teste HZB gravado ha kFramesInFlight frames neste slot (a fence
        // ja foi esperada no BeginFrame). nullptr = sem teste valido -> tudo visivel.
        const u32* OcclusionVis = UseOcclusionCulling
            ? HiZ.ResolveResults(FrameSlot, static_cast<u32>(SceneState->Scene.Renderables().size()))
            : nullptr;
        u32 OccludedCount = 0;

        auto& VisibleScratch = _Ctx.Visible;
        VisibleScratch.reserve(AllItems.size());
        for (const FDrawItem& A : AllItems) {
            if (UseFrustumCulling && Vw.AABBOutsideFrustum(A.R->AABBMin, A.R->AABBMax)) continue;
            // Objeto selecionado nunca e cullado (gizmo/drag move mais rapido que a
            // latencia do readback e o pop incomodaria bem aqui). O resultado so cobre
            // [0, Capacity); indices alem disso (ex.: proxy RT do terreno) ficam visiveis.
            if (OcclusionVis && A.SceneIndex < HiZ.Capacity() &&
                !OcclusionVis[A.SceneIndex] &&
                static_cast<int>(A.SceneIndex) != SelectedRenderable) {
                ++OccludedCount;
                continue;
            }
            const f32 cx = (A.R->AABBMin.X + A.R->AABBMax.X) * 0.5f - CamPos.X;
            const f32 cy = (A.R->AABBMin.Y + A.R->AABBMax.Y) * 0.5f - CamPos.Y;
            const f32 cz = (A.R->AABBMin.Z + A.R->AABBMax.Z) * 0.5f - CamPos.Z;
            VisibleScratch.push_back({ A.R, A.Mat, cx*cx + cy*cy + cz*cz, A.Slot, A.SceneIndex });
        }
        std::sort(VisibleScratch.begin(), VisibleScratch.end(),
                  [](const FVisibleItem& a, const FVisibleItem& b) { return a.Dist < b.Dist; });
        LastVisibleCount  = static_cast<u32>(VisibleScratch.size());
        LastOccludedCount = OccludedCount;

        if (UseTerrain && Terrain.IsLoaded())
            Terrain.UpdatePerFrame(FrameSlot, Vw.ViewProjection, Vw.ViewProjUnjittered,
                                   FrameState->PrevViewProj,
                                   Vw.CameraPosition, Vw.FovY, Vw.MipBias);
    }
}
