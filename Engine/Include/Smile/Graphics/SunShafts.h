#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumeTexture.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Sun shafts volumetricos em froxels. O primeiro compute avalia densidade,
    // fase HG e visibilidade no CSM; o segundo integra scattering/transmitancia
    // ao longo de Z. A historia e reprojetada no volume antes da integracao.
    struct alignas(256) SunShaftConstants {
        Mat44 InvViewProj;
        Mat44 PrevViewProj;
        Vec4  CameraWorldPos;       // xyz=current camera, w=history weight
        Vec4  SunDirectionIntensity;// xyz=direction TO light, w=radiance scale
        Vec4  SunColorPhase;        // rgb=light color, w=HG anisotropy g
        Vec4  FogLayer1;            // x=density, y=height falloff, z=height
        Vec4  FogLayer2;            // x=density, y=height falloff, z=height, w=far Z
        Vec4  GridParams;           // xyz=grid dimensions, w=max view depth
        Vec4  TemporalParams;       // xyz=cell jitter [0,1], w=history valid
    };
    static_assert(sizeof(SunShaftConstants) == 256);

    class FSunShafts {
    public:
        static constexpr u32 kGridPixelSize = 16;
        static constexpr u32 kGridSizeZ     = 64;

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderWidth, u32 RenderHeight);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    u32 RenderWidth, u32 RenderHeight);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjUnjittered,
                            const Mat44& PrevViewProjUnjittered,
                            const Vec3& CameraWorldPos, const Vec3& DirToLight,
                            const Vec3& LightColor, f32 LightIntensity,
                            const Vec4& FogLayer1, const Vec4& FogLayer2,
                            f32 FarZ, const Vec3& Jitter, bool Active);

        void Record(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                    D3D12_GPU_VIRTUAL_ADDRESS CSMConstants,
                    u32 ShadowMapSRVSlot);
        void EnsureReadable(ID3D12GraphicsCommandList* CommandList) {
            if (Initialized)
                Integrated.Transition(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        bool IsInitialized() const { return Initialized; }
        bool HasWork()       const { return Initialized && Active; }
        u32  IntegratedSRV() const { return Integrated.SRVSlot(); }
        f32  GetMaxDistance() const { return MaxDistance; }

        void SetIntensity(f32 V)   { Intensity = V; InvalidateHistory(); }
        f32  GetIntensity() const  { return Intensity; }
        void SetAnisotropy(f32 V)  { Anisotropy = V; InvalidateHistory(); }
        f32  GetAnisotropy() const { return Anisotropy; }
        void SetMaxDistance(f32 V) { MaxDistance = V; InvalidateHistory(); }
        void SetHistoryWeight(f32 V) { HistoryWeight = V; }
        f32  GetHistoryWeight() const { return HistoryWeight; }
        void InvalidateHistory() { HistoryValid = false; }

    private:
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);
        void RefreshDescriptorTables(FTextureSRVHeap& SRVHeap);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddress() const;

        ID3D12Device* NativeDevice = nullptr;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> IntegratePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedBase = nullptr;
        u32 FrameSlot = 0;

        FVolumeTexture Scattering[2];
        FVolumeTexture Integrated;
        u32 ScatterTable[2]   = { kInvalidSlot, kInvalidSlot };
        u32 IntegrateTable[2] = { kInvalidSlot, kInvalidSlot };

        u32 GridWidth  = 1;
        u32 GridHeight = 1;
        u64 FrameCounter = 0;

        Vec3 LastCameraPosition{};
        bool LastCameraValid = false;
        bool HistoryValid = false;
        bool Active = false;
        bool Initialized = false;

        f32 Intensity = 0.20f;
        f32 Anisotropy = 0.70f;
        f32 MaxDistance = 800.0f;
        f32 HistoryWeight = 0.90f;
    };
}
