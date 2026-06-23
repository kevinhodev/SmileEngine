#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FCloudNoise;

    struct alignas(256) CloudConstants {
        Mat44 InvViewProjNoTrans;
        Vec4  CameraPos;    // xyz = camera (atmosphere km-frame), w = view height
        Vec4  SunDir;       // xyz = direction TO sun, w = sun intensity
        Vec4  SunColor;     // rgb, w unused
        Vec4  PlanetRadii;  // x = bottomR, y = cloudInnerR, z = cloudOuterR (km)
        Vec4  CloudParams;  // x = coverage, y = densityScale, z = noiseScale, w = time
        Vec4  CloudParams2; // x = weatherScale, y = erosionStrength, z = detailScale, w = cloudTypeBias
        Vec4  WindParams;   // xyz = wind velocity (km/s), w unused
        Vec4  MarchParams;  // x = primary steps, y = light steps, z = ambient strength
        Vec4  ScreenParams; // x = rtW, y = rtH, z = 1/rtW, w = 1/rtH
        Vec4  PhaseParams;  // x = g1, y = g2, z = blend, w = powderStrength
        Vec4  AtmoLink;     // x = atmoTopR, y = msOctaves, z = ambientScale, w unused
    };

    class FVolumetricClouds {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        FCloudNoise& Noise, u32 AtmoTransmittanceSRV, u32 AtmoMultiScatterSRV,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat,
                        u32 Width, u32 Height);

        void RecreateComposite(ID3D12Device* Device,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    u32 Width, u32 Height);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjNoTranslation, f32 ViewHeightKm,
                            const Vec3& DirToSun, const Vec3& SunColor, f32 Time,
                            u32 Width, u32 Height);

        void RecordRaymarch(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);
        void Composite(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        bool IsInitialized() const { return Initialized; }

        void SetCoverage(f32 V)         { CPUConstants.CloudParams.X  = V; }
        void SetDensityScale(f32 V)     { CPUConstants.CloudParams.Y  = V; }
        void SetWindSpeed(f32 V)        { CPUConstants.WindParams.X = V; CPUConstants.WindParams.Z = V * 0.4f; }
        void SetErosion(f32 V)          { CPUConstants.CloudParams2.Y = V; }
        void SetPhaseG(f32 V)           { CPUConstants.PhaseParams.X  = V; }
        void SetPowder(f32 V)           { CPUConstants.PhaseParams.W  = V; }
        void SetAltitude(f32 BottomKm, f32 ThicknessKm) {
            CPUConstants.PlanetRadii.Y = CPUConstants.PlanetRadii.X + BottomKm;
            CPUConstants.PlanetRadii.Z = CPUConstants.PlanetRadii.X + BottomKm + ThicknessKm;
        }
        f32  GetCoverage() const       { return CPUConstants.CloudParams.X; }
        f32  GetDensityScale() const   { return CPUConstants.CloudParams.Y; }
        f32  GetWindSpeed() const      { return CPUConstants.WindParams.X; }
        f32  GetErosion() const        { return CPUConstants.CloudParams2.Y; }
        f32  GetPhaseG() const         { return CPUConstants.PhaseParams.X; }
        f32  GetPowder() const         { return CPUConstants.PhaseParams.W; }
        f32  GetBottomAltitude() const { return CPUConstants.PlanetRadii.Y - CPUConstants.PlanetRadii.X; }
        f32  GetThickness() const      { return CPUConstants.PlanetRadii.Z - CPUConstants.PlanetRadii.Y; }

    private:
        void CreateRT(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void TransitionRT(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);
        void CreateConstantBuffer(ID3D12Device* Device);
        void BuildNoiseTable(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, FCloudNoise& Noise,
                             u32 AtmoTransmittanceSRV, u32 AtmoMultiScatterSRV);
        void BuildCompositeRootSignature(ID3D12Device* Device);
        void BuildCompositePSO(ID3D12Device* Device,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        FVolumetricPipeline RaymarchPSO;        
        u32 NoiseTableStart = 0;                

        Microsoft::WRL::ComPtr<ID3D12Resource> CloudRT;
        u32 RTSRVSlot = kInvalidSlot;
        u32 RTUAVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES RTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        u32 RTWidth = 0, RTHeight = 0;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8*             MappedBase = nullptr;
        u32             FrameSlot  = 0;
        CloudConstants  CPUConstants{};

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CloudConstants);
        }
        CloudConstants* Mapped() const {
            return reinterpret_cast<CloudConstants*>(
                MappedBase + static_cast<size_t>(FrameSlot) * sizeof(CloudConstants));
        }

        bool Initialized = false;
    };
}
