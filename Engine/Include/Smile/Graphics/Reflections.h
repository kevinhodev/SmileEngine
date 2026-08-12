#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/GIHitSampling.h"
#include "Smile/Graphics/DDGI.h" // FDDGICascadeConstants
#include "Smile/Graphics/ReGIR.h"
#include "Smile/Graphics/RadianceCache.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace Smile {
    class FGpuProfiler;
    class FTextureSRVHeap;

    // Constantes do Specular GI (b0 do trace E do composite). O prefixo (InvViewProj, CameraPos,
    // ScreenParams, ReflectParams) e IDENTICO ao CompositeCB -> os dois passes compartilham o
    // MESMO buffer (o composite le so o prefixo). Bate campo-a-campo com ReflectionCB
    // (ReflectionTrace.cs.hlsl) e CompositeCB (ReflectionComposite.ps.hlsl).
    struct alignas(256) ReflectionConstants {
        Mat44 InvViewProj;       // FULL inverse view-proj (row-major)
        Vec4  CameraPos;         // xyz = camera world
        Vec4  ScreenParams;      // W, H, 1/W, 1/H
        Vec4  ReflectParams;     // x=maxRoughnessToTrace, y=roughnessFadeLength, z=livre, w=albedoLOD
        Vec4  GridMinSpacing;    // DDGI grid origin + spacing
        Vec4  GridCount;         // DDGI probe counts
        Vec4  AtlasParams;       // DDGI irradiance atlas (tile, W, H)
        Vec4  SunDirIntensity;   // xyz = dir TO sun, w = intensity
        Vec4  SunColor;          // rgb = sun color, w = ShadowRayMask (mask dos shadow rays no hit)
        Vec4  TraceParams;       // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
                                 // (so sombras no hit; origem do raio usa offset robusto)
        Vec4  HalfScreenParams;  // halfW, halfH, 1/halfW, 1/halfH (trace e half-res; Fase 2b)
        Mat44 PrevViewProj;      // VP (sem jitter) do frame anterior — reprojeção do temporal (Fase 3)
        Vec4  PrevCameraPos;     // xyz = camera anterior; w = numero de transforms temporais
        Vec4  TemporalParams;    // x=maxFramesAccumulated, y=neighborhoodClampScale(γ), z=spatialRadius, w=mirrorMaxRoughness
        Vec4  DebugParams;       // x = modo de debug do reflexo (0=off, 1=acumulacao, 2=mascara espelho)
        Mat44 View;              // worldPos -> view.z (IN_VIEWZ)
        // Perfil de epsilons (FRayEpsilonProfile), anexado no FIM p/ nao deslocar offsets.
        Vec4  RayEpsA;           // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4  RayEpsB;           // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        Vec4  PolicyParams;      // x = cullar backface nos raios de reflexao (0/1); yzw livres
        // Gather do 2o bounce no hit (contrato do HitShading.hlsli): o mesmo sampler completo
        // do deferred, com Chebyshev e skip de sonda inativa.
        Vec4  GIDistParams;      // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4  GIBiasParams;      // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4  ReGIRGridMinSlots;
        Vec4  ReGIRInvCellEnabled;
        Vec4  ReGIRGridCountSamples;
        Vec4  ReGIRResources;
        Mat44 ViewProj;          // current world -> clip; water contact SSR
        Vec4  WaterEnvironmentParams; // x=atmosphere, y=intensity, z=env max mip, w=scene max mip
        // Parameterizacao do sky-view LUT p/ o ShadeSky do HitShading.hlsli, vinda do
        // FAtmosphere (fonte unica). Anexado no FIM p/ nao deslocar offset nenhum — os shaders
        // de trace que nao usam a cauda da agua declaram ViewProj/WaterEnvironmentParams como
        // preenchimento p/ alcancar este offset (convencao ja existente nesses arquivos).
        Vec4  SkyParams;         // x = view height (km), y = raio do planeta (km), zw = livres
        // World radiance cache (FRadianceCacheShaderParams). Contrato por NOME com o
        // RC_UNPACK_PARAMS do RadianceCache.hlsli; anexado no FIM como o SkyParams.
        Vec4  RadianceCacheCamCell;
        Vec4  RadianceCacheLodCapFlags;
        Vec4  RadianceCacheResources;
        // Cascatas do DDGI, para o gather do 2o bounce no HitShading. Mesmo bloco dos outros
        // quatro cbuffers; o GIGridMinSpacing continua sendo a GROSSA (peso do volume).
        FDDGICascadeConstants GICascades;
    };
    static_assert(offsetof(ReflectionConstants, GICascades) == 688,
                  "bloco de cascatas anexado ao fim do cbuffer (ver Renderer.h)");
    static_assert(offsetof(ReflectionConstants, ReGIRGridMinSlots) == 480,
                  "ReflectionConstants divergiu do cbuffer ReflectionCB");
    static_assert(offsetof(ReflectionConstants, ViewProj) == 544,
                  "ViewProj deve permanecer anexado ao fim do ReflectionCB");
    static_assert(offsetof(ReflectionConstants, WaterEnvironmentParams) == 608,
                  "WaterEnvironmentParams divergiu do ReflectionCB da agua");
    static_assert(offsetof(ReflectionConstants, SkyParams) == 624,
                  "SkyParams deve permanecer anexado ao fim do ReflectionCB");

    // Specular GI — reflexoes ray-traced (DXR inline), esqueleto estilo Lumen Reflections.
    // Fase 1: mirror, full-res, sem denoise. Passe de compute (trace, sombreado pelo MESMO
    // HitShading.hlsli do DDGI) -> composite aditivo no HDR. Reusa a geometria/atlas do DDGI.
    // No-op sem DXR/DDGI pronto. So roda sem MSAA (G-buffer single-sample).
    class FReflections : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Reflexos"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // Cria a pipeline de trace (compute) + a pipeline de composite (grafica) + o CB. 1x.
        void Initialize(ID3D12Device* Device);
        void RecreatePipelines(ID3D12Device* Device);

        // (Re)cria a textura de saida (radiancia) no tamanho da tela e (re)monta as tabelas de
        // descritores. Chamar no setup da cena e em TODO resize (depth/gbuffer recriados). Os
        // slots vem do DDGI (geometria/atlas), RaytracingScene (TLAS), Atmosphere (skyview) e
        // Renderer (depth, gbuffer, BRDF LUT).
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot, u32 IrradSlot,
                            u32 DepthSlot, u32 GBufferSlot, u32 GBufferCSlot, u32 BRDFLutSlot,
                            u32 GBufferASlot, u32 VelocitySlot,
                            u32 SceneColorSlot, u32 SceneDepthSlot, u32 SceneColorMipCount,
                            u32 AtmosphereSpecularSlot, u32 HDRSpecularSlot,
                            // t4/t5 do trace: atlas de distancia e ProbeData do DDGI — o 2o
                            // bounce usa o gather completo (Chebyshev + skip), nao a trilinear.
                            u32 DistSlot, u32 ProbeDataSlot,
                            const u32 TransformSlots[FCommandQueue::kFramesInFlight],
                            const u32 SurfaceSlots[FCommandQueue::kFramesInFlight]);

        // Params estaticos do volume DDGI (grid/atlas) p/ o CB. Chamar quando o volume e (re)criado.
        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Mat44& ViewProj,
                            const Mat44& PrevViewProj,
                            const Vec3& CameraPos, const Vec3& PrevCameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir,
                            f32 SunIntensity, const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity,
                            const Mat44& View, bool UseAtmosphereSky,
                            f32 WaterEnvironmentIntensity,
                            u32 PunctualLightCount, u32 TemporalInstanceCount,
                            bool MotionHistoryValid);

        // F5: copia o SRV do buffer de luzes puntuais do frame pro t8 da tabela de trace DO
        // FrameSlot (compartilhada pelo trace glossy e pelo mirror; versionada por frame em voo).
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot, u32 FrameSlot);

        // Grava o trace (compute) -> radiancia (UAV). Caller ja setou os descriptor heaps e
        // transicionou depth/gbuffer p/ legiveis por shader. Deixa a radiancia legivel por PS.
        // Com UseNrd: para no Resolved (Trace->Resolve->Mirror) e o NRD faz o denoise; sem UseNrd:
        // segue o denoiser caseiro (Resolve->Temporal->Spatial).
        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                         FGpuProfiler* Profiler = nullptr);

        // Segundo passe, executado DEPOIS que a agua escreveu depth/G-buffer/velocity. Traca apenas
        // pixels SMILE_SHADINGMODEL_WATER e acumula em historico proprio, sem sobrescrever o sinal
        // especular dos opacos usado pelo NRD/RR.
        void RecordWaterTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                              FGpuProfiler* Profiler = nullptr);
        void RecordWaterComposite(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                  D3D12_CPU_DESCRIPTOR_HANDLE HdrRtv, u32 Width, u32 Height);
        u32 GetWaterSpecHitTable() const { return WaterSpecHitTable; }

        // NRD (unificado): cria o UAV da IN_SPEC (pack escreve) + SRV da OUT_SPEC (composite le) e a
        // tabela do composite-NRD. Chamar apos SetupForResize e Nrd.SetupForResize.
        void SetupNrdSpec(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          ID3D12Resource* NrdInSpec, ID3D12Resource* NrdOutSpec);
        // Empacota o especular (Resolved -> IN_SPEC do NRD). Caller transicionou a IN_SPEC p/ UAV
        // (Nrd.TransitionInputsToWrite) e o Resolved ja esta NON_PIXEL (fim do RecordTrace c/ UseNrd).
        void RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Sem reflexoes ativas: escreve sinal especular ZERO valido na IN_SPEC. O RELAX combinado
        // (DIFFUSE_SPECULAR) le a IN_SPEC todo frame; textura recem-criada nunca escrita = conteudo
        // indefinido entrando no historico. Caller ja transicionou p/ UAV (TransitionInputsToWrite).
        void RecordNrdSpecZero(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Instrumentacao de timer no trace half-res (ver FShaderTimer): kInvalidSlot desliga e o
        // passe volta p/ a PSO normal. Dono = Renderer, empurra todo frame.
        void SetTimerSlot(u32 Slot) { TimerSlot = Slot; }
        bool HasTimerPipeline() const { return TraceTimed; }
        // O alvo de timer deste passe acompanha o dominio do dispatch, que e HALF-res.
        u32  TraceWidth() const  { return HalfWidth; }
        u32  TraceHeight() const { return HalfHeight; }

        // Perfil compartilhado de epsilons (dono = Renderer, empurra todo frame).
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        // Gather do 2o bounce (dono = Renderer, empurra todo frame; ver FGIHitSampling).
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        // Cascatas: empurradas por FRAME (nao no SetGIParams, que so roda em setup/resize) porque
        // a origem da cascata fina segue a camera. Dono = Renderer, fonte = FDDGI.
        void SetGICascades(const FDDGICascadeConstants& C) { GICascadesCPU = C; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        // SEM o bit de update — o Renderer publica ShaderParams(false) aqui. A radiancia que este
        // passe produz vale para UMA direcao de espelho e o cache nao guarda direcao; grava-la
        // envenenaria as celulas para o DDGI e o ReSTIR GI. Consultar continua valido.
        void SetRadianceCacheParams(const FRadianceCacheShaderParams& P) { RadianceCacheParams = P; }
        // Parameterizacao do sky-view LUT p/ o ShadeSky dos raios que escapam (dono = Renderer,
        // empurra todo frame a partir do FAtmosphere — fonte unica, ver Atmosphere.h).
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }
        // Limpa o historico temporal PROPRIO (caminho legado, sem NRD) no proximo RecordTrace.
        void InvalidateHistory()  { NeedsHistoryClear = true; NeedsWaterHistoryClear = true; }

        void SetUseNrd(bool V) { UseNrd = V; }
        bool GetUseNrd() const { return UseNrd; }

        // Modo "cru" p/ o DLSS Ray Reconstruction: como no UseNrd, o RecordTrace PARA no Resolved (sem
        // Temporal/Spatial nem NRD), mas o RecordComposite compoe o Resolved DIRETO (ruidoso, linear) —
        // o RR faz o denoise. Ligar com SetUseNrd(false) (senao o composite tentaria a OUT_SPEC do NRD).
        void SetRawSpec(bool V) { RawSpec = V; }
        bool GetRawSpec() const { return RawSpec; }

        // SRV shader-visivel do Resolved (radiancia especular crua + hitDist no .a). O guides pass do RR
        // le daqui p/ extrair o kBufferTypeSpecularHitDistance. Valido apos SetupForResize.
        u32 GetResolvedSRVSlot() const { return ResolvedSRVSlot; }

        // Grava o composite (fullscreen, blend aditivo no HDR). HdrRtv = RTV do HDR color (ja em
        // RENDER_TARGET). Caller restaura RT/viewport/root sig depois se precisar.
        void RecordComposite(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                             D3D12_CPU_DESCRIPTOR_HANDLE HdrRtv, u32 Width, u32 Height);

        bool IsReady() const { return Ready; }

        // Tunaveis (editor).
        void SetEnabled(bool V)       { Enabled = V; }
        bool GetEnabled() const       { return Enabled; }
        void SetMaxRoughness(f32 V)   { MaxRoughnessToTrace = V; }
        f32  GetMaxRoughness() const  { return MaxRoughnessToTrace; }
        void SetRoughnessFade(f32 V)  { RoughnessFadeLength = V; }
        f32  GetRoughnessFade() const { return RoughnessFadeLength; }
        // Back-face culling NOS RAIOS DE REFLEXAO. Politica por passe, no lugar da chave global
        // que existia na TLAS: o Lumen culla no passe de reflexao
        // (LumenReflectionHardwareRayTracing.usf:181) e NAO culla no gather do ReSTIR
        // (LumenReSTIRGather.usf:315) — os dois regimes convivem porque a ray flag e por raio.
        // Default OFF = comportamento historico da Smile (nada cullava, ja que a TLAS marcava
        // tudo como two-sided). Ligar aproxima da UE e precisa de A/B proprio: reflexo de contato
        // e de parede sao os lugares onde muda.
        void SetBackfaceCull(bool V) { BackfaceCull = V; }
        bool GetBackfaceCull() const { return BackfaceCull; }

        void SetFoliageShadows(bool V){ FoliageShadows = V; }
        bool GetFoliageShadows() const{ return FoliageShadows; }
        void SetTemporal(bool V)      { Temporal = V; }
        bool GetTemporal() const      { return Temporal; }
        void SetMaxFrames(f32 V)      { MaxFrames = V; }
        f32  GetMaxFrames() const     { return MaxFrames; }
        void SetSpatialRadius(f32 V)  { SpatialRadius = V; }
        f32  GetSpatialRadius() const { return SpatialRadius; }
        void SetFullResMaxRough(f32 V)  { FullResMaxRough = V; }
        f32  GetFullResMaxRough() const { return FullResMaxRough; }
        void SetDebugMode(u32 V)      { DebugMode = V; }
        u32  GetDebugMode() const     { return DebugMode; }
        void SetWaterReflectionScale(f32 V) { WaterReflectionScale = V; }
        void SetWaterWindDirection(f32 Radians) { WaterWindDirection = Radians; }

    private:
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateCompositePipeline(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FComputePipeline TracePSO;       // 12 SRV, 3 UAV [radiance, raydata, motion]
        // Gemea instrumentada do trace (FShaderTimer): mesmas tabelas + slot falso da NVAPI.
        // PSO separada e nao um if no CB — a instrumentacao custa registrador no passe quente.
        FComputePipeline TracePSOTimed;
        FComputePipeline TraceMirrorPSO; // 12 SRV, 2 UAV [resolved, motion]
        FComputePipeline ResolvePSO;  // 5 SRV, 2 UAV [resolved, motion]
        FComputePipeline TemporalPSO; // 5 SRV [resolved, gbuf, depth, histPrev, motion], 1 UAV
        FComputePipeline SpatialPSO;  // 3 SRV [histCurr, gbuf, depth], 1 UAV [denoised]
        FComputePipeline NrdPackPSO;  // 3 SRV [resolved, gbuf, depth], 1 UAV [NRD IN_SPEC] (NRD)
        FComputePipeline WaterTracePSO;    // 15 SRV [cena + water + copies + env], 2 UAV
        FComputePipeline WaterTemporalPSO; // 5 SRV, 1 UAV; historico exclusivo da agua
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRS;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> WaterCompositePSO;

        // Trace -> Radiance(half) + RayData(half). Resolve -> Resolved(full, denoise espacial+upsample).
        // Temporal -> History[curr] (acumulado; ping-pong). Composite le History[curr]. RGBA16F.
        Microsoft::WRL::ComPtr<ID3D12Resource> Radiance;
        Microsoft::WRL::ComPtr<ID3D12Resource> RayData;
        Microsoft::WRL::ComPtr<ID3D12Resource> RayMotion;
        Microsoft::WRL::ComPtr<ID3D12Resource> Resolved;
        Microsoft::WRL::ComPtr<ID3D12Resource> ResolvedMotion;
        Microsoft::WRL::ComPtr<ID3D12Resource> History[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> Denoised; // saida do spatial; lida pelo composite
        Microsoft::WRL::ComPtr<ID3D12Resource> WaterResolved;
        Microsoft::WRL::ComPtr<ID3D12Resource> WaterMotion;
        Microsoft::WRL::ComPtr<ID3D12Resource> WaterHistory[2];
        D3D12_RESOURCE_STATES RadianceState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RayDataState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RayMotionState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResolvedMotionState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES HistoryState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES DenoisedState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES WaterResolvedState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES WaterMotionState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES WaterHistoryState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 RadianceSRVSlot     = kInvalidSlot;
        u32 RayDataSRVSlot      = kInvalidSlot;
        u32 RayMotionSRVSlot    = kInvalidSlot;
        u32 ResolvedSRVSlot     = kInvalidSlot;
        u32 ResolvedUAVSlot     = kInvalidSlot;
        u32 ResolvedMotionSRVSlot = kInvalidSlot;
        u32 ResolvedMotionUAVSlot = kInvalidSlot;
        u32 HistorySRVSlot[2]   = { kInvalidSlot, kInvalidSlot };
        u32 HistoryUAVSlot[2]   = { kInvalidSlot, kInvalidSlot };
        u32 DenoisedSRVSlot     = kInvalidSlot;
        u32 DenoisedUAVSlot     = kInvalidSlot;
        u32 TraceUAVTable       = kInvalidSlot; // 3 UAVs [radiance, raydata, raymotion]
        u32 ResolvedUAVTable    = kInvalidSlot; // 2 UAVs [resolved, resolvedmotion]
        // 8 SRVs [TLAS,skyview,inst,irrad,verts,idx,depth,gbuf] + t8 (luzes, por frame). Uma
        // tabela por frame em voo: o t8 muda todo frame e a do frame anterior ainda pode estar
        // sendo lida pela GPU (descriptor versioning).
        static constexpr u32 kTraceTables = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)
        u32 TraceTable[kTraceTables] = { kInvalidSlot, kInvalidSlot };
        u32 ResolveTableStart   = kInvalidSlot; // 5 SRVs [radiance, raydata, motion, depth, gbuf]
        // Por paridade (curr=0/1): temporal le History[1-curr] e escreve History[curr]; composite
        // le History[curr]. 2 tabelas pre-montadas (sem CopyDescriptors por frame).
        u32 TemporalTable[2]    = { kInvalidSlot, kInvalidSlot }; // + resolved motion
        u32 SpatialTable[2]     = { kInvalidSlot, kInvalidSlot }; // 3 SRVs [hist[curr], gbuf, depth]
        u32 CompositeTable[2]   = { kInvalidSlot, kInvalidSlot }; // 5 SRVs [denoised, gbuf, depth, brdfLut, gbufC]
        // NRD unificado: pack especular + composite lendo a OUT_SPEC do NRD em vez do Denoised caseiro.
        u32 SpecPackSrvTable    = kInvalidSlot;                   // 3 SRVs [resolved, gbuf, depth]
        u32 SpecPackUAVSlot     = kInvalidSlot;                   // UAV da IN_SPEC do NRD
        ID3D12Resource* NrdInSpec = nullptr;                      // nao-owned (FNrdDenoiser); p/ o clear
        u32 NrdOutSpecSRV       = kInvalidSlot;                   // SRV da OUT_SPEC do NRD
        u32 CompositeTableNrd[2] = { kInvalidSlot, kInvalidSlot };// 5 SRVs [nrdOutSpec, gbuf, depth, brdfLut, gbufC]
        // RR: composite lendo o Resolved CRU (parity-independente — Resolved nao faz ping-pong).
        u32 CompositeTableRaw    = kInvalidSlot;                  // 5 SRVs [resolved, gbuf, depth, brdfLut, gbufC]
        u32 WaterResolvedSRVSlot = kInvalidSlot;
        u32 WaterResolvedUAVSlot = kInvalidSlot;
        u32 WaterMotionSRVSlot   = kInvalidSlot;
        u32 WaterMotionUAVSlot   = kInvalidSlot;
        u32 WaterHistorySRVSlot[2] = { kInvalidSlot, kInvalidSlot };
        u32 WaterHistoryUAVSlot[2] = { kInvalidSlot, kInvalidSlot };
        u32 WaterTraceUAVTable = kInvalidSlot;
        u32 WaterTraceTable[kTraceTables] = { kInvalidSlot, kInvalidSlot };
        u32 WaterTemporalTable[2] = { kInvalidSlot, kInvalidSlot };
        u32 WaterCompositeTable[2] = { kInvalidSlot, kInvalidSlot };
        u32 WaterCompositeRawTable = kInvalidSlot;
        u32 WaterSpecHitTable = kInvalidSlot; // [WaterResolved, GBufferB] para o guide do RR
        u32 DepthSlotCached     = kInvalidSlot;
        u32 GBufferSlotCached   = kInvalidSlot;
        u32 GBufferCSlotCached  = kInvalidSlot; // metallic (dieta do G-buffer)
        u32 GBufferASlotCached  = kInvalidSlot; // BaseColor (tint do metal no composite)
        u32 BRDFLutSlotCached   = kInvalidSlot;
        u32  FrameParity        = 0;     // alterna a cada RecordTrace
        u32  CurrParity         = 0;     // paridade usada neste frame (trace->composite)
        bool NeedsHistoryClear  = false; // limpa os 2 history no 1o RecordTrace pos-setup
        u32  WaterFrameParity = 0;
        u32  WaterCurrParity  = 0;
        bool NeedsWaterHistoryClear = false;
        bool WaterUseRaw = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        ReflectionConstants CPU{};

        // Params estaticos do DDGI (grid/atlas), guardados p/ preencher o CB por frame.
        Vec4 GIGridMinSpacing{ 0,0,0,1 };
        Vec4 GIGridCount{ 0,0,0,0 };
        Vec4 GIAtlasParams{ 6,1,1,0 };
        FDDGICascadeConstants GICascadesCPU{};
        f32  GIMaxRayDist = 0.0f;

        u32  Width = 0, Height = 0;          // full-res (resolve, composite)
        u32  HalfWidth = 0, HalfHeight = 0;  // half-res (trace; Fase 2b)
        u32  TimerSlot  = kInvalidSlot;      // alvo de timer vigente (kInvalidSlot = captura off)
        bool TraceTimed = false;             // permutacao instrumentada criada com sucesso
        bool Initialized = false;
        bool Ready       = false;

        // Tunaveis.
        bool Enabled             = true;
        FRayEpsilonProfile RayEps;        // perfil compartilhado (dono = Renderer)
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        FRadianceCacheShaderParams RadianceCacheParams{};
        Vec4               SkyLutParams{};
        f32  MaxRoughnessToTrace = 0.6f;  // acima -> so DDGI (combine do Lumen)
        f32  RoughnessFadeLength = 0.1f;  // fade RT<->DDGI
        f32  AlbedoLOD           = 2.0f;  // LOD do albedo no hit (mais nitido que o difuso=4)
        bool FoliageShadows      = true;  // folhagem nos shadow rays do hit (GATHER vs OPAQUE)
        bool BackfaceCull        = false; // culling nos raios de reflexao (ver setter)
        bool UseNrd              = false; // denoise via NRD RELAX especular (unificado c/ o GI)
        bool RawSpec             = false; // reflexao crua p/ o DLSS Ray Reconstruction (denoise = RR)
        // Temporal (Fase 3).
        bool Temporal            = true;  // acumulacao temporal (off = só resolve espacial)
        f32  MaxFrames           = 12.0f; // frames acumulados (Lumen default)
        f32  NeighborhoodGamma   = 1.0f;  // γ do neighborhood clamp (↑ = menos ruido, + ghosting)
        f32  SpatialRadius       = 8.0f;  // raio max (px) do denoise espacial pos-temporal na borda
        f32  FullResMaxRough     = 0.4f;  // <= isto -> trace full-res (espelho exato <0.05, senao GGX); resto half-res
        u32  DebugMode           = 0;     // 0=off, 1=acumulacao (frames), 2=mascara espelho (overlay)
        f32  WaterReflectionScale = 1.0f;
        f32  WaterWindDirection = 0.0f;
        f32  WaterSceneColorMaxMip = 0.0f;
    };
}
