#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FCommandQueue;
    class FScene;

    struct alignas(256) DDGIConstants {
        Vec4 GridMinSpacing;  // xyz = origem do grid (mundo), w = espacamento
        Vec4 GridCountRays;   // xyz = nº de probes por eixo, w = raios por probe
        Vec4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
        Vec4 SunDirIntensity; // xyz = direcao P/ o sol, w = intensidade do sol
        Vec4 SunColorHyst;    // rgb = cor do sol, w = hysteresis (blend temporal)
        Vec4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = normalBias
        Vec4 DistAtlasParams; // x = dist tile, y = dist atlasW, z = dist atlasH, w = realHitShading
        Vec4 MiscParams;      // x = relocationEnabled (Fase 2), y = deactivThresh, z = maxRays, w = minRays
        Vec4 MiscParams2;     // x = canMarkActivated (relocacao tem +1 frame agendado), yzw = -
    };

    class FDDGI {
    public:
        static constexpr int kRaysPerProbe = 64; 
        static constexpr int kTileSize     = 6;  
        static constexpr int kDistTileSize = 14; 

        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, const Vec3& AABBMin, const Vec3& AABBMax,
                           u32 TlasSRVSlot, u32 SkyViewSRVSlot);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex);

        void RecordUpdate(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        bool IsReady() const { return Ready; }
        u32  IrradianceAtlasSRV() const { return AtlasSRVSlot; }
        u32  InstanceSRV() const { return InstanceSRVSlot; }
        u32  VertexSRV()   const { return VertexSRVSlot; }
        u32  IndexSRV()    const { return IndexSRVSlot; }
        u32  SceneGITableStart()  const { return SceneGITableStart_; }
        u32  DistAtlasSRV()    const { return DistSRVSlot; }
        u32  ProbesTraceSRV()  const { return ProbesTraceSRVSlot; }
        u32  ProbeDataSRV()    const { return ProbeDataSRVSlot; }
        u32  ProbeRayCountSRV()const { return ProbeRayCountSRVSlot; } 
        u32  NumProbesCount()  const { return NumProbes; }
        u32  RaysPerProbe()    const { return kRaysPerProbe; }

        Vec3 GridMin()   const { return GridMinV; }
        f32  Spacing()   const { return SpacingV; }
        Vec3 GridCount() const { return Vec3{ (f32)CountX, (f32)CountY, (f32)CountZ }; }
        f32  AtlasW()    const { return (f32)AtlasWidth; }
        f32  AtlasH()    const { return (f32)AtlasHeight; }
        f32  TileSizeF() const { return (f32)kTileSize; }
        f32  DistAtlasW()    const { return (f32)DistAtlasWidth; }
        f32  DistAtlasH()    const { return (f32)DistAtlasHeight; }
        f32  DistTileSizeF() const { return (f32)kDistTileSize; }
        f32  MaxRayDistance() const { return MaxRayDist; } 

        void SetIntensity(f32 V)  { Intensity = V; }
        f32  GetIntensity() const { return Intensity; }
        void SetHysteresis(f32 V) { Hysteresis = V; }
        f32  GetHysteresis() const{ return Hysteresis; }

        void SetDeactivationThreshold(f32 V) { DeactivationThreshold = V; TriggerReclassify(); }
        f32  GetDeactivationThreshold() const { return DeactivationThreshold; }

        void SetAdaptiveRays(bool V) { AdaptiveRays = V; TriggerReclassify(); }
        bool GetAdaptiveRays() const { return AdaptiveRays; }
        void SetMaxRays(int V) { MaxRays = V; TriggerReclassify(); }   
        int  GetMaxRays() const { return MaxRays; }
        void SetMinRays(int V) { MinRays = V; TriggerReclassify(); }   
        int  GetMinRays() const { return MinRays; }

        void SetRealHitShading(bool V) { RealHitShading = V; }
        bool GetRealHitShading() const { return RealHitShading; }

        void SetRelocation(bool V) { Relocation = V; RelocateFramesLeft = V ? kRelocateConvergeFrames : 4; }
        bool GetRelocation() const { return Relocation; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseSceneResources(FTextureSRVHeap& SRVHeap);

        void TriggerReclassify() {
            if (Relocation && Ready && RelocateFramesLeft < kReclassifyFrames)
                RelocateFramesLeft = kReclassifyFrames;
        }
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return CB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(DDGIConstants);
        }

        FVolumetricPipeline TracePSO;      
        FVolumetricPipeline UpdatePSO;     
        FVolumetricPipeline UpdateDistPSO; 
        FVolumetricPipeline RelocatePSO;   

        Microsoft::WRL::ComPtr<ID3D12Resource> IrradAtlas;       
        Microsoft::WRL::ComPtr<ID3D12Resource> DistAtlas;        
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbesTrace;      
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceGeoBuf;   
        Microsoft::WRL::ComPtr<ID3D12Resource> MergedVertexBuf;  
        Microsoft::WRL::ComPtr<ID3D12Resource> MergedIndexBuf;   
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeDataBuf;     
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeRayCountBuf; 

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 AtlasSRVSlot       = kInvalidSlot;
        u32 AtlasUAVSlot       = kInvalidSlot;
        u32 DistSRVSlot        = kInvalidSlot;
        u32 DistUAVSlot        = kInvalidSlot;
        u32 ProbesTraceSRVSlot = kInvalidSlot;
        u32 ProbesTraceUAVSlot = kInvalidSlot;
        u32 InstanceSRVSlot    = kInvalidSlot;
        u32 VertexSRVSlot      = kInvalidSlot;
        u32 IndexSRVSlot       = kInvalidSlot;
        u32 ProbeDataSRVSlot   = kInvalidSlot;
        u32 ProbeDataUAVSlot   = kInvalidSlot;
        u32 ProbeRayCountSRVSlot = kInvalidSlot;
        u32 ProbeRayCountUAVSlot = kInvalidSlot;
        u32 TraceTableStart    = kInvalidSlot;
        u32 SceneGITableStart_ = kInvalidSlot;
        u32 UpdateTableStart   = kInvalidSlot;   // [ProbesTrace, ProbeData] p/ Update/UpdateDist

        D3D12_RESOURCE_STATES AtlasState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES DistState      = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbesState    = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB   = nullptr;
        u32 FrameSlot  = 0;
        DDGIConstants CPU{};

        Vec3 GridMinV{ 0,0,0 };
        f32  SpacingV    = 1.0f;
        int  CountX = 0, CountY = 0, CountZ = 0;
        u32  NumProbes   = 0;
        u32  AtlasWidth  = 0, AtlasHeight = 0;
        u32  DistAtlasWidth = 0, DistAtlasHeight = 0;

        static constexpr D3D12_RESOURCE_STATES kAtlasRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        f32  Intensity    = 1.0f;
        f32  Hysteresis   = 0.99f;
        f32  SkyIntensity = 1.0f;
        f32  NormalBias   = 0.2f;   
        f32  MaxRayDist   = 0.0f;  
        bool RealHitShading = true; 
        bool Relocation     = true; 
        f32  DeactivationThreshold = 0.20f; 
        bool AdaptiveRays   = false; 
        int  MaxRays        = 64;    
        int  MinRays        = 16;    
        static constexpr u32 kRelocateConvergeFrames = 180;
        static constexpr u32 kReclassifyFrames = 6;
        u32  RelocateFramesLeft = 0;

        bool Initialized = false;
        bool Ready       = false;
    };
}
