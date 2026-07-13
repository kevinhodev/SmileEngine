#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Camada artistica de sun shafts. Extrai uma mascara HDR/depth em quarter-res,
    // aplica tres blurs radiais e compoe aditivamente antes do TAA/FSR2. Fica
    // deliberadamente separada do volume fisico em FSunShafts.
    struct alignas(256) SunShaftBloomConstants {
        Vec4 SunPosParams;  // xy=sun UV, z=fade, w=target aspect H/W
        Vec4 BloomParams;   // x=threshold, y=soft knee, z=max brightness, w=intensity
        Vec4 BlurParams;    // x=radial distance, y=source radius, z=jitter, w=unused
        Vec4 TintDepth;     // rgb=normalized key tint, w=1/occlusion distance
        Vec4 ScreenParams;  // xy=target dimensions, zw=reciprocal dimensions
        Mat44 InvViewProj;
        Vec4 CameraWorldPos;
    };
    static_assert(sizeof(SunShaftBloomConstants) == 256);

    class FSunShaftBloom {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        DXGI_FORMAT HDRFormat, u32 RenderWidth, u32 RenderHeight);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    u32 RenderWidth, u32 RenderHeight);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& ViewProjUnjittered,
                            const Mat44& InvViewProj, const Vec3& CameraWorldPos,
                            const Vec3& DirectionToLight, const Vec3& LightColor,
                            f32 LightIntensity, u32 FrameIndex, bool Active);

        // HDR e depth chegam como SRV; ao fim, o resultado quarter-res esta legivel.
        void RecordMaskAndBlur(ID3D12GraphicsCommandList* CommandList,
                               FTextureSRVHeap& SRVHeap,
                               u32 HDRSRVSlot, u32 DepthSRVSlot);
        // O caller fornece o HDR novamente como RTV. A composicao e aditiva.
        void Composite(ID3D12GraphicsCommandList* CommandList,
                       FTextureSRVHeap& SRVHeap, u32 FullWidth, u32 FullHeight);

        bool IsInitialized() const { return Initialized; }
        bool HasWork() const { return Initialized && Active && Fade > 0.001f; }

        void SetEnabled(bool V) { Enabled = V; }
        bool GetEnabled() const { return Enabled; }
        void SetIntensity(f32 V) { Intensity = V; }
        f32 GetIntensity() const { return Intensity; }
        void SetThreshold(f32 V) { Threshold = V; }
        f32 GetThreshold() const { return Threshold; }
        void SetRayLength(f32 V) { RayLength = V; }
        f32 GetRayLength() const { return RayLength; }
        void SetOcclusionDistance(f32 V) { OcclusionDistance = V; }
        f32 GetOcclusionDistance() const { return OcclusionDistance; }

    private:
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kBlurPasses  = 3;
        static constexpr u32 kCBRegions   = 2 + kBlurPasses; // mask, 3 blur, apply

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT HDRFormat);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateTargets(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           u32 RenderWidth, u32 RenderHeight);
        void Transition(ID3D12GraphicsCommandList* CommandList, u32 Index,
                        D3D12_RESOURCE_STATES After);

        SunShaftBloomConstants* Region(u32 Pass) const;
        D3D12_GPU_VIRTUAL_ADDRESS RegionAddress(u32 Pass) const;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> BlurPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;
        Microsoft::WRL::ComPtr<ID3D12Resource> Targets[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        FDescriptorHeap RTVHeap;

        u8* MappedBase = nullptr;
        u32 SRVSlots[2] = { kInvalidSlot, kInvalidSlot };
        D3D12_RESOURCE_STATES States[2] = {
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        };
        u32 Width = 1;
        u32 Height = 1;
        u32 FrameSlot = 0;
        u32 FinalTarget = 1;
        f32 Fade = 0.0f;
        bool Active = false;
        bool Enabled = true;
        bool Initialized = false;

        f32 Intensity = 0.35f;
        f32 Threshold = 1.0f;
        f32 SoftKnee = 0.5f;
        f32 MaxBrightness = 25.0f;
        f32 RayLength = 0.08f;
        f32 SourceRadius = 0.45f;
        f32 OcclusionDistance = 1000.0f;
    };
}
