#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumeTexture.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {

    // Froxel volumetric fog (arquitetura da UE): grid 3D fixo alinhado ao frustum
    // (kGridW x kGridH x kGridZ, fatiado em view-Z com distribuicao log ate MaxDistance),
    // 3 dispatches — densidade (mesma 2-exponencial do height fog) -> light scattering
    // (sol via CSM + ambiente via DDGI) -> integracao acumulada. O fog fullscreen amostra
    // o volume integrado em t3 e EXCLUI o height fog analitico no alcance coberto; alem
    // dele o analitico continua. F2: jitter Halton + reprojecao temporal no scattering
    // (historia ping-pong, blend 0.9, supersampling em history-miss — receita da UE).
    // F3 adiciona luzes puntuais.
    class FVolumetricFogPass {
    public:
        static constexpr u32 kGridW = 160, kGridH = 90, kGridZ = 64;

        struct FFrameParams {
            Mat44 InvViewProjUnjit;          // SEM jitter, com translacao
            Mat44 ViewProjUnjit;             // idem, direto — vira o PrevViewProj do proximo frame
            u32   FrameIndex = 0;            // dirige o jitter Halton
            Vec3  CameraPos{};
            Vec3  CameraForward{ 0.0f, 0.0f, 1.0f };
            Vec3  DirToSun{ 0.0f, 1.0f, 0.0f };
            Vec3  SunColorTimesIntensity{};  // radiancia real da key light (cor x int)
            Vec4  CollapsedFog{};            // FFogPass::CollapsedFogParams (mesmo meio do frame)
            Vec3  SkyAmbient{};              // fallback do ambiente quando DDGI off
            f32   NearZ = 0.1f;
            Vec4  DDGIGridMin{};             // xyz = origem, w = spacing
            Vec4  DDGIGridCount{};           // xyz = probes por eixo, w = DDGI on (>0.5)
            Vec4  DDGIParams{};              // x = intensidade, y = tile, z/w = atlas W/H
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void UpdatePerFrame(u32 FrameSlot, const FFrameParams& P);

        // F3 — luzes puntuais: chamado DEPOIS do culling de luzes do frame (o loop do
        // scattering le a MESMA lista FGPULight do deferred). Patcha o CB do slot ja
        // escrito pelo UpdatePerFrame deste frame.
        void PatchLights(u32 NumLights, f32 InvSpotRes, f32 DepthBias, f32 PointNear);

        // F4 — sombra das nuvens no sol: chamado depois do update das nuvens (os params
        // dependem do centro snapado do shadow map). MESMOS vetores do deferred/shafts.
        void PatchCloudShadow(const Vec4& CloudShadowParams, const Vec4& CloudShadowParams2);

        // CSM e sombras locais legiveis em NON_PIXEL antes (EnsureReadableCompute).
        // DDGIIrradianceSRVSlot: passar um SRV valido qualquer com DDGI off (CB gate).
        // LightsVA = slice do FrameSlot no LightBuffer do deferred (root SRV t3);
        // LocalShadowSRVSlot = atlas spot (cube array vive no slot+1, tabela cobre os 2).
        // CloudShadowSRVSlot: shadow map das nuvens (passar um SRV valido qualquer com
        // nuvens off — CB gate CloudShadowParams.w = 0).
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     D3D12_GPU_VIRTUAL_ADDRESS CSMConstantsAddr, u32 CSMShadowSRVSlot,
                     u32 DDGIIrradianceSRVSlot, D3D12_GPU_VIRTUAL_ADDRESS LightsVA,
                     u32 LocalShadowSRVSlot, u32 CloudShadowSRVSlot);

        u32  IntegratedSRVSlot() const { return Integrated.SRVSlot(); }
        // (B, O, S, kGridZ) do ultimo UpdatePerFrame — o fog fullscreen fatia igual.
        Vec4 GridZParams() const   { return GridZ; }
        bool IsInitialized() const { return Initialized; }

        // Historia obsoleta quando o efeito dorme um frame ou os params do grid mudam.
        void ResetHistory() { HistoryValid = false; }

        void SetMaxDistance(f32 V) {
            const f32 NewV = V < 10.0f ? 10.0f : V;
            if (NewV != MaxDistance) ResetHistory(); // slicing muda -> historia nao reprojeta
            MaxDistance = NewV;
        }
        f32  GetMaxDistance() const     { return MaxDistance; }
        void SetTemporal(bool V) {
            if (V && !Temporal) ResetHistory(); // religou: historia e lixo
            Temporal = V;
        }
        bool GetTemporal() const        { return Temporal; }
        void SetPhaseG(f32 V)           { PhaseG = V; }
        f32  GetPhaseG() const          { return PhaseG; }
        void SetExtinctionScale(f32 V)  { ExtinctionScale = V; }
        f32  GetExtinctionScale() const { return ExtinctionScale; }
        void SetAmbientIntensity(f32 V) { AmbientIntensity = V; }
        f32  GetAmbientIntensity() const{ return AmbientIntensity; }
        void SetLightsIntensity(f32 V)  { LightsIntensity = V; }
        f32  GetLightsIntensity() const { return LightsIntensity; }

    private:
        // Espelho exato do VolFogCB (VolumetricFogCommon.hlsli).
        struct alignas(256) VolFogConstants {
            Vec4  GridZParams;
            Vec4  GridSize;
            Mat44 InvViewProj;
            Vec4  CameraWorldPos;
            Vec4  CamForward;
            Vec4  SunDirPhase;
            Vec4  SunColorInt;
            Vec4  FogDensityP;
            Vec4  AlbedoAmb;
            Vec4  DDGIGridMin;
            Vec4  DDGIGridCount;
            Vec4  DDGIParams;
            Vec4  AmbientFallback;
            Mat44 PrevViewProj;
            Vec4  PrevCamPos;
            Vec4  PrevCamForward;
            Vec4  TemporalParams;
            Vec4  FrameJitterOffsets[8];
            Vec4  LightParamsVF;
            Vec4  LightParamsVF2;
            Vec4  CloudShadowParams;
            Vec4  CloudShadowParams2;
        };
        static constexpr u32 kMissSupersamples = 4;    // amostras qdo a historia falha
        static constexpr f32 kHistoryWeight    = 0.9f; // blend da UE

        void BuildScatteringRootSignature(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);

        FVolumeTexture VBufferA;             // (albedo*ext, ext) por froxel
        FVolumeTexture LightScatteringPP[2]; // (luz espalhada, ext) — ping-pong temporal
        FVolumeTexture Integrated;           // (inscatter acumulado, transmitancia)
        u32  CurrentScatter = 0;             // indice escrito NESTE frame

        FVolumetricPipeline SetupPSO;     // b0 + t0 (unused) + u0
        FVolumetricPipeline IntegratePSO; // b0 + t0 (scattering) + u0

        Microsoft::WRL::ComPtr<ID3D12RootSignature> ScatterRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedBase = nullptr;
        u32 FrameSlot  = 0;

        Vec4 GridZ{};

        // Bookkeeping da reprojecao temporal (frame anterior).
        bool  HistoryValid  = false;
        Mat44 StoredPrevVP  = Mat44::Identity();
        Vec3  PrevCamPosV{};
        Vec3  PrevCamFwdV{ 0.0f, 0.0f, 1.0f };
        bool  HasPrevFrame  = false;
        bool  Temporal      = true;

        f32  MaxDistance      = 100.0f; // alcance do volume (m); alem = analitico
        f32  PhaseG           = 0.3f;   // HG do sol (UE usa 0.2; shafts de longe usam 0.7)
        f32  ExtinctionScale  = 1.0f;   // densidade extra do volume (so o froxel)
        f32  AmbientIntensity = 1.0f;
        f32  LightsIntensity  = 1.0f;   // luzes puntuais no ar (0 desliga o loop)
        Vec3 Albedo{ 0.9f, 0.9f, 0.9f };

        bool Initialized = false;

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(VolFogConstants);
        }
        VolFogConstants* Mapped() const {
            return reinterpret_cast<VolFogConstants*>(
                MappedBase + static_cast<size_t>(FrameSlot) * sizeof(VolFogConstants));
        }
    };
}
