#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Constantes do Specular GI (b0 do trace E do composite). O prefixo (InvViewProj, CameraPos,
    // ScreenParams, ReflectParams) e IDENTICO ao CompositeCB -> os dois passes compartilham o
    // MESMO buffer (o composite le so o prefixo). Bate campo-a-campo com ReflectionCB
    // (ReflectionTrace.cs.hlsl) e CompositeCB (ReflectionComposite.ps.hlsl).
    struct alignas(256) ReflectionConstants {
        Mat44 InvViewProj;       // FULL inverse view-proj (row-major)
        Vec4  CameraPos;         // xyz = camera world
        Vec4  ScreenParams;      // W, H, 1/W, 1/H
        Vec4  ReflectParams;     // x=maxRoughnessToTrace, y=roughnessFadeLength, z=realHitShading, w=albedoLOD
        Vec4  GridMinSpacing;    // DDGI grid origin + spacing
        Vec4  GridCount;         // DDGI probe counts
        Vec4  AtlasParams;       // DDGI irradiance atlas (tile, W, H)
        Vec4  SunDirIntensity;   // xyz = dir TO sun, w = intensity
        Vec4  SunColor;          // rgb = sun color
        Vec4  TraceParams;       // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
        Vec4  HalfScreenParams;  // halfW, halfH, 1/halfW, 1/halfH (trace e half-res; Fase 2b)
        Mat44 PrevViewProj;      // VP (sem jitter) do frame anterior — reprojeção do temporal (Fase 3)
        Vec4  TemporalParams;    // x=maxFramesAccumulated, y=neighborhoodClampScale(γ), zw=-
    };

    // Specular GI — reflexoes ray-traced (DXR inline), esqueleto estilo Lumen Reflections.
    // Fase 1: mirror, full-res, sem denoise. Passe de compute (trace, sombreado pelo MESMO
    // HitShading.hlsli do DDGI) -> composite aditivo no HDR. Reusa a geometria/atlas do DDGI.
    // No-op sem DXR/DDGI pronto. So roda sem MSAA (G-buffer single-sample).
    class FReflections {
    public:
        // Cria a pipeline de trace (compute) + a pipeline de composite (grafica) + o CB. 1x.
        void Initialize(ID3D12Device* Device);

        // (Re)cria a textura de saida (radiancia) no tamanho da tela e (re)monta as tabelas de
        // descritores. Chamar no setup da cena e em TODO resize (depth/gbuffer recriados). Os
        // slots vem do DDGI (geometria/atlas), RaytracingScene (TLAS), Atmosphere (skyview) e
        // Renderer (depth, gbuffer, BRDF LUT).
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot, u32 IrradSlot,
                            u32 VertexSlot, u32 IndexSlot, u32 DepthSlot, u32 GBufferSlot,
                            u32 BRDFLutSlot);

        // Params estaticos do volume DDGI (grid/atlas) p/ o CB. Chamar quando o volume e (re)criado.
        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Mat44& PrevViewProj,
                            const Vec3& CameraPos, u32 Width, u32 Height, const Vec3& SunDir,
                            f32 SunIntensity, const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity,
                            f32 NormalBias, bool RealHitShading);

        // Grava o trace (compute) -> radiancia (UAV). Caller ja setou os descriptor heaps e
        // transicionou depth/gbuffer p/ legiveis por shader. Deixa a radiancia legivel por PS.
        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

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
        void SetRealHitShading(bool V){ RealHit = V; }
        bool GetRealHitShading() const{ return RealHit; }
        void SetTemporal(bool V)      { Temporal = V; }
        bool GetTemporal() const      { return Temporal; }
        void SetMaxFrames(f32 V)      { MaxFrames = V; }
        f32  GetMaxFrames() const     { return MaxFrames; }

    private:
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateCompositePipeline(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline TracePSO;    // 8 SRV, 2 UAV [radiance, raydata], heap-directly-indexed
        FVolumetricPipeline ResolvePSO;  // 4 SRV [radiance, raydata, depth, gbuf], 1 UAV [resolved]
        FVolumetricPipeline TemporalPSO; // 4 SRV [resolved, gbuf, depth, histPrev], 1 UAV [histCurr]
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRS;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;

        // Trace -> Radiance(half) + RayData(half). Resolve -> Resolved(full, denoise espacial+upsample).
        // Temporal -> History[curr] (acumulado; ping-pong). Composite le History[curr]. RGBA16F.
        Microsoft::WRL::ComPtr<ID3D12Resource> Radiance;
        Microsoft::WRL::ComPtr<ID3D12Resource> RayData;
        Microsoft::WRL::ComPtr<ID3D12Resource> Resolved;
        Microsoft::WRL::ComPtr<ID3D12Resource> History[2];
        D3D12_RESOURCE_STATES RadianceState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RayDataState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES HistoryState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 RadianceSRVSlot     = kInvalidSlot;
        u32 RayDataSRVSlot      = kInvalidSlot;
        u32 ResolvedSRVSlot     = kInvalidSlot;
        u32 ResolvedUAVSlot     = kInvalidSlot;
        u32 HistorySRVSlot[2]   = { kInvalidSlot, kInvalidSlot };
        u32 HistoryUAVSlot[2]   = { kInvalidSlot, kInvalidSlot };
        u32 TraceUAVTable       = kInvalidSlot; // 2 UAVs contiguos [radiance, raydata]
        u32 TraceTableStart     = kInvalidSlot; // 8 SRVs [TLAS,skyview,inst,irrad,verts,idx,depth,gbuf]
        u32 ResolveTableStart   = kInvalidSlot; // 4 SRVs [radiance, raydata, depth, gbuf]
        // Por paridade (curr=0/1): temporal le History[1-curr] e escreve History[curr]; composite
        // le History[curr]. 2 tabelas pre-montadas (sem CopyDescriptors por frame).
        u32 TemporalTable[2]    = { kInvalidSlot, kInvalidSlot }; // 4 SRVs [resolved, gbuf, depth, hist[1-curr]]
        u32 CompositeTable[2]   = { kInvalidSlot, kInvalidSlot }; // 4 SRVs [hist[curr], gbuf, depth, brdfLut]
        u32  FrameParity        = 0;     // alterna a cada RecordTrace
        u32  CurrParity         = 0;     // paridade usada neste frame (trace->composite)
        bool NeedsHistoryClear  = false; // limpa os 2 history no 1o RecordTrace pos-setup

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        ReflectionConstants CPU{};

        // Params estaticos do DDGI (grid/atlas), guardados p/ preencher o CB por frame.
        Vec4 GIGridMinSpacing{ 0,0,0,1 };
        Vec4 GIGridCount{ 0,0,0,0 };
        Vec4 GIAtlasParams{ 6,1,1,0 };
        f32  GIMaxRayDist = 0.0f;

        u32  Width = 0, Height = 0;          // full-res (resolve, composite)
        u32  HalfWidth = 0, HalfHeight = 0;  // half-res (trace; Fase 2b)
        bool Initialized = false;
        bool Ready       = false;

        // Tunaveis.
        bool Enabled             = true;
        f32  MaxRoughnessToTrace = 0.6f;  // acima -> so DDGI (combine do Lumen)
        f32  RoughnessFadeLength = 0.1f;  // fade RT<->DDGI
        f32  AlbedoLOD           = 2.0f;  // LOD do albedo no hit (mais nitido que o difuso=4)
        bool RealHit             = true;  // normal real no hit (igual ao DDGI Fase 1a)
        // Temporal (Fase 3).
        bool Temporal            = true;  // acumulacao temporal (off = só resolve espacial)
        f32  MaxFrames           = 12.0f; // frames acumulados (Lumen default)
        f32  NeighborhoodGamma   = 1.0f;  // γ do neighborhood clamp (↑ = menos ruido, + ghosting)
    };
}
