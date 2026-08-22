#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Resources/VolumeTexture.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <string>

namespace Smile {
    class FCommandQueue;
    class FUploadQueue;

    class FCloudNoise : public FPipelineOwner {
    public:
        // --- Dono de pipeline (RenderPass.h): tem shader, mas NAO grava frame ---
        const char* Name() const override { return "Ruido das nuvens (bake)"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        static constexpr u32 kBaseRes    = 128;
        static constexpr u32 kDetailRes  = 32;
        static constexpr u32 kWeatherRes = 512;

        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap);

        // Re-bakea SO o weather map procedural com os parametros atuais (seed/celulas).
        // Chamador garante GPU ociosa (Flush) antes.
        void RebakeWeather(FCommandQueue& CmdQueue, FTextureSRVHeap& SRVHeap);

        // Weather map autorado (textura pintada, canais R=cobertura G=tipo B=peak height)
        // substitui o procedural; ClearWeatherOverride volta ao bakeado.
        bool LoadWeatherOverride(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                 FTextureSRVHeap& SRVHeap, const std::wstring& Path);
        void ClearWeatherOverride(FTextureSRVHeap& SRVHeap);
        bool HasWeatherOverride() const { return OverrideActive; }

        void SetSeed(u32 V)     { WeatherSeed = V; }
        u32  GetSeed() const    { return WeatherSeed; }
        void SetCellMult(u32 V) { WeatherCellMult = V < 1u ? 1u : (V > 8u ? 8u : V); }
        u32  GetCellMult() const{ return WeatherCellMult; }

        u32  BaseNoiseSRV()   const { return BaseNoise.SRVSlot(); }
        u32  DetailNoiseSRV() const { return DetailNoise.SRVSlot(); }
        u32  WeatherSRV()     const { return OverrideActive ? WeatherOverride.SRVSlot()
                                                            : WeatherSRVSlot; }
        bool IsInitialized()  const override { return Initialized; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void Bake(FCommandQueue& CmdQueue, FTextureSRVHeap& SRVHeap);
        void RecordWeatherBake(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        FVolumeTexture   BaseNoise;
        FVolumeTexture   DetailNoise;
        FComputePipeline BaseNoisePSO;
        FComputePipeline DetailNoisePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> WeatherTex;
        u32 WeatherSRVSlot = 0;
        u32 WeatherUAVSlot = 0;
        D3D12_RESOURCE_STATES WeatherState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        FComputePipeline WeatherPSO;

        u32 WeatherSeed     = 1337;
        u32 WeatherCellMult = 3;

        FTexture WeatherOverride;
        bool     OverrideActive = false;

        bool Initialized = false;
    };
}
