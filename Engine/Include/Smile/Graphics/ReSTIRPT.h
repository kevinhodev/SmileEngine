#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Constantes do ReSTIR PT (b0 de TODOS os passes). alignas(256); casa campo-a-campo com o
    // cbuffer UNICO de Shaders/PathTracer/PTCommon.hlsli — o layout so muda em lockstep nos dois.
    struct alignas(256) ReSTIRPTConstants {
        Mat44 InvViewProj;        // inversa FULL da view-proj (jittered, casa com o depth)
        Mat44 View;               // world -> view (pack do NRD, F4)
        Vec4  CameraPos;          // xyz = camera mundo
        Vec4  ScreenParams;       // W, H, 1/W, 1/H
        Vec4  GridMinSpacing;     // DDGI: xyz = origem do grid, w = espacamento
        Vec4  GridCount;          // DDGI: xyz = nº de probes por eixo
        Vec4  AtlasParams;        // DDGI irradiancia: x = tile, y = W, z = H
        Vec4  SunDirIntensity;    // xyz = direcao P/ o sol (norm.), w = intensidade
        Vec4  SunColorCosRadius;  // rgb = cor do sol, w = cos(raio angular) p/ sombra suave
        Vec4  TraceParams;        // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
        Vec4  PathParams;         // x=maxBounces, y=rrStartBounce, z=fireflyMax, w=albedoLOD
        Vec4  ReuseParams;        // F1: x=MCap, y=posRejectScale, z=temporal(0/1)
        Vec4  SpatialParams;      // F3: x=sigma(px), y=count, z=spatial(0/1), w=normalReject
        Vec4  ShiftParams;        // F2: x=kFootprint, y=reconnectRoughMin
        Vec4  NrdHitDistParams;   // F4: xyz = ReblurHitDistanceParameters {A,B,C}
        Vec4  DebugParams;        // x=debugMode (0=off, 1=accum), y=accumFrame
    };

    // ReSTIR Path Tracing (GRIS/Lin 2022 + Enhanced/Lin 2026) — modo experimental atras de
    // UsePathTracer. F0: megakernel DI+GI unificado sem reservoir (PTInitial.cs.hlsl), saida
    // full-radiance lida pelo deferred como passthrough (ReflectionParams.w == 3). Molde do
    // FReSTIRGI (tabelas pre-montadas, CB ring, transicoes rastreadas).
    class FReSTIRPT {
    public:
        void Initialize(ID3D12Device* Device);

        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot, u32 IrradSlot,
                            u32 VertexSlot, u32 IndexSlot, u32 DepthSlot,
                            u32 GBufASlot, u32 GBufBSlot, u32 GBufCSlot, u32 VelocitySlot);

        // ViewProjUnjittered detecta camera parada (reset da acumulacao de debug).
        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity, f32 NormalBias,
                            const Mat44& View, const Mat44& ViewProjUnjittered);

        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        bool IsReady() const { return Ready; }
        // Tabela t16 do deferred (root param 9): a radiancia final do PT.
        u32  OutputSRVSlot() const { return PTOutSRV; }

        // Tunaveis.
        void SetMaxBounces(f32 V)   { MaxBounces = V; }
        f32  GetMaxBounces() const  { return MaxBounces; }
        void SetDebugMode(u32 V)    { DebugMode = V; AccumFrames = 0; }
        u32  GetDebugMode() const   { return DebugMode; }

    private:
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline TracePSO; // 11 SRV, 2 UAV, heap-directly-indexed (PTInitial)

        Microsoft::WRL::ComPtr<ID3D12Resource> PTOutput;  // RGBA16F: radiancia + hitDist
        Microsoft::WRL::ComPtr<ID3D12Resource> AccumTex;  // RGBA32F: media progressiva (debug)
        D3D12_RESOURCE_STATES PTOutputState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AccumState    = D3D12_RESOURCE_STATE_COMMON;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 PTOutSRV   = kInvalidSlot;
        u32 PTOutUAV   = kInvalidSlot;
        u32 AccumUAV   = kInvalidSlot;
        u32 TraceTable    = kInvalidSlot; // 11 SRVs [TLAS,sky,inst,irrad,verts,idx,depth,gbufA..C,vel]
        u32 TraceUAVTable = kInvalidSlot; // 2 UAVs [PTOut, Accum]

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        ReSTIRPTConstants CPU{};

        Vec4 GIGridMinSpacing{ 0, 0, 0, 1 };
        Vec4 GIGridCount{ 0, 0, 0, 0 };
        Vec4 GIAtlasParams{ 6, 1, 1, 0 };
        f32  GIMaxRayDist = 0.0f;

        u32  Width = 0, Height = 0;
        bool NeedsClear  = false;
        bool Initialized = false;
        bool Ready       = false;

        // Acumulacao progressiva (debug): zera quando a camera mexe (ViewProj nao-jitterada muda).
        Mat44 PrevVPUnjit{};
        u32   AccumFrames = 0;
        bool  HavePrevVP  = false;

        // Tunaveis.
        f32  MaxBounces        = 3.0f;  // segmentos indiretos alem de x1 (cauda DDGI no ultimo)
        f32  RrStartBounce     = 3.0f;  // Russian roulette a partir deste bounce (so no inicial)
        f32  FireflyMax        = 25.0f; // teto de luminancia por caminho (0 = off)
        f32  AlbedoLOD         = 2.0f;
        f32  SunAngularRadiusDeg = 0.6f;
        u32  DebugMode         = 0;    // 0 = off, 1 = acumulacao progressiva
    };
}
