#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/VolumeTexture.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>

namespace Smile {
    class FCommandQueue;

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

        Microsoft::WRL::ComPtr<ID3D12Resource> WeatherTex;
        u32 WeatherSRVSlot = 0;
        u32 WeatherUAVSlot = 0;
        D3D12_RESOURCE_STATES WeatherState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        FComputePipeline WeatherPSO;

        bool Initialized = false;
    };
}
