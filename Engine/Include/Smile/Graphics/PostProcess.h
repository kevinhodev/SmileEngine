#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    struct alignas(256) PostParams {
        f32 BloomIntensity;
        f32 Exposure;
        f32 Padding[2];
    };

    class FPostProcessor {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     ID3D12Resource* ResolvedHDR, D3D12_CPU_DESCRIPTOR_HANDLE SwapChainRTV,
                     u32 HDRSRVSlot, u32 Width, u32 Height);

    private:
        void CreateBloomTextures(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ExtractPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> BlurPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> TonemapPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> BloomBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> BloomBlurBuffer;
        FDescriptorHeap        BloomRTVHeap;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 BloomSRVSlot = kInvalidSlot;
        u32 BloomBlurSRVSlot = kInvalidSlot;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBParams; // For PostParams (Exposure, BloomIntensity)
        PostParams* MappedParams = nullptr;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBBlurH;  // Direction H (1/w, 0)
        Microsoft::WRL::ComPtr<ID3D12Resource> CBBlurV;  // Direction V (0, 1/h)
        float* MappedBlurH = nullptr;
        float* MappedBlurV = nullptr;

        u32 BloomWidth = 0;
        u32 BloomHeight = 0;
        bool Initialized = false;
    };
}
