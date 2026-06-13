#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FDDGI;

    class FDDGIDebug {
    public:
        enum class EMode : u32 {
            Irradiance     = 0,
            Distance       = 1, 
            Relocation     = 2, 
            Classification = 3, 
            Stability      = 4, 
        };

        void Initialize(ID3D12Device* Device, u32 SampleCount, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, u32 SampleCount, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 NumProbes);

        void Render(u32 FrameSlot, ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                    FDDGI& DDGI, const Mat44& ViewProj, const Vec3& CameraPos, u32 FrameIndex);

        void  SetEnabled(bool V)     { Enabled = V; }
        bool  GetEnabled() const     { return Enabled; }
        void  SetMode(EMode M)       { Mode = M; }
        EMode GetMode() const        { return Mode; }
        void  SetProbeRadius(f32 V)  { ProbeRadius = V; }
        f32   GetProbeRadius() const { return ProbeRadius; }
        void  SetShowVolume(bool V)  { ShowVolume = V; }
        bool  GetShowVolume() const  { return ShowVolume; }
        void  SetShowRays(bool V)    { ShowRays = V; }
        bool  GetShowRays() const    { return ShowRays; }
        void  SetRayRadius(f32 V)    { RayRadius = V; } 
        f32   GetRayRadius() const   { return RayRadius; }

    private:
        struct alignas(256) DDGIDebugConstants {
            Mat44 ViewProj;        // 64
            Vec4  GridMinSpacing;  // xyz = origem do grid, w = espacamento
            Vec4  GridCount;       // xyz = probes por eixo, w = numProbes
            Vec4  AtlasParams;     // x = tile, y = atlasW, z = atlasH, w = -
            Vec4  DistAtlasParams; // x = distTile, y = distW, z = distH, w = maxRayDist
            Vec4  DebugParams;     // x = mode, y = probeRadius, z = relocMaxOffset, w = deactivThreshold
            Vec4  CameraPos;       // xyz = camera, w = -
            Vec4  RayParams;       // x = frameIndex, y = rayRadius (world), z = -, w = -
            u8    _Tail[256 - 64 - 7 * 16] = {};
        };
        static_assert(sizeof(DDGIDebugConstants) == 256, "DDGIDebugConstants must be 256 bytes");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, u32 SampleCount, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbePSO;  
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VolumePSO; 
        Microsoft::WRL::ComPtr<ID3D12PipelineState> RaysPSO;   
        FVolumetricPipeline StatsPSO; 

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeStatsBuf;
        u32 ProbeStatsSRVSlot = kInvalidSlot;
        u32 ProbeStatsUAVSlot = kInvalidSlot;
        u32 NumProbes = 0;
        D3D12_RESOURCE_STATES ProbeStatsState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCBBase = nullptr;

        bool  Enabled     = false;
        EMode Mode        = EMode::Irradiance;
        f32   ProbeRadius = 0.10f; 
        bool  ShowVolume  = true;
        bool  ShowRays    = false; 
        f32   RayRadius   = 6.0f;  
    };
}
