#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/RayTracing/RayEpsilons.h"
#include "Smile/Graphics/GI/GIHitSampling.h"
#include "Smile/Graphics/GI/DDGI.h" // FDDGICascadeConstants
#include "Smile/Graphics/GI/GIFallback.h"
#include "Smile/Graphics/GI/ReGIR.h"
#include "Smile/Graphics/GI/RadianceCache.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace Smile {
    class FGpuProfiler;
    class FTextureSRVHeap;

    // Espelha o cbuffer ReSTIRCB dos shaders.
    struct alignas(256) ReSTIRGIConstants {
        Mat44 InvViewProj;     // inversa FULL da view-proj (row-major)
        Vec4  CameraPos;       // xyz = camera mundo
        Vec4  ScreenParams;    // W, H, 1/W, 1/H
        Vec4  GridMinSpacing;  // DDGI: xyz = origem do grid, w = espacamento
        Vec4  GridCount;       // DDGI: xyz = nº de probes por eixo
        Vec4  AtlasParams;     // DDGI irradiancia: x = tile, y = W, z = H
        Vec4  SunDirIntensity; // xyz = direcao P/ o sol (norm.), w = intensidade
        Vec4  SunColor;        // rgb = cor do sol, w = ShadowRayMask (mask dos shadow rays no hit)
        Vec4  TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                               // (so sombras no hit; origem de raio do G-buffer usa offset robusto)
        Vec4  ShadeParams;     // x = livre, y = albedoLOD, z = fireflyMax, w = validateInterval
        Vec4  ReuseParams;     // x = MCap, y = posRejectScale, z = visibility (0/1), w = temporal (0/1)
        Vec4  SpatialParams;   // x = radius(px), y = count, z = spatial (0/1), w = normalReject
        Vec4  JitterParams;    // xy = prevJitterUv - currJitterUv (reprojecao temporal no espaco jittered)
        Mat44 View;            // anexado p/ o pack do NRD (worldPos -> view.z = IN_VIEWZ)
        // Campos anexados preservam os offsets existentes do cbuffer.
        Vec4  RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4  RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        Vec4  PolicyParams;      // x = politica de backface no gather (0/1),
                                 // y = strength do boiling filter (0 = off),
                                 // z = correcao de vies do temporal (0/1),
                                 // w = kill de backface no Jacobiano (0 = abs() historico)
        // Gather do segundo bounce (HitShading.hlsli).
        Vec4  GIDistParams;      // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4  GIBiasParams;      // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4  ReGIRGridMinSlots;
        Vec4  ReGIRInvCellEnabled;
        Vec4  ReGIRGridCountSamples;
        Vec4  ReGIRResources;
        // Sky-view LUT usado por HitShading.hlsli.
        Vec4  SkyParams;         // x = view height (km), y = raio do planeta (km), zw = livres
        Vec4  DebugParams;       // x = slot bindless do alvo de timer (< 0 = captura off)
        // Histórico de superfície anterior e validade da reconstrução.
        Vec4  HistoryParams;     // x = slot bindless do Surface do frame ANTERIOR
        // Espelha FRadianceCacheShaderParams.
        Vec4  RadianceCacheCamCell;
        Vec4  RadianceCacheLodCapFlags;
        Vec4  RadianceCacheResources;
        // Cascatas do DDGI para o segundo bounce.
        FDDGICascadeConstants GICascades;
        // ScreenParams é full-res; estes campos descrevem o domínio interno.
        Vec4 TraceScreenParams; // W, H, 1/W, 1/H internos
        Vec4 ResolutionParams;  // x=escala (1/2), yz=fase 2x2, w=half-res efetivo
    };
    static_assert(offsetof(ReSTIRGIConstants, GICascades) == 560,
                  "bloco de cascatas anexado ao fim do cbuffer (ver Renderer.h)");
    static_assert(offsetof(ReSTIRGIConstants, ReGIRGridMinSlots) == 400,
                  "ReSTIRGIConstants divergiu do cbuffer ReSTIRCB");
    static_assert(offsetof(ReSTIRGIConstants, SkyParams) == 464,
                  "SkyParams deve permanecer anexado ao fim do ReSTIRCB");
    static_assert(offsetof(ReSTIRGIConstants, TraceScreenParams) == 704,
                  "resolucao interna deve permanecer anexada depois das cascatas");

    // Final gather difuso: trace/temporal, reuso espacial e resolve.
    // Reservoir {x2,n2,Lo,M,W} em duas texturas ping-pong.
    class FReSTIRGI : public FRenderPass {
    public:
        // Classifica a origem do candidato novo, antes dos reusos.
        static constexpr const char* kSourceDebugTargetName = "GI · fonte do candidato tracado";
        void SetSourceDebug(bool V) { SourceDebug = V; }
        bool GetSourceDebug() const { return SourceDebug; }
        void OnRegisterDebugTargets() override;

        const char* Name() const override { return "ReSTIR GI"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::ReSTIRGI; }
        void OnInvalidateHistory(EHistoryTarget) override { InvalidateHistory(); }

        void Initialize(ID3D12Device* Device);

        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        // Fallback sem DDGI usa descriptors neutros, nunca inválidos.
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot,
                            const FGIFallbackBindings& Fallback,
                            u32 DepthSlot, u32 GBufferSlot, u32 VelocitySlot);

        // PrevSurfaceSlot reconstrói x1 temporal; kInvalidSlot desliga o reuso neste frame.
        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity,
                            const Mat44& View, const Vec2& JitterDeltaUv,
                            u32 PrevSurfaceSlot,
                            u32 PunctualLightCount = 0);

        // Atualiza t13 na tabela versionada da paridade corrente.
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot);

        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                         FGpuProfiler* Profiler = nullptr);

        // kInvalidSlot seleciona a PSO normal, sem instrumentação.
        void SetTimerSlot(u32 Slot) { TimerSlot = Slot; }
        bool HasTimerPipeline() const { return TraceTimed; }

        // Cria as tabelas de pack depois do setup do passe e do NRD.
        void SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          ID3D12Resource* NrdInViewZ, ID3D12Resource* NrdInNormalRough,
                          ID3D12Resource* NrdInMv, ID3D12Resource* NrdInDiffRadHit,
                          ID3D12Resource* NrdOut);
        // O caller prepara estados UAV/SRV dos recursos do pack.
        void RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        void SetUseNrd(bool V) { UseNrd = V; }
        bool GetUseNrd() const { return UseNrd; }

        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        // Atualizada por frame porque a cascata fina segue a câmera.
        void SetGICascades(const FDDGICascadeConstants& C) { GICascadesCPU = C; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        void SetRadianceCacheParams(const FRadianceCacheShaderParams& P) { RadianceCacheParams = P; }
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }

        bool IsReady() const   { return Ready; }
        void SetHalfRes(bool V) { HalfRes = V; }
        bool GetHalfRes() const { return HalfRes; }
        bool HalfResEffective() const { return Ready && HalfRes && Width < FullWidth; }
        u32 TraceWidth() const { return Width; }
        u32 TraceHeight() const { return Height; }
        bool IsNrdOutput() const  { return UseNrd && NrdOutSRV != kInvalidSlot; }
        // SRV vigente do deferred.
        u32  GITexSRVSlot() const { return IsNrdOutput() ? NrdOutSRV : GITexSRV; }
        u32  GITexRawSRVSlot() const { return GITexSRV; }
        u32  NrdOutSRVSlot() const   { return NrdOutSRV; }

        // Limpa reservoirs e reconstrução no próximo frame.
        void InvalidateHistory()   { NeedsClear = true; ReconstructionHistoryValid = false; }

        void SetTemporal(bool V)   { if (V && !Temporal) InvalidateHistory(); Temporal = V; }
        bool GetTemporal() const   { return Temporal; }
        void SetFoliageShadows(bool V) { if (V != FoliageShadows) InvalidateHistory();
                                         FoliageShadows = V; }

        // Retrace de auto-interseção e término preto em verso one-sided.
        void SetBackfacePolicy(bool V) { if (V != BackfacePolicy) InvalidateHistory();
                                         BackfacePolicy = V; }
        bool GetBackfacePolicy() const { return BackfacePolicy; }
        bool GetFoliageShadows() const { return FoliageShadows; }
        // O passe espacial não realimenta o histórico temporal.
        void SetSpatial(bool V)    { Spatial = V; }
        bool GetSpatial() const    { return Spatial; }
        void SetVisibility(bool V) { Visibility = V; }
        bool GetVisibility() const { return Visibility; }
        void SetBoilingStrength(f32 V) { BoilingStrength = V; }
        f32  GetBoilingStrength() const { return BoilingStrength; }
        // Alteram pesos/aceitação já gravados e exigem histórico novo.
        void SetTemporalBiasCorrection(bool V) {
            if (V != TemporalBiasCorr) InvalidateHistory();
            TemporalBiasCorr = V;
        }
        bool GetTemporalBiasCorrection() const { return TemporalBiasCorr; }
        void SetJacobianKillBackface(bool V) {
            if (V != JacobianKillBackface) InvalidateHistory();
            JacobianKillBackface = V;
        }
        bool GetJacobianKillBackface() const { return JacobianKillBackface; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FComputePipeline TracePSO;   // 14 SRV, 3 UAV, heap-directly-indexed (Pass A)
        // Variante instrumentada; a PSO normal não paga o custo do timer.
        FComputePipeline TracePSOTimed;
        FComputePipeline SpatialPSO; // 10 SRV, 1 UAV, heap-directly-indexed (Pass B; alpha-test M6)
        // 5 SRVs e 1 UAV temporário.
        FComputePipeline UpsamplePSO;
        FComputePipeline NrdPackPSO; // 4 SRV [GITex,gbuf,depth,vel], 4 UAV [NRD IN] (Fase C)

        // GITexture permanece full-res e com descriptor estável; o resolve usa alvo separado.
        Microsoft::WRL::ComPtr<ID3D12Resource> GITexture;
        D3D12_RESOURCE_STATES GITextureState = D3D12_RESOURCE_STATE_COMMON;
        Microsoft::WRL::ComPtr<ID3D12Resource> TraceGITexture;
        D3D12_RESOURCE_STATES TraceGITextureState = D3D12_RESOURCE_STATE_COMMON;
        Microsoft::WRL::ComPtr<ID3D12Resource> ResolvedGITexture;
        D3D12_RESOURCE_STATES ResolvedGITextureState = D3D12_RESOURCE_STATE_COMMON;
        // Reservoir ping-pong: 32 B/pixel.
        //   Res0 = RGBA32F      [x2.xyz, W]
        //   Res1 = RGBA32_UINT  [n2 oct | Lo RGB9E5 | M+idade | n1 oct]
        // x1 vem do depth/histórico de superfície.
        Microsoft::WRL::ComPtr<ID3D12Resource> Res0[2], Res1[2];
        D3D12_RESOURCE_STATES Res0State[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES Res1State[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        // Fonte do candidato novo, antes dos reusos.
        Microsoft::WRL::ComPtr<ID3D12Resource> SourceDebugTex;
        u32 SourceDebugSRV = 0xFFFFFFFFu, SourceDebugUAV = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES SourceDebugState = D3D12_RESOURCE_STATE_COMMON;
        bool SourceDebug = false;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 GITexSRV   = kInvalidSlot;
        u32 GITexUAV   = kInvalidSlot;
        u32 TraceGITexSRV = kInvalidSlot;
        u32 TraceGITexUAV = kInvalidSlot;
        u32 ResolvedGITexUAV = kInvalidSlot;
        u32 Res0SRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res1SRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res0UAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res1UAV[2] = { kInvalidSlot, kInvalidSlot };
        // Tabelas seguem a paridade dos reservoirs; t11/t12 preservam o t13 das luzes.
        // TraceTable: 14 SRVs; TraceUAVTable: 3 UAVs; SpatialTable: 10 SRVs.
        // TraceUAVTable[p] = 3 UAVs  [GITex, curr0, curr1].
        // SpatialTable[p]  = 10 SRVs [TLAS, curr0, curr1, filler, filler, gbuf, depth, inst×3].
        static constexpr u32 kParityTables = 2;
        u32 TraceTable[kParityTables]    = { kInvalidSlot, kInvalidSlot };
        u32 TraceUAVTable[kParityTables] = { kInvalidSlot, kInvalidSlot };
        u32 SpatialTable[kParityTables]  = { kInvalidSlot, kInvalidSlot };
        u32 UpsampleTable                = kInvalidSlot; // 5 SRVs; ver UpsamplePSO

        // NRD pack: [GI, gbuffer, depth, velocity] -> [viewZ, NR, MV, radiance/hit].
        u32 PackSrvTable = kInvalidSlot;
        u32 PackUavTable = kInvalidSlot;
        u32 NrdOutSRV    = kInvalidSlot; // SRV da OUT do NRD (tabela t16 quando UseNrd)
        u32 DepthSlot = kInvalidSlot, GBufferSlot = kInvalidSlot, VelocitySlot = kInvalidSlot;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        ReSTIRGIConstants CPU{};

        Vec4 GIGridMinSpacing{ 0, 0, 0, 1 };
        Vec4 GIGridCount{ 0, 0, 0, 0 };
        Vec4 GIAtlasParams{ 6, 1, 1, 0 };
        FDDGICascadeConstants GICascadesCPU{};
        f32  GIMaxRayDist = 0.0f;

        u32  FullWidth = 0, FullHeight = 0;
        u32  Width = 0, Height = 0; // dominio interno do trace/reservoir
        u32  FrameParity = 0;
        u32  TimerSlot   = kInvalidSlot; // alvo de timer vigente (kInvalidSlot = captura off)
        bool TraceTimed  = false;        // permutacao instrumentada criada com sucesso
        bool NeedsClear  = false;
        bool ReconstructionHistoryValid = false;
        bool Initialized = false;
        bool Ready       = false;
        bool HalfRes     = false; // experimental; default OFF

        f32  AlbedoLOD      = 2.0f;
        bool Temporal       = true;
        bool Spatial        = true;   // reuso espacial (off = só temporal = A2)
        bool UseNrd         = false;  // denoise via NRD RELAX (Fase C); off = ReSTIR cru no deferred
        bool FoliageShadows = true;   // folhagem nos shadow rays do hit (mask GATHER vs OPAQUE)
        bool BackfacePolicy = false;  // retrace + terminacao preta no verso one-sided (ver setter)
        bool Visibility     = false;  // visibility rays no espacial: shading visibility (1 raio) +
                                      // visibilidade nos pesos MIS (até K raios)
        f32  MCap           = 20.0f;
        f32  BoilingStrength = 0.0f; // 0 desliga
        bool TemporalBiasCorr = false;
        bool JacobianKillBackface = true;
        f32  PosRejectScale = 0.01f;
        f32  ValidateInterval = 0.0f; // intervalo de re-shade; 0 desliga
        f32  FireflyMax     = 8.0f;   // caminho NRD; 0 desliga
        f32  FireflyMaxRaw  = 4.0f;   // GI cru; 0 desliga
        f32  SpatialRadius  = 16.0f;  // raio (px) dos vizinhos
        f32  SpatialCount   = 4.0f;   // nº de vizinhos
        f32  NormalReject   = 0.9f;   // dot(n_q, n_r) minimo

        FRayEpsilonProfile RayEps;
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        FRadianceCacheShaderParams RadianceCacheParams{};
        Vec4               SkyLutParams{};
    };
}
