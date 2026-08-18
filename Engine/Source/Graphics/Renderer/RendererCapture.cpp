#include "Smile/Graphics/Renderer/Renderer.h"
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
    namespace {
        constexpr DXGI_FORMAT kDebugPreviewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr u32 kDebugPreviewRowPitch =
            (Renderer::kDebugPreviewWidth * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        constexpr u64 kDebugProbeIrrOffset  = 0;
        constexpr u64 kDebugProbeDistOffset = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        constexpr u64 kDebugProbeReadbackSize =
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * 2ull;

        f32 HalfToFloat(u16 H) {
            const u32 Sign = static_cast<u32>(H & 0x8000u) << 16;
            u32 Exp  = (H >> 10) & 0x1Fu;
            u32 Mant = H & 0x03FFu;
            u32 Bits;
            if (Exp == 0) {
                if (Mant == 0) {
                    Bits = Sign;
                } else {
                    i32 E = -14;
                    while ((Mant & 0x0400u) == 0) { Mant <<= 1; --E; }
                    Mant &= 0x03FFu;
                    Bits = Sign | (static_cast<u32>(E + 127) << 23) | (Mant << 13);
                }
            } else if (Exp == 0x1Fu) {
                Bits = Sign | 0x7F800000u | (Mant << 13);
            } else {
                Bits = Sign | ((Exp + 112u) << 23) | (Mant << 13);
            }
            f32 Result;
            std::memcpy(&Result, &Bits, sizeof(Result));
            return Result;
        }
    }

    void Renderer::CreateDebugPreviewTargets() {
        DebugPreviewPass.Initialize(Backend->Device.Native(), kDebugPreviewFormat);

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format   = kDebugPreviewFormat;
        Clear.Color[0] = 0.035f;
        Clear.Color[1] = 0.037f;
        Clear.Color[2] = 0.031f;
        Clear.Color[3] = 1.0f;
        DebugPreviewTarget = GpuResources::CreateTex2D(
            Backend->Device.Native(), kDebugPreviewWidth, kDebugPreviewHeight, kDebugPreviewFormat,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::RenderTargets, &Clear, 1, 1, "Preview de debug");

        DebugPreviewRTVHeap.Initialize(
            Backend->Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        Backend->Device.Native()->CreateRenderTargetView(
            DebugPreviewTarget.Get(), nullptr, DebugPreviewRTVHeap.CpuHandle(0));

        // Preview e probe possuem buffers separados porque seus tamanhos diferem muito.
        const u64 PreviewBytes = static_cast<u64>(kDebugPreviewRowPitch) * kDebugPreviewHeight;

        for (u32 I = 0; I < FCommandQueue::kFramesInFlight; ++I) {
            DebugPreviewReadback[I] =
                GpuResources::CreateReadbackBuffer(Backend->Device.Native(), PreviewBytes);
            DebugPreviewReadbackPending[I] = false;
            DebugPreviewReadbackVersion[I] = 0;

            DebugProbeSampleReadback[I] =
                GpuResources::CreateReadbackBuffer(Backend->Device.Native(), kDebugProbeReadbackSize);
            DebugProbeSamplePending[I] = false;
            DebugProbeSampleVersion[I] = 0;
            DebugProbeSampleIndex[I]   = kNoDebugProbe;
        }
    }

    void Renderer::CollectDebugPreviewReadback(u32 _FrameSlot) {
        if (_FrameSlot >= FCommandQueue::kFramesInFlight) return;

        if (DebugPreviewReadbackPending[_FrameSlot]) {
            DebugPreviewReadbackPending[_FrameSlot] = false;
            if (DebugPreviewReadbackVersion[_FrameSlot] == DebugPreviewConfigVersion &&
                DebugPreviewEnabled) {
                void* Mapped = nullptr;
                const size_t TotalBytes =
                    static_cast<size_t>(kDebugPreviewRowPitch) * kDebugPreviewHeight;
                D3D12_RANGE ReadRange{ 0, TotalBytes };
                SMILE_HR(DebugPreviewReadback[_FrameSlot]->Map(0, &ReadRange, &Mapped));

                DebugPreviewPixels.resize(
                    static_cast<size_t>(kDebugPreviewWidth) * kDebugPreviewHeight * 4u);
                const auto* Src = static_cast<const u8*>(Mapped);
                u8* Dst = DebugPreviewPixels.data();
                constexpr size_t TightRow = static_cast<size_t>(kDebugPreviewWidth) * 4u;
                for (u32 Y = 0; Y < kDebugPreviewHeight; ++Y)
                    std::memcpy(Dst + static_cast<size_t>(Y) * TightRow,
                                Src + static_cast<size_t>(Y) * kDebugPreviewRowPitch,
                                TightRow);

                D3D12_RANGE NoWrite{ 0, 0 };
                DebugPreviewReadback[_FrameSlot]->Unmap(0, &NoWrite);
                DebugPreviewPixelsReady = true;
            }
        }

        if (DebugProbeSamplePending[_FrameSlot]) {
            DebugProbeSamplePending[_FrameSlot] = false;
            if (DebugProbeSampleVersion[_FrameSlot] == DebugPreviewConfigVersion &&
                DebugProbeSampleIndex[_FrameSlot] == DebugProbeIndex &&
                DebugPreviewEnabled) {
                void* Mapped = nullptr;
                D3D12_RANGE ReadRange{ 0, static_cast<SIZE_T>(kDebugProbeReadbackSize) };
                SMILE_HR(DebugProbeSampleReadback[_FrameSlot]->Map(
                    0, &ReadRange, &Mapped));
                const auto* Bytes = static_cast<const u8*>(Mapped);
                const auto* Irr = reinterpret_cast<const u16*>(
                    Bytes + kDebugProbeIrrOffset);
                const auto* Dist = reinterpret_cast<const u16*>(
                    Bytes + kDebugProbeDistOffset);

                DebugProbeSampleResult.ProbeIndex = DebugProbeSampleIndex[_FrameSlot];
                for (u32 C = 0; C < 3; ++C)
                    DebugProbeSampleResult.Irradiance[C] =
                        std::pow(std::max(HalfToFloat(Irr[C]), 0.0f), 1.5f);
                const f32 Mean  = HalfToFloat(Dist[0]);
                const f32 Mean2 = HalfToFloat(Dist[1]);
                DebugProbeSampleResult.MeanDistance = Mean;
                DebugProbeSampleResult.DistanceDeviation =
                    std::sqrt(std::abs(Mean2 - Mean * Mean));

                D3D12_RANGE NoWrite{ 0, 0 };
                DebugProbeSampleReadback[_FrameSlot]->Unmap(0, &NoWrite);
                DebugProbeSampleReady = true;
            }
        }
    }

    bool Renderer::ConsumeDebugPreview(std::vector<u8>& _OutPixels) {
        if (!DebugPreviewPixelsReady) return false;
        _OutPixels = DebugPreviewPixels;
        DebugPreviewPixelsReady = false;
        return true;
    }

    bool Renderer::ConsumeDebugProbeSample(FDebugProbeSample& _OutSample) {
        if (!DebugProbeSampleReady) return false;
        _OutSample = DebugProbeSampleResult;
        DebugProbeSampleReady = false;
        return true;
    }

    // === Captura deterministica (Docs/CAPTURE-PROTOCOL.md) ==============================

    FCaptureSettings Renderer::CurrentCaptureSettings() const {
        FCaptureSettings S;
        S.Upscaler        = static_cast<u32>(Upscaler);
        S.Denoiser        = static_cast<u32>(Denoiser);
        S.UpscalerQuality = UpscalerQuality;
        S.UseTAA          = UseTAA;
        S.RenderScale     = RenderScale;
        S.ElapsedTime     = ElapsedTime;
        S.Wetness         = Weather.GetWetness();
        S.TimeOfDayHours  = TimeOfDay.TimeHours;
        S.SunDir[0] = SunDir.X; S.SunDir[1] = SunDir.Y; S.SunDir[2] = SunDir.Z;
        S.CacheAutoWarmup = RadianceCache.GetAutoWarmup();
        return S;
    }

    // Capturas partem da molhadura de equilibrio para nao depender do instante do clique.
    f32 Renderer::SettledWetness() const {
        const f32 Target = Weather.GetRainAmount();
        return Target <= 0.001f ? 0.0f : Target;
    }

    // Use setters para preservar os mesmos acoplamentos e invalidacoes da UI.
    void Renderer::ApplyCaptureSettings(const FCaptureSettings& _S) {
        // Denoiser primeiro: DLSS_RR trava o upscaler em DLSS por conta propria (SetDenoiser), e
        // faze-lo depois desfaria o upscaler que acabamos de escolher.
        Settings().SetDenoiser(static_cast<EDenoiser>(_S.Denoiser));
        Settings().SetUpscaler(static_cast<EUpscaler>(_S.Upscaler));
        Settings().SetUpscalerQuality(_S.UpscalerQuality);
        Settings().SetUseTAA(_S.UseTAA);
        // Ignorado sob DLSS_RR (a res de entrada vem do modo de qualidade), e esse e o
        // comportamento correto — ver Renderer::SetRenderScale.
        Settings().SetRenderScale(_S.RenderScale);
    }

    // Restaure o mundo sempre; restaure knobs apenas quando o preset os alterou.
    void Renderer::RestoreCaptureState(const FCaptureSettings& _S) {
        ElapsedTime      = _S.ElapsedTime;
        TimeOfDay.TimeHours = _S.TimeOfDayHours;
        SetSunDirection(Vec3{ _S.SunDir[0], _S.SunDir[1], _S.SunDir[2] });
        Weather.SetWetness(_S.Wetness);
        // Nao sobrescreva uma escolha feita pelo operador durante a sessao.
        if (RadianceCache.GetAutoWarmup() == Capture.AppliedByPreset().CacheAutoWarmup)
            Settings().SetRadianceCacheAutoWarmup(_S.CacheAutoWarmup);
        if (!_S.KnobsMutated) return;

        // Preserve qualquer knob que divergiu do valor aplicado pelo preset.
        const FCaptureSettings& Applied = Capture.AppliedByPreset();
        FCaptureSettings Target = _S;
        if (static_cast<u32>(Upscaler) != Applied.Upscaler) Target.Upscaler = static_cast<u32>(Upscaler);
        if (static_cast<u32>(Denoiser) != Applied.Denoiser) Target.Denoiser = static_cast<u32>(Denoiser);
        if (UpscalerQuality != Applied.UpscalerQuality)     Target.UpscalerQuality = UpscalerQuality;
        if (UseTAA != Applied.UseTAA)                       Target.UseTAA = UseTAA;
        if (RenderScale != Applied.RenderScale)             Target.RenderScale = RenderScale;
        ApplyCaptureSettings(Target);
    }

    void Renderer::UpdateFrameCapture() {
        // Evita que setters usados pelo proprio protocolo cancelem a sessao.
        struct FGuard {
            bool& Flag;
            ~FGuard() { Flag = false; }
        } Guard{ CaptureSetupGuard };
        CaptureSetupGuard = true;

        // Restauracao ANTES de abrir sessao nova: as duas mexem em upscaler/render scale, e as
        // duas so podem acontecer aqui, fora da gravacao do command list.
        if (Capture.HasPendingRestore()) RestoreCaptureState(Capture.ConsumeRestore());

        if (!Capture.HasPendingRequest()) return;

        // O estado a devolver e guardado SEMPRE, nos dois presets: mesmo o gameplay, que nao toca
        // em knob nenhum, muda o relogio.
        FCaptureSettings Stash = CurrentCaptureSettings();

        // O preset cientifico desliga upscaler e TAA para eliminar todo jitter.
        FCaptureSettings Applied = Stash;
        if (Capture.Pending().Preset == ECapturePreset::Scientific) {
            Stash.KnobsMutated = true;
            FCaptureSettings Sci;
            Sci.Upscaler        = static_cast<u32>(EUpscaler::None);
            Sci.Denoiser        = static_cast<u32>(Denoiser); // eixo do operador, nao do preset
            Sci.UpscalerQuality = 0;
            Sci.UseTAA          = false;
            Sci.RenderScale     = 1.0f;
            // Sair do DLSS_RR e consequencia de tirar o upscaler (o RR faz denoise E upscale num
            // eval so). Fica registrado no manifesto, que grava o estado EFETIVO.
            if (Denoiser == EDenoiser::DLSS_RR) Sci.Denoiser = static_cast<u32>(EDenoiser::NRD);
            ApplyCaptureSettings(Sci);
            // O que ficou DE FATO no renderer, e nao o que o preset pediu: o SetUpscaler pode ter
            // recusado um upscaler indisponivel e o SetRenderScale e ignorado sob DLSS_RR. E
            // contra ISTO que a restauracao compara para saber se o operador mexeu.
            Applied = CurrentCaptureSettings();
            Applied.KnobsMutated = true;
        }

        // Auto-warmup mudaria o regime e invalidaria historicos no meio da contagem.
        Settings().SetRadianceCacheAutoWarmup(false);
        Applied.CacheAutoWarmup = false;
        Capture.StashSettings(Stash, Applied);

        // Fase temporal canonica torna animacoes reproduziveis entre capturas.
        ElapsedTime = kCaptureElapsedSeconds;
        Weather.SetWetness(SettledWetness());

        // Hora do dia e estado autorado e so e fixada quando o pedido a declara.
        const f32 PinHours = Capture.Pending().PinTimeOfDayHours;
        CapturePinApplied  = -1.0f;
        if (PinHours >= 0.0f && TimeOfDay.Enabled) {
            TimeOfDay.TimeHours = std::fmod(PinHours, 24.0f);
            SetSunDirection(TimeOfDay.SunDirection());
            // O EFETIVO, nao o pedido: com o TOD desligado o sol e autorado a mao e fixar a hora
            // nao faz nada, entao o manifesto nao pode registrar um controle que nao houve.
            CapturePinApplied = TimeOfDay.TimeHours;
        }
        // Reafirmados a cada frame da sessao (ver TickWorldClock). Com o TOD desligado o sol e
        // autorado a mao e nao deriva da hora, por isso os dois sao guardados.
        CaptureSunHours = TimeOfDay.TimeHours;
        CaptureSunDir   = SunDir;

        // Resete depois do preset, que pode realocar alvos e invalidar historicos.
        Capture.BeginSession();
        Settings().NotifyDeterministicCapture();
        LogInfo("Captura: aquecendo " + std::to_string(Capture.Active().WarmupFrames) +
                " frames renderizados");
    }

    FCaptureState Renderer::CollectCaptureState(const FFrameModes& _Modes,
                                                const FEffectiveIndirectPolicy& _Policy) const {
        FCaptureState S;
        S.OutputWidth  = OutputWidth();
        S.OutputHeight = OutputHeight();
        S.RenderWidth  = RenderWidth();
        S.RenderHeight = RenderHeight();
        S.RenderScale  = RenderScale;

        switch (Upscaler) {
            case EUpscaler::FSR:  S.Upscaler = "FSR";  break;
            case EUpscaler::DLSS: S.Upscaler = "DLSS"; break;
            default:              S.Upscaler = "None"; break;
        }
        switch (Denoiser) {
            case EDenoiser::NRD:     S.Denoiser = "NRD";     break;
            case EDenoiser::DLSS_RR: S.Denoiser = "DLSS_RR"; break;
            default:                 S.Denoiser = "None";    break;
        }
        // Registre o que executou por dominio. RR vale apenas quando o eval ocorreu de fato.
        S.IndirectDenoiserEffective = _Modes.NrdIndirectMode ? "NRD"
                                    : (RRRanThisFrame ? "DLSS_RR" : "None");
        S.DirectDenoiserEffective   = _Modes.NrdDirectMode   ? "NRD"
                                    : (RRRanThisFrame ? "DLSS_RR" : "None");
        S.UpscalerQuality = UpscalerQuality;
        // EFETIVO, nao selecionado: o preset cientifico desliga o upscaler, e sem upscaler o
        // `TAAActive = UseTAA && !UpscaleActive` decide sozinho se o TAA acendeu.
        S.UseTAA = _Modes.TAAActive;

        // O manifesto descreve execucao efetiva, nao apenas toggles solicitados.
        S.UseGI       = UseGI;               // dominio indireto: intencao, e nao ha passe unico
        S.DDGIReady   = DDGI.IsReady();      // EXISTENCIA do volume (criterio do fallback)
        S.ReSTIRGI    = _Modes.ReSTIRGIActive;
        S.ReSTIRDI    = _Modes.ReSTIRDIActiveFrame;
        S.ReGIR       = ReGIRRanThisFrame;    // toggle + consumidor + luz na cena
        S.ReGIRRequested     = UseReGIR;      // so o toggle — a diferenca entre os dois e o achado
        S.PunctualLightCount = GILightCountThisFrame;

        // Levantamento e distribuicao publicada podem divergir durante um rebuild assincrono.
        {
            const FMeshLights::FDistributionStats& D = MeshLights.Distribution();
            S.MeshLightSurveyed  = MeshLights.ExtractedTriangleCount();
            S.MeshLightSamplable = MeshLights.SamplableTriangleCount();
            S.MeshAliasReady     = MeshLights.IsAliasReady();
            S.MeshLightTotalFlux = D.TotalFlux;
            // Fluxo zero e valido quando a alias table esta pronta.
            S.MeshLightFluxValid = MeshLights.IsAliasReady();
            S.MeshLightUniformFallback = D.UniformFallback;
            S.MeshTriDegenerate = D.Degenerate;
            S.MeshTriZeroFlux   = D.ZeroFlux;
            S.MeshTriNonFinite  = D.NonFinite;
            S.MeshLightTrianglePayloadBytes = MeshLights.TrianglePayloadBytes();
            S.MeshLightAliasUploadPayloadBytes  = MeshLights.AliasUploadPayloadBytes();
            S.MeshLightAliasDefaultPayloadBytes = MeshLights.AliasDefaultPayloadBytes();
            S.MeshCompactRequested         = MeshLights.GetCompactSupport();
            S.MeshCompactEffective         = MeshLights.IsCompactSupportEffective();
            // TAMANHO DO DOMINIO, e nao trafego — ver o comentario no FCaptureState.
            S.MeshLightAliasDomainBytes    = MeshLights.AliasDomainBytes();
            S.MeshLightTriangleDomainBytes = MeshLights.TriangleDomainBytes();
            S.MeshLightTriangleCompactPayloadBytes  = MeshLights.TriangleCompactPayloadBytes();
            S.MeshLightTriangleStagingPayloadBytes  = MeshLights.TriangleStagingPayloadBytes();
            S.MeshLightTriangleReadbackPayloadBytes = MeshLights.TriangleReadbackPayloadBytes();
            S.MeshLightVramBytes             = MeshLights.VramBytes();

            S.DIAnalyticCandidatesRequested = ReSTIRDI.GetInitialCandidates();
            S.DIMeshCandidatesRequested     = ReSTIRDI.GetMeshCandidates();
            S.DIMeshLightsInPoolRequested   = ReSTIRDI.GetMeshLightsInPool();
            S.DIInitialVisibilityRequested  = ReSTIRDI.GetInitialVisibility();
            // Valores efetivos sao os publicados no cbuffer do frame concluido.
            const bool DIRan = _Modes.ReSTIRDIActiveFrame;
            S.DIAnalyticCandidatesEffective = DIRan ? ReSTIRDI.EffectiveAnalyticCandidates() : 0u;
            S.DIMeshCandidatesEffective     = DIRan ? ReSTIRDI.EffectiveMeshCandidates() : 0u;
            S.DIMeshLightsInPoolEffective   = DIRan && ReSTIRDI.EffectiveMeshLightCount() > 0u;
            S.DIInitialVisibilityEffective  = DIRan && ReSTIRDI.GetInitialVisibility();
            S.DIRenderWidth  = DIRan ? ReSTIRDI.DIRenderWidth()  : 0u;
            S.DIRenderHeight = DIRan ? ReSTIRDI.DIRenderHeight() : 0u;
            // RIS, dado compacto e meia resolucao continuam nao existindo. Ficam explicitamente
            // false em vez de ausentes — ver o comentario no FCaptureState.
        }

        S.Reflections = _Modes.ReflectionsActive;
        // Use o snapshot publicado; o resolve pode alterar o estado antes desta coleta.
        S.CacheUpdate = RadianceCache.PublishedUpdateThisFrame();
        S.CacheQuery  = RadianceCache.PublishedQueryThisFrame();
        // Identifique o produtor e a fracao efetiva que alimentaram a imagem.
        S.CacheDedicatedUpdate = RadianceCache.PublishedDedicatedThisFrame();
        S.CacheCompactUpdate   = S.CacheDedicatedUpdate && RadianceCache.GetCompactUpdate();
        S.CacheUpdateFraction  = RadianceCache.PublishedUpdateFraction();
        S.CacheUsePrevTerminal = RadianceCache.GetUsePrevCacheAtTerminal();
        S.CacheMaxVertices     = RadianceCache.PublishedUpdateVertices();
        S.CacheMinRoughness    = RadianceCache.PublishedMinCacheableRoughness();
        // Piso de confianca EFETIVO — zero quando ninguem consultou o cache neste frame. Ele vale
        // para os dois lados (traces de render e terminal do updater), entao vem publicado por
        // quem quer que tenha recebido a flag de query.
        S.CacheMinSamples      = RadianceCache.PublishedMinSampleCount();
        S.CacheStats  = RadianceCache.PublishedStatsThisFrame();
        S.CacheStatsDetail = RadianceCache.PublishedStatsDetailThisFrame();
        S.CacheStatsSource = RadianceCache.PublishedStatsSourceThisFrame();
        // O warmup explica por que uma consulta pode estar fechada neste frame.
        S.CacheWarmup     = RadianceCache.WarmupStateName();
        S.CacheAutoWarmup = RadianceCache.GetAutoWarmup();
        // Preserve pedido e snapshot efetivo; nao reavalie politica depois do EndFrame.
        S.IndirectPrimaryRequested  = IndirectPrimaryName(IndirectPrimary);
        S.IndirectPrimaryEffective  = IndirectPrimaryName(_Policy.Primary);
        S.IndirectFallbackRequested = IndirectFallbackName(IndirectFallback);
        S.IndirectFallbackEffective = IndirectFallbackName(_Policy.Fallback);
        S.IndirectFallbackActive    = _Policy.FallbackActive;
        S.DDGISurfaceAvailable      = _Policy.DDGISurface;
        S.DDGIVolumetricAvailable   = _Policy.DDGIVolumetric;
        S.GIMeasureTerminatorOff = GIMeasureTerminatorOff;
        // Toggle, e nao "efetivo": esta politica e lida por frame pelos DOIS consumidores (o
        // gather do ReSTIR GI e o produtor do cache) da mesma fonte, entao ela descreve o regime
        // do frame inteiro. Mesmo criterio do GIMeasureTerminatorOff acima.
        S.GIBackfacePolicy = ReSTIRGI.GetBackfacePolicy();

        // Ocupacao vem do pos-resolve; queries e hits, do anel pre-resolve. Nao publique dados
        // antigos quando o cache nao participou, mas preserve configuracoes somente-leitura.
        if (S.CacheUpdate || S.CacheQuery) {
            S.CacheOccupied  = CaptureCacheStats.Occupied;
            S.CacheValid     = CaptureCacheStats.HasSamples;
            S.CacheConfident = CaptureCacheStats.Confident;
            S.CacheSamples   = CaptureCacheStats.Samples;
            S.CacheEvicted   = CaptureCacheStats.Evicted;
            S.CacheCapacity  = RadianceCache.Capacity();
            const FRadianceCacheStats& Ring = RadianceCache.Stats();
            S.CacheQueries  = Ring.Queries;  // zero sem a instrumentacao ligada
            S.CacheHits     = Ring.Hits;
            // Do mesmo anel, e zerados sem o regime de detalhe: as duas metades da telemetria vem
            // da mesma copia PRE-resolve, porque misses e insercoes sao escritos pelos passes que
            // rodam antes dele.
            S.CacheMissShort   = Ring.MissShort;
            S.CacheMissCone    = Ring.MissCone;
            S.CacheMissNoEntry = Ring.MissNoEntry;
            S.CacheMissEmpty   = Ring.MissEmpty;
            S.CacheMissWarming = Ring.MissWarming;
            S.CacheMissStale   = Ring.MissStale;
            S.CacheInsertTries = Ring.InsertTries;
            S.CacheInsertFull  = Ring.InsertFull;
            S.CacheContended   = Ring.Contended;
            S.CacheRetries     = Ring.Retries;
            S.CacheCapped      = Ring.Capped;
            S.CacheProbeSum    = Ring.ProbeSum;
            S.CacheProbeMax    = Ring.ProbeMax;
            S.CachePaths       = Ring.Paths;
            S.CachePathVerts   = Ring.PathVerts;
            S.CachePathDepth   = Ring.PathDepth;
            S.CacheTermSky     = Ring.TermSky;
            S.CacheTermCache   = Ring.TermCache;
            S.CacheTermKilled  = Ring.TermKilled;
            S.CacheTermMiss    = Ring.TermMiss;
            S.CacheTermNoQuery = Ring.TermNoQuery;
            S.CacheTermLobe    = Ring.TermLobe;
            S.CacheTermOther   = Ring.TermOther;
        }

        // Estatisticas de terminal existem mesmo quando update e query estao fechados.
        if (S.CacheStatsSource) {
            const FRadianceCacheStats& Ring = RadianceCache.Stats();
            S.CacheSrcTotal      = Ring.SrcTotal;
            S.CacheSrcCache      = Ring.SrcCache;
            S.CacheSrcDDGI       = Ring.SrcDDGI;
            S.CacheSrcZero       = Ring.SrcZero;
            S.CacheSrcIneligible = Ring.SrcIneligible;
        }

        // Antes do ++ dos contadores: e o indice com que ESTE frame amostrou.
        S.TemporalSampleIndex = TemporalSampleIndex;
        S.FrameIndex          = FrameIndex;

        const Vec3 Pos = Camera.GetPosition();
        S.CameraPos[0] = Pos.X; S.CameraPos[1] = Pos.Y; S.CameraPos[2] = Pos.Z;
        S.PitchDeg = Camera.GetPitch() * ToDeg;
        S.YawDeg   = Camera.GetYaw()   * ToDeg;
        S.FovYDeg  = kFovYDegrees;

        S.SunDir[0] = SunDir.X; S.SunDir[1] = SunDir.Y; S.SunDir[2] = SunDir.Z;
        S.TimeOfDayHours     = TimeOfDay.TimeHours;
        S.TimeOfDayEnabled   = TimeOfDay.Enabled;
        S.PinnedHoursApplied = CapturePinApplied;
        return S;
    }

    void Renderer::FinishFrameCapture(const FFrameModes& _Modes,
                                      const FEffectiveIndirectPolicy& _Policy, u32 _FrameSlot) {
        if (!Capture.AdvanceFrame()) return;
        // A captura e offline e paga um unico stall antes de mapear os readbacks.
        Backend->DirectQueue.Flush();
        // O anel contem query/hits pre-resolve; CaptureStats contem ocupacao pos-resolve.
        RadianceCache.CollectStats(_FrameSlot);
        CaptureCacheStats = {};
        RadianceCache.CollectCaptureStats(CaptureCacheStats);
        Capture.Finish(CollectCaptureState(_Modes, _Policy));
    }
    // Nomes sao chaves estaveis do visualizador e preservam selecao entre reconstrucoes.
    void Renderer::RegisterDebugTargets() {
        using namespace DebugTargets;
        constexpr u32 kNoSlot = 0xFFFFFFFFu;

        // O indice e detalhe de apresentacao; preserva a selecao pelo nome enquanto a lista
        // e reconstruida. Isso tambem remove entradas DDGI/ReSTIR obsoletas quando uma nova
        // cena nao consegue recriar seus recursos, em vez de deixar um SRV ja liberado visivel.
        std::vector<std::string> SelectedNames;
        std::string MainTargetName;
        {
            const auto& Previous = All();
            SelectedNames.reserve(DebugSelection.size());
            for (u32 Index : DebugSelection)
                if (Index < Previous.size()) SelectedNames.push_back(Previous[Index].Name);
            if (DebugTargetIndex < Previous.size())
                MainTargetName = Previous[DebugTargetIndex].Name;
        }
        Clear();

        // --- Cena / G-buffer -------------------------------------------------------------
        // Os 8 modos antigos viram entradas com decode GBufferField (SubIndex == modo antigo).
        // O SrvSlot aqui e so p/ o registro ter algo valido; o decode le os 3 MRTs da tabela.
        if (GBuffer.IsInitialized()) {
            const u32 GB = GBuffer.SRVTableStart();
            Register("GBuffer · base color",    GB, EDebugDecode::GBufferField, 1);
            Register("GBuffer · normal",        GB, EDebugDecode::GBufferField, 2);
            Register("GBuffer · roughness",     GB, EDebugDecode::GBufferField, 3);
            Register("GBuffer · metallic",      GB, EDebugDecode::GBufferField, 4);
            Register("GBuffer · subsurface",    GB, EDebugDecode::GBufferField, 5);
            Register("GBuffer · AO",            GB, EDebugDecode::GBufferField, 6);
            Register("GBuffer · shading model", GB, EDebugDecode::GBufferField, 7);
        }
        if (Targets.VelocitySRVSlot != kNoSlot)
            Register("Motion vectors", Targets.VelocitySRVSlot, EDebugDecode::Velocity);
        if (Targets.DepthSRVSlot != kNoSlot)
            Register("Depth (reverse-Z)", Targets.DepthSRVSlot, EDebugDecode::ReverseZ);
        // "HDR color" NAO entra: o Targets.HDRColorBuffer e o proprio render target deste passe
        // (RTV em Targets.HDRRTVHeap.CpuHandle(0)), entao le-lo como SRV no mesmo draw seria
        // ler e escrever o mesmo recurso. Precisaria de copia; fica p/ quando houver captura.

        // --- Sombras ---------------------------------------------------------------------
        // Cada entrada seleciona uma fatia do mesmo array. A profundidade ortografica ja e
        // linear e pode ser mostrada sem decodificacao adicional.
        if (SunShadows.IsInitialized() && SunShadows.ShadowSRVSlot() != kNoSlot) {
            static constexpr const char* kCascadeNames[FSunShadows::kNumCascades] = {
                "Sombra do sol · cascata 0", "Sombra do sol · cascata 1",
                "Sombra do sol · cascata 2", "Sombra do sol · cascata 3" };
            for (u32 c = 0; c < FSunShadows::kNumCascades; ++c)
                Register(kCascadeNames[c], SunShadows.ShadowSRVSlot(),
                         EDebugDecode::ArraySlice, /*SubIndex=*/c);
        }

        // --- Iluminacao indireta ---------------------------------------------------------
        if (AO.AOSRVSlot() != kNoSlot)
            Register("GTAO", AO.AOSRVSlot(), EDebugDecode::Grayscale);
        // Entrada do GTAO, nao a normal do G-buffer: sai do Z-prepass (DepthNormal.ps) com a
        // normal INTERPOLADA do vertice, sem normal map — oclusao e sobre a forma da geometria.
        // Fica no grupo do GTAO porque nasceu como entrada dele. Hoje tambem e escrita quando o
        // visualizador do radiance cache esta selecionado (ver GeometricNormalWillRun no Render).
        // Fora desses dois casos o alvo congela no ultimo frame valido ou no clear neutro cinza.
        if (Targets.NormalSRVSlot != kNoSlot)
            Register("GTAO · normal geometrica", Targets.NormalSRVSlot, EDebugDecode::Raw);
        // Raw e NRD usam entradas fixas para o alvo nao mudar com o modo do denoiser.
        if (ReSTIRGI.GITexRawSRVSlot() != kNoSlot)
            Register("ReSTIR GI", ReSTIRGI.GITexRawSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.5f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (ReSTIRGI.NrdOutSRVSlot() != kNoSlot)
            Register("ReSTIR GI · NRD", ReSTIRGI.NrdOutSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.5f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (ReSTIRDI.OutputSRVSlot() != kNoSlot)
            Register("ReSTIR DI", ReSTIRDI.OutputSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        // O resolvido e a entrada especular crua do RR; preserve outliers sem filtro linear.
        if (Reflections.GetResolvedSRVSlot() != kNoSlot)
            Register("Reflexos · resolvido", Reflections.GetResolvedSRVSlot(), EDebugDecode::HDR,
                     0, 1, /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        // Guides mostram texels exatos, sem filtro. Com debug ativo o RR e pulado, portanto eles
        // representam o frame anterior e nao revelam artefatos criados pelo proprio eval.
        if (RRGuides.DiffuseAlbedoSRV() != kNoSlot) {
            Register("DLSS-RR · albedo difuso", RRGuides.DiffuseAlbedoSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            Register("DLSS-RR · albedo especular", RRGuides.SpecularAlbedoSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            // O guide armazena WorldNormal cru; roughness fica no alpha e e vista no G-buffer.
            Register("DLSS-RR · normal+rough", RRGuides.NormalRoughnessSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            // Heatmap distingue zero exato de hits proximos; 20 m satura a escala.
            Register("DLSS-RR · spec hitDist", RRGuides.SpecHitDistSRV(), EDebugDecode::Heatmap,
                     0, 1, /*Exposure=*/1.0f / 20.0f, 0, /*LinearFilter=*/false);
        }
        // Instrumentacao de timer (NVAPI). A Exposure e o 1/valor "quente" do artigo da NVIDIA:
        // escala FIXA de proposito — normalizar pelo maximo do frame faria a mesma cena mudar de
        // cor conforme a camera anda, e duas capturas deixariam de ser comparaveis.
        // Filtro linear OFF: cada texel e uma medida por thread, interpolar inventa custo.
        if (TimerGI.SrvSlot() != kNoSlot)
            Register("RT · timer · ReSTIR GI", TimerGI.SrvSlot(), EDebugDecode::Heatmap, 0, 1,
                     /*Exposure=*/kShaderTimerScaleGI, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (TimerReflections.SrvSlot() != kNoSlot)
            Register("RT · timer · reflexos", TimerReflections.SrvSlot(), EDebugDecode::Heatmap, 0, 1,
                     /*Exposure=*/kShaderTimerScaleReflections, /*AtlasTilePx=*/0,
                     /*LinearFilter=*/false);
        // Debug da BVH. Decode Raw porque o passe ja resolve a cor final (paleta de categoria,
        // rampa de falsa-cor) — nao ha canal cru p/ o visualizador interpretar, e um decode
        // generico so poderia estragar cor autorada. Filtro linear OFF: cada texel e um raio, e
        // interpolar borraria a fronteira entre duas instancias.
        if (BvhDebug.SrvSlot() != kNoSlot)
            Register("RT · BVH", BvhDebug.SrvSlot(), EDebugDecode::Raw, 0, 1,
                     /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (DDGI.IrradianceAtlasSRV() != kNoSlot) {
            // O atlas guarda irradiancia comprimida por gamma; o decode generico de HDR
            // deixava o sinal artificialmente claro e nao correspondia ao que o lighting usa.
            Register("DDGI · irradiancia", DDGI.IrradianceAtlasSRV(),
                     EDebugDecode::DDGIIrradiance, 0, 1,
                     /*Exposure=*/0.4f, /*AtlasTilePx=*/6 + 2);
            // R e a media em unidades de mundo, ja limitada no update a 2,6 * spacing.
            // MaxRayDistance e o alcance do trace da cena e pode ser dezenas de vezes maior.
            const f32 DistMax  = DDGI.DistanceMomentMax();
            const f32 DistNorm = DistMax > 0.0f ? 1.0f / DistMax : 1.0f;
            Register("DDGI · distancia",   DDGI.DistAtlasSRV(),       EDebugDecode::DDGIDistance, 0, 1,
                     /*Exposure=*/DistNorm, /*AtlasTilePx=*/14 + 2);
        }

        // "Upscaler · saida" NAO entra: o upscaler roda DEPOIS deste passe no frame, entao o
        // que se leria aqui e o resultado do frame ANTERIOR — dado enganoso. Mesmo caso do
        // "HDR color": so volta com um passe de captura por copia (e o que a Unreal faz).

        // --- Atmosfera / volumetrico -----------------------------------------------------
        // As tres LUTs 2D do Hillaire. A transmitancia e [0..1] por construcao (fracao de luz
        // que sobrevive ao caminho), entao entra CRUA: passar tonemap nela mentiria sobre o
        // valor. Multiscatter e sky-view sao radiancia — HDR, com exposicao propria porque o
        // sky-view vive na escala de kSunIlluminance (22) e estoura branco em 1.0.
        // O volume 3D do aerial perspective e o cubo de reflexo ficam de fora: precisam de
        // decode novo (fatia de volume / cruz de cubo) no DebugView.ps, que e outra rodada.
        if (Atmosphere.IsInitialized()) {
            Register("Atmosfera · transmitancia", Atmosphere.TransmittanceSRV(), EDebugDecode::Raw);
            Register("Atmosfera · multiscatter",  Atmosphere.MultiScatterSRV(),  EDebugDecode::HDR,
                     0, 1, /*Exposure=*/1.0f);
            Register("Atmosfera · sky-view",      Atmosphere.SkyViewSRV(),       EDebugDecode::HDR,
                     0, 1, /*Exposure=*/0.15f);
        }
        if (SunShafts.IsInitialized())
            Register("Sun shafts", SunShafts.VolumetricSRVSlot(), EDebugDecode::HDR, 0, 1, /*Exposure=*/2.0f);

        // --- passes que publicam os proprios alvos ---------------------------------------
        // As entradas acima sao a dedo porque leem estado do Renderer (Targets, GBuffer). Um
        // passe migrado para o contrato publica sozinho, e AQUI: depois do Clear, senao a
        // entrada nasce apagada; antes do remapeamento, senao a selecao nao o encontra.
        Passes.RegisterDebugTargetsAll();

        std::vector<u32> RemappedSelection;
        RemappedSelection.reserve(SelectedNames.size());
        for (const std::string& Name : SelectedNames) {
            const u32 Index = IndexOf(Name);
            if (Index != DebugTargets::kInvalid) RemappedSelection.push_back(Index);
        }
        SetDebugSelection(RemappedSelection);
        DebugTargetIndex = MainTargetName.empty() ? kNoDebugTarget : IndexOf(MainTargetName);
    }

    void Renderer::SetDebugProbeIndex(i32 _Index) {
        const u32 NewIndex = (_Index >= 0 && DDGI.IsReady() &&
                              static_cast<u32>(_Index) < DDGI.NumProbesCount())
                           ? static_cast<u32>(_Index) : kNoDebugProbe;
        if (DebugProbeIndex == NewIndex) return;

        DebugProbeIndex = NewIndex;
        DebugProbeSampleU = -1.0f;
        DebugProbeSampleV = -1.0f;
        DebugProbeSampleReady = false;
        const bool Active = DebugProbeIndex != kNoDebugProbe;
        DDGIDebugPass.SetSelectedProbe(Active ? static_cast<i32>(DebugProbeIndex) : -1);
        DDGIDebugPass.SetEnabled(Active);
        if (Active) {
            DDGIDebugPass.SetMode(FDDGIDebug::EMode::Selected);
            DDGIDebugPass.SetProbeRadius(0.14f);
            DDGIDebugPass.SetShowVolume(false);
            DDGIDebugPass.SetShowRays(false);
        }
        ++DebugPreviewConfigVersion;
    }

    void Renderer::SetDebugProbeSampleUV(f32 _U, f32 _V) {
        const bool Valid = DebugProbeIndex != kNoDebugProbe &&
                           _U >= 0.0f && _U <= 1.0f &&
                           _V >= 0.0f && _V <= 1.0f;
        const f32 U = Valid ? std::clamp(_U, 0.0f, 0.999999f) : -1.0f;
        const f32 V = Valid ? std::clamp(_V, 0.0f, 0.999999f) : -1.0f;
        if (DebugProbeSampleU == U && DebugProbeSampleV == V) return;
        DebugProbeSampleU = U;
        DebugProbeSampleV = V;
        DebugProbeSampleReady = false;
        ++DebugPreviewConfigVersion;
    }

    bool Renderer::RequestDebugProbePoint(u32 _X, u32 _Y) {
        if (DebugProbeIndex == kNoDebugProbe || !DDGI.IsReady()) return false;
        const u32 InternalX = static_cast<u32>(
            static_cast<f32>(_X) * RenderScale + 0.5f);
        const u32 InternalY = static_cast<u32>(
            static_cast<f32>(_Y) * RenderScale + 0.5f);
        return DDGIDebugPass.RequestPointDiagnostic(InternalX, InternalY);
    }

    void Renderer::CancelDebugProbePoint() {
        DDGIDebugPass.CancelPointDiagnostic();
    }

    void Renderer::RepeatDebugProbePoint() {
        if (DebugProbeIndex == kNoDebugProbe || !DDGI.IsReady()) return;
        DDGIDebugPass.RepeatPointDiagnostic();
    }

    bool Renderer::ConsumeDebugProbePoint(
            FDDGIPointDiagnostic& _OutDiagnostic) {
        return DDGIDebugPass.ConsumePointDiagnostic(_OutDiagnostic);
    }

    void Renderer::SetDebugProbeContributors(
            const FDDGIPointDiagnostic& _Diagnostic) {
        u32 Indices[FDDGIDebug::kPointProbeCount]{};
        f32 Weights[FDDGIDebug::kPointProbeCount]{};
        u32 Count = 0;
        i32 RiskSlot = -1;
        for (u32 I = 0; I < FDDGIDebug::kPointProbeCount; ++I) {
            const FDDGIPointProbeDiagnostic& P = _Diagnostic.Probes[I];
            if (!P.Active || P.ProbeIndex >= DDGI.NumProbesCount()) continue;
            if (static_cast<i32>(I) == _Diagnostic.RiskSlot)
                RiskSlot = static_cast<i32>(Count);
            Indices[Count] = P.ProbeIndex;
            Weights[Count] = P.NormalizedWeight;
            ++Count;
        }
        SetDebugProbeContributors(Indices, Weights, Count, RiskSlot);
    }

    void Renderer::SetDebugProbeContributors(
            const u32* _Indices, const f32* _Weights,
            u32 _Count, i32 _RiskSlot) {
        if (_Count > 0 && _Indices && _Weights) {
            DDGIDebugPass.SetSelectedProbes(
                _Indices, _Weights, _Count, _RiskSlot);
            DDGIDebugPass.SetEnabled(true);
            DDGIDebugPass.SetMode(FDDGIDebug::EMode::Selected);
        } else if (DebugProbeIndex != kNoDebugProbe) {
            DDGIDebugPass.SetSelectedProbe(
                static_cast<i32>(DebugProbeIndex));
        }
    }

    // Limpa contribuintes do point-pick sem encerrar a sessao da probe-base.
    void Renderer::ClearDebugProbeContributors() {
        DDGIDebugPass.SetSelectedProbe(DebugProbeIndex != kNoDebugProbe
                                       ? static_cast<i32>(DebugProbeIndex) : -1);
        ++DebugPreviewConfigVersion; // o painel refaz o preview do tile
    }

    // Retorna se ferramentas escreveram no HDR e tornaram a entrada invalida para o RR.
    bool Renderer::RecordDebugViews(FPassContext& _Ctx, bool _DDGIDebugDrew) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const bool DDGIDebugDrew       = _DDGIDebugDrew;

        // A grade de preview e composta em target offscreen e copiada para o QML.
        const auto& DbgAll = DebugTargets::All();
        const bool MainDebugActive = GBufferDebugMode > 0 || DebugTargetIndex < DbgAll.size();
        const bool PreviewActive   = DebugPreviewEnabled && !DebugSelection.empty();
        // A janela e uma ferramenta interativa: capturar so 1/3 dos frames fazia o jitter
        // temporal aparecer como tremedeira e deixava o movimento visivelmente defasado.
        const bool CapturePreview  = PreviewActive;

        // Debug fullscreen e probes nao combinam com os guides da cena. Pule o RR para nao
        // contaminar seu historico; a saida RR tambem nao possui RTV para mover o debug depois.
        const bool RRPoisoned = Modes.RRMode && (MainDebugActive || DDGIDebugDrew);

        if ((MainDebugActive || CapturePreview) &&
            GBuffer.IsInitialized() && DebugViewPass.IsInitialized()) {
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Timers e BVH saem de seus dispatches em UAV.
            TimerGI.ToRead(CommandList);
            TimerReflections.ToRead(CommandList);
            BvhDebug.ToRead(CommandList); // idem: sai do dispatch em UAV
            // TransitionForRR restaura os guides depois deste passe grafico.
            if (RRGuides.IsReady()) RRGuides.TransitionForDebug(CommandList);

            // Depth e normal podem ser escolhidos tanto no toolbar quanto na janela. Deixa-os
            // legiveis durante os dois draws e restaura exatamente os estados de entrada.
            const bool NeedPublishedInputs =
                CapturePreview || DebugTargetIndex < DbgAll.size();
            const D3D12_RESOURCE_STATES NormalBefore = Targets.NormalBufferState;
            if (NeedPublishedInputs) {
                FBarrierBatch InputBatch;
                InputBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                InputBatch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                InputBatch.Flush(CommandList);
            }

            if (CapturePreview && DebugPreviewPass.IsInitialized() && DebugPreviewTarget) {
                FDebugTile PreviewTiles[16]{};
                u32 PreviewTileCount = 0;
                for (u32 Idx : DebugSelection) {
                    if (PreviewTileCount >= 16u || Idx >= DbgAll.size()) continue;
                    const FDebugTarget& T = DbgAll[Idx];
                    FDebugTile& Tile   = PreviewTiles[PreviewTileCount++];
                    Tile.SrvSlot       = T.SrvSlot;
                    Tile.Decode        = T.Decode;
                    Tile.SubIndex      = T.SubIndex;
                    // Um tile isolado usa a escala da propria cascata; o overview usa a global.
                    f32 ExposureScale = 1.0f;
                    if (DebugProbeIndex != kNoDebugProbe && T.AtlasTilePx > 0 &&
                        T.Name.rfind("DDGI", 0) == 0) {
                        // Zero significa overview; +1 permite selecionar fisicamente o tile 0.
                        Tile.SubIndex = DDGI.AtlasTileFromProbe(DebugProbeIndex) + 1u;
                        if (T.Decode == EDebugDecode::DDGIDistance && DDGI.ProbesPerCascade() > 0) {
                            const u32 Casc = DebugProbeIndex / DDGI.ProbesPerCascade();
                            const f32 TileMax = DDGI.CascadeDistanceMomentMax(Casc);
                            const f32 GlobalMax = DDGI.DistanceMomentMax();
                            if (TileMax > 0.0f) ExposureScale = GlobalMax / TileMax;
                        }
                    }
                    Tile.AtlasTilePx   = T.AtlasTilePx;
                    Tile.Mip           = DebugMip < T.MipCount ? DebugMip : 0;
                    Tile.ChannelWeight = DebugChannelWeight;
                    Tile.Exposure      = T.Exposure * DebugExposure * ExposureScale;
                    Tile.NearZ         = Vw.NearZ;
                    Tile.FarZ          = Vw.FarZ;
                    Tile.LinearFilter  = T.LinearFilter;
                }

                auto PreviewRTV = DebugPreviewRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &PreviewRTV, FALSE, nullptr);
                const f32 Clear[4] = { 0.035f, 0.037f, 0.031f, 1.0f };
                CommandList->ClearRenderTargetView(PreviewRTV, Clear, 0, nullptr);

                D3D12_VIEWPORT PreviewViewport{
                    0.0f, 0.0f,
                    static_cast<f32>(kDebugPreviewWidth),
                    static_cast<f32>(kDebugPreviewHeight),
                    0.0f, 1.0f
                };
                DebugPreviewPass.Execute(
                    CommandList, Backend->SRVHeap, PreviewTiles, PreviewTileCount, DebugColumns,
                    GBuffer.SRVTableStart(), Targets.VelocitySRVSlot, PreviewViewport,
                    Vw.JitterUv, /*EncodeForDisplay=*/true);

                D3D12_RESOURCE_BARRIER ToCopy{};
                ToCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                ToCopy.Transition.pResource   = DebugPreviewTarget.Get();
                ToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                CommandList->ResourceBarrier(1, &ToCopy);

                D3D12_TEXTURE_COPY_LOCATION Src{};
                Src.pResource        = DebugPreviewTarget.Get();
                Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Src.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION Dst{};
                Dst.pResource = DebugPreviewReadback[FrameSlot].Get();
                Dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                Dst.PlacedFootprint.Offset             = 0;
                Dst.PlacedFootprint.Footprint.Format   = kDebugPreviewFormat;
                Dst.PlacedFootprint.Footprint.Width    = kDebugPreviewWidth;
                Dst.PlacedFootprint.Footprint.Height   = kDebugPreviewHeight;
                Dst.PlacedFootprint.Footprint.Depth    = 1;
                Dst.PlacedFootprint.Footprint.RowPitch = kDebugPreviewRowPitch;
                CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

                std::swap(ToCopy.Transition.StateBefore, ToCopy.Transition.StateAfter);
                CommandList->ResourceBarrier(1, &ToCopy);
                DebugPreviewReadbackPending[FrameSlot] = true;
                DebugPreviewReadbackVersion[FrameSlot] = DebugPreviewConfigVersion;
                DebugPreviewLastCapturedVersion = DebugPreviewConfigVersion;

                if (DebugProbeIndex != kNoDebugProbe && DDGI.IsReady() &&
                    DebugProbeSampleU >= 0.0f && DebugProbeSampleV >= 0.0f) {
                    ID3D12Resource* IrrResource  = DDGI.IrradianceAtlasResource();
                    ID3D12Resource* DistResource = DDGI.DistanceAtlasResource();
                    constexpr D3D12_RESOURCE_STATES AtlasRead =
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    D3D12_RESOURCE_BARRIER AtlasBarriers[2]{};
                    ID3D12Resource* Resources[2] = { IrrResource, DistResource };
                    for (u32 I = 0; I < 2; ++I) {
                        AtlasBarriers[I].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        AtlasBarriers[I].Transition.pResource = Resources[I];
                        AtlasBarriers[I].Transition.Subresource =
                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        AtlasBarriers[I].Transition.StateBefore = AtlasRead;
                        AtlasBarriers[I].Transition.StateAfter =
                            D3D12_RESOURCE_STATE_COPY_SOURCE;
                    }
                    CommandList->ResourceBarrier(2, AtlasBarriers);

                    // O empacotamento 2D do atlas define a quantidade de tiles por linha.
                    const u32 TilesX = DDGI.AtlasTilesPerRow();
                    const u32 AtlasTile = DDGI.AtlasTileFromProbe(DebugProbeIndex);
                    const u32 TileX = AtlasTile % std::max(TilesX, 1u);
                    const u32 TileY = AtlasTile / std::max(TilesX, 1u);
                    const u32 IrrLocalX = std::min(
                        static_cast<u32>(DebugProbeSampleU * FDDGI::kTileSize),
                        static_cast<u32>(FDDGI::kTileSize - 1));
                    const u32 IrrLocalY = std::min(
                        static_cast<u32>(DebugProbeSampleV * FDDGI::kTileSize),
                        static_cast<u32>(FDDGI::kTileSize - 1));
                    const u32 DistLocalX = std::min(
                        static_cast<u32>(DebugProbeSampleU * FDDGI::kDistTileSize),
                        static_cast<u32>(FDDGI::kDistTileSize - 1));
                    const u32 DistLocalY = std::min(
                        static_cast<u32>(DebugProbeSampleV * FDDGI::kDistTileSize),
                        static_cast<u32>(FDDGI::kDistTileSize - 1));

                    auto CopyTexel = [&](ID3D12Resource* Resource, DXGI_FORMAT Format,
                                         u32 X, u32 Y, u64 Offset) {
                        D3D12_TEXTURE_COPY_LOCATION SrcLoc{};
                        SrcLoc.pResource = Resource;
                        SrcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        SrcLoc.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION DstLoc{};
                        DstLoc.pResource = DebugProbeSampleReadback[FrameSlot].Get();
                        DstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        DstLoc.PlacedFootprint.Offset = Offset;
                        DstLoc.PlacedFootprint.Footprint.Format = Format;
                        DstLoc.PlacedFootprint.Footprint.Width = 1;
                        DstLoc.PlacedFootprint.Footprint.Height = 1;
                        DstLoc.PlacedFootprint.Footprint.Depth = 1;
                        DstLoc.PlacedFootprint.Footprint.RowPitch =
                            D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                        const D3D12_BOX Box{ X, Y, 0, X + 1u, Y + 1u, 1 };
                        CommandList->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLoc, &Box);
                    };
                    CopyTexel(
                        IrrResource, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        TileX * static_cast<u32>(FDDGI::kTileSize + 2) + 1u + IrrLocalX,
                        TileY * static_cast<u32>(FDDGI::kTileSize + 2) + 1u + IrrLocalY,
                        kDebugProbeIrrOffset);
                    CopyTexel(
                        DistResource, DXGI_FORMAT_R16G16_FLOAT,
                        TileX * static_cast<u32>(FDDGI::kDistTileSize + 2) + 1u + DistLocalX,
                        TileY * static_cast<u32>(FDDGI::kDistTileSize + 2) + 1u + DistLocalY,
                        kDebugProbeDistOffset);

                    for (D3D12_RESOURCE_BARRIER& Barrier : AtlasBarriers)
                        std::swap(Barrier.Transition.StateBefore,
                                  Barrier.Transition.StateAfter);
                    CommandList->ResourceBarrier(2, AtlasBarriers);
                    DebugProbeSamplePending[FrameSlot] = true;
                    DebugProbeSampleVersion[FrameSlot] = DebugPreviewConfigVersion;
                    DebugProbeSampleIndex[FrameSlot]   = DebugProbeIndex;
                }
            }

            if (MainDebugActive) {
                auto HDRDbgRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &HDRDbgRTV, FALSE, nullptr);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);

                FDebugTile Tile{};
                if (DebugTargetIndex < DbgAll.size()) {
                    const FDebugTarget& T = DbgAll[DebugTargetIndex];
                    Tile.SrvSlot       = T.SrvSlot;
                    Tile.Decode        = T.Decode;
                    Tile.SubIndex      = T.SubIndex;
                    Tile.Mip           = DebugMip < T.MipCount ? DebugMip : 0;
                    Tile.AtlasTilePx   = T.AtlasTilePx;
                    Tile.ChannelWeight = DebugChannelWeight;
                    Tile.Exposure      = T.Exposure * DebugExposure;
                    Tile.NearZ         = Vw.NearZ;
                    Tile.FarZ          = Vw.FarZ;
                    Tile.LinearFilter  = T.LinearFilter;
                } else if (GBufferDebugMode == 8) {
                    Tile.SrvSlot = Targets.VelocitySRVSlot;
                    Tile.Decode  = EDebugDecode::Velocity;
                } else {
                    Tile.SrvSlot  = GBuffer.SRVTableStart();
                    Tile.Decode   = EDebugDecode::GBufferField;
                    Tile.SubIndex = GBufferDebugMode;
                }
                DebugViewPass.Execute(CommandList, Backend->SRVHeap, &Tile, 1, 1,
                                      GBuffer.SRVTableStart(), Targets.VelocitySRVSlot, Viewport,
                                      Vec2{ 0.0f, 0.0f }, /*EncodeForDisplay=*/false);
            }

            if (NeedPublishedInputs) {
                FBarrierBatch RestoreBatch;
                RestoreBatch.Transition(
                    Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
                RestoreBatch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                               NormalBefore);
                RestoreBatch.Flush(CommandList);
            }
            GBuffer.TransitionToWrite(CommandList);
        }
        return RRPoisoned;
    }

}
