#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    struct alignas(256) PostParams {
        f32 BloomIntensity;
        f32 Exposure;
        f32 Padding[2];
    };

    class FPostProcessor : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Pós (bloom+tonemap)"; }
        bool IsInitialized() const override { return Initialized; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     ID3D12Resource* ResolvedHDR, D3D12_CPU_DESCRIPTOR_HANDLE SwapChainRTV,
                     u32 HDRSRVSlot, u32 FrameSlot, u32 Width, u32 Height);
        void SetBloomIntensity(f32 Intensity) { if (MappedParams) MappedParams->BloomIntensity = Intensity; }
        f32  GetBloomIntensity() const        { return MappedParams ? MappedParams->BloomIntensity : 0.04f; }
        void SetExposure(f32 Exposure)        { if (MappedParams) MappedParams->Exposure = Exposure; }
        f32  GetExposure() const              { return MappedParams ? MappedParams->Exposure : 1.0f; }

    private:
        void CreateBloomTextures(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ExtractPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DownsamplePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> UpsamplePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> TonemapPSO;

        static constexpr int kNumBloomLevels = 5;
        Microsoft::WRL::ComPtr<ID3D12Resource> BloomBuffers[kNumBloomLevels];
        Microsoft::WRL::ComPtr<ID3D12Resource> BloomBlurBuffers[kNumBloomLevels - 1];
        FDescriptorHeap        BloomRTVHeap;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 BloomSRVBase = kInvalidSlot; 

        u32 PostTableStart = kInvalidSlot;
        ID3D12Resource* CachedHDRForTable = nullptr;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBParams; 
        u8* MappedParamsBase = nullptr;
        PostParams* MappedParams = nullptr;

        u32 BloomWidths[kNumBloomLevels] = {};
        u32 BloomHeights[kNumBloomLevels] = {};
        bool Initialized = false;
    };
}
