#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/VolumeTexture.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>

namespace Smile {
    class FCommandQueue;

    // Bakes the two cloud noise volumes once on the GPU at startup (same pattern
    // as the IBL / atmosphere bakes): a 128^3 Perlin-Worley base-shape volume and
    // a 32^3 Worley detail/erosion volume. Stored as RGBA16F (UAV-write proven by
    // the IBL/atmosphere paths). Both rest in NON_PIXEL_SHADER_RESOURCE for the
    // cloud raymarch to sample.
    class FCloudNoise {
    public:
        static constexpr u32 kBaseRes    = 128;
        static constexpr u32 kDetailRes  = 32;
        static constexpr u32 kWeatherRes = 512;

        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap);

        u32  BaseNoiseSRV()   const { return BaseNoise.SRVSlot(); }
        u32  DetailNoiseSRV() const { return DetailNoise.SRVSlot(); }
        u32  WeatherSRV()     const { return WeatherSRVSlot; }
        bool IsInitialized()  const { return Initialized; }

    private:
        void Bake(FCommandQueue& CmdQueue, FTextureSRVHeap& SRVHeap);

        FVolumeTexture   BaseNoise;
        FVolumeTexture   DetailNoise;
        FComputePipeline BaseNoisePSO;
        FComputePipeline DetailNoisePSO;

        // 2D weather map (R = coverage, G = type, B = wetness).
        Microsoft::WRL::ComPtr<ID3D12Resource> WeatherTex;
        u32 WeatherSRVSlot = 0;
        u32 WeatherUAVSlot = 0;
        D3D12_RESOURCE_STATES WeatherState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        FComputePipeline WeatherPSO;

        bool Initialized = false;
    };
}
