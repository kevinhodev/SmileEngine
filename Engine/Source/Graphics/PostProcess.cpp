#include "Smile/Graphics/PostProcess.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/SwapChain.h"
#include "Smile/Graphics/CommandQueue.h"
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <iterator>

namespace Smile {
    namespace {
        void TransitionResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            if (before == after) return;
            D3D12_RESOURCE_BARRIER b{};
            b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter  = after;
            cl->ResourceBarrier(1, &b);
        }
    }

    void FPostProcessor::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height) {
        if (Initialized) return;

        BuildRootSignature(Device);
        BuildPSOs(Device);
        CreateConstantBuffers(Device);
        CreateBloomTextures(Device, SRVHeap, Width, Height);

        Initialized = true;
        LogDebug("PostProcessor (HDR + Bloom + ACES Filmic) inicializado com sucesso");
    }

    void FPostProcessor::Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height) {
        if (!Initialized) return;
        CreateBloomTextures(Device, SRVHeap, Width, Height);
    }

    void FPostProcessor::CreateBloomTextures(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height) {
        BloomWidths[0]  = std::max(1u, Width / 4);
        BloomHeights[0] = std::max(1u, Height / 4);
        for (int i = 1; i < kNumBloomLevels; ++i) {
            BloomWidths[i]  = std::max(1u, BloomWidths[i-1] / 2);
            BloomHeights[i] = std::max(1u, BloomHeights[i-1] / 2);
        }

        for (int i = 0; i < kNumBloomLevels; ++i) {
            BloomBuffers[i].Reset();
        }
        for (int i = 0; i < kNumBloomLevels - 1; ++i) {
            BloomBlurBuffers[i].Reset();
        }

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        auto CreateBloomTarget = [&](int Level) {
            return GpuResources::CreateTex2D(
                Device, BloomWidths[Level], BloomHeights[Level], DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, EVramCategory::RenderTargets,
                &ClearValue, 1, 1, "Bloom");
        };

        for (int i = 0; i < kNumBloomLevels; ++i)
            BloomBuffers[i] = CreateBloomTarget(i);

        for (int i = 0; i < kNumBloomLevels - 1; ++i)
            BloomBlurBuffers[i] = CreateBloomTarget(i);

        if (!BloomRTVHeap.Native())
            BloomRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 9, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        
        for (int i = 0; i < kNumBloomLevels; ++i) {
            Device->CreateRenderTargetView(BloomBuffers[i].Get(), &RTVDesc, BloomRTVHeap.CpuHandle(i));
        }
        for (int i = 0; i < kNumBloomLevels - 1; ++i) {
            Device->CreateRenderTargetView(BloomBlurBuffers[i].Get(), &RTVDesc, BloomRTVHeap.CpuHandle(5 + i));
        }

        if (BloomSRVBase == kInvalidSlot)
            BloomSRVBase = SRVHeap.Allocate(17);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;

        for (int i = 0; i < kNumBloomLevels; ++i) {
            SRVHeap.CreateSRV(Device, BloomBuffers[i].Get(), SRVDesc, BloomSRVBase + i);
        }
        for (int i = 0; i < kNumBloomLevels - 1; ++i) {
            SRVHeap.CreateSRV(Device, BloomBlurBuffers[i].Get(), SRVDesc, BloomSRVBase + 5 + i);
        }

        SRVHeap.CreateSRV(Device, BloomBuffers[4].Get(), SRVDesc, BloomSRVBase + 9 + 3 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[3].Get(), SRVDesc, BloomSRVBase + 9 + 3 * 2 + 1);

        SRVHeap.CreateSRV(Device, BloomBlurBuffers[3].Get(), SRVDesc, BloomSRVBase + 9 + 2 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[2].Get(), SRVDesc, BloomSRVBase + 9 + 2 * 2 + 1);

        SRVHeap.CreateSRV(Device, BloomBlurBuffers[2].Get(), SRVDesc, BloomSRVBase + 9 + 1 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[1].Get(), SRVDesc, BloomSRVBase + 9 + 1 * 2 + 1);

        SRVHeap.CreateSRV(Device, BloomBlurBuffers[1].Get(), SRVDesc, BloomSRVBase + 9 + 0 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[0].Get(), SRVDesc, BloomSRVBase + 9 + 0 * 2 + 1);

        CachedHDRForTable = nullptr;

        if (MappedParamsBase) {
            for (int i = 1; i < kNumBloomLevels; ++i) {
                float* DParams = reinterpret_cast<float*>(MappedParamsBase + 256 + i * 256);
                DParams[0] = 1.0f / static_cast<float>(BloomWidths[i - 1]);
                DParams[1] = 1.0f / static_cast<float>(BloomHeights[i - 1]);
            }
            for (int i = 0; i < kNumBloomLevels - 1; ++i) {
                float* UParams = reinterpret_cast<float*>(MappedParamsBase + 256 + kNumBloomLevels * 256 + i * 256);
                UParams[0] = 0.0f; 
                UParams[1] = 0.0f;
                UParams[2] = 1.0f / static_cast<float>(BloomWidths[i]); 
            }
        }
    }

    void FPostProcessor::BuildRootSignature(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 2;
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER Params[2]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.ShaderRegister   = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(Params);
        Desc.pParameters       = Params;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("PostProcess Root Sig Error: ") +
                              static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(), IID_PPV_ARGS(&RootSig)));
    }

    void FPostProcessor::BuildPSOs(ID3D12Device* Device) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PSExtract = LoadShaderBytecode("BloomExtract.ps_6_0.cso");
        auto PSDownsample = LoadShaderBytecode("BloomDownsample.ps_6_0.cso");
        auto PSUpsample   = LoadShaderBytecode("BloomUpsample.ps_6_0.cso");
        auto PSTonemap = LoadShaderBytecode("FinalTonemap.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = FALSE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        PSODesc.SampleDesc            = { 1, 0 };

        PSODesc.PS = { PSExtract.data(), PSExtract.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&ExtractPSO)));

        PSODesc.PS = { PSDownsample.data(), PSDownsample.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&DownsamplePSO)));

        PSODesc.PS = { PSUpsample.data(), PSUpsample.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&UpsamplePSO)));

        PSODesc.PS = { PSTonemap.data(), PSTonemap.size() };
        PSODesc.RTVFormats[0] = FSwapChain::kFormat; 
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TonemapPSO)));
    }

    void FPostProcessor::CreateConstantBuffers(ID3D12Device* Device) {
        // Bloco unico de 4 KB com layout proprio (params + downsample/upsample por nivel, em
        // passos de 256 escritos a mao no CreateBloomTextures), nao um slice por frame — dai
        // SliceCount 1 e o alinhamento de CBV desligado.
        const GpuResources::FUploadBuffer Upload =
            GpuResources::CreateUploadBuffer(Device, 4096, 1, false);
        CBParams         = Upload.Resource;
        MappedParamsBase = Upload.Mapped;

        MappedParams = reinterpret_cast<PostParams*>(MappedParamsBase);

        MappedParams->BloomIntensity = 0.04f;
        MappedParams->Exposure       = 1.5f;
    }

    void FPostProcessor::Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                                 ID3D12Resource* ResolvedHDR, D3D12_CPU_DESCRIPTOR_HANDLE SwapChainRTV,
                                 u32 HDRSRVSlot, u32 FrameSlot, u32 Width, u32 Height) {
        if (!Initialized) return;

        D3D12_VIEWPORT FullVP{};
        FullVP.Width    = static_cast<FLOAT>(Width);
        FullVP.Height   = static_cast<FLOAT>(Height);
        FullVP.MinDepth = 0.0f;
        FullVP.MaxDepth = 1.0f;

        D3D12_RECT FullScissor{};
        FullScissor.right  = static_cast<LONG>(Width);
        FullScissor.bottom = static_cast<LONG>(Height);

        CommandList->SetGraphicsRootSignature(RootSig.Get());

        TransitionResource(CommandList, BloomBuffers[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_VIEWPORT VP0{};
        VP0.Width    = static_cast<FLOAT>(BloomWidths[0]);
        VP0.Height   = static_cast<FLOAT>(BloomHeights[0]);
        VP0.MinDepth = 0.0f;
        VP0.MaxDepth = 1.0f;

        D3D12_RECT Scissor0{};
        Scissor0.right  = static_cast<LONG>(BloomWidths[0]);
        Scissor0.bottom = static_cast<LONG>(BloomHeights[0]);

        CommandList->SetPipelineState(ExtractPSO.Get());
        CommandList->RSSetViewports(1, &VP0);
        CommandList->RSSetScissorRects(1, &Scissor0);

        auto BloomRTV0 = BloomRTVHeap.CpuHandle(0);
        CommandList->OMSetRenderTargets(1, &BloomRTV0, FALSE, nullptr);

        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(HDRSRVSlot));

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 0, nullptr);
        CommandList->IASetIndexBuffer(nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, BloomBuffers[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        CommandList->SetPipelineState(DownsamplePSO.Get());
        for (int i = 1; i < kNumBloomLevels; ++i) {
            TransitionResource(CommandList, BloomBuffers[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_VIEWPORT VP{};
            VP.Width    = static_cast<FLOAT>(BloomWidths[i]);
            VP.Height   = static_cast<FLOAT>(BloomHeights[i]);
            VP.MinDepth = 0.0f;
            VP.MaxDepth = 1.0f;

            D3D12_RECT Scissor{};
            Scissor.right  = static_cast<LONG>(BloomWidths[i]);
            Scissor.bottom = static_cast<LONG>(BloomHeights[i]);

            CommandList->RSSetViewports(1, &VP);
            CommandList->RSSetScissorRects(1, &Scissor);

            auto RTV = BloomRTVHeap.CpuHandle(i);
            CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);

            u64 CBOffset = 256 + i * 256;
            CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress() + CBOffset);
            CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(BloomSRVBase + (i - 1)));

            CommandList->DrawInstanced(3, 1, 0, 0);

            TransitionResource(CommandList, BloomBuffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        CommandList->SetPipelineState(UpsamplePSO.Get());
        for (int i = kNumBloomLevels - 2; i >= 0; --i) {
            TransitionResource(CommandList, BloomBlurBuffers[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_VIEWPORT VP{};
            VP.Width    = static_cast<FLOAT>(BloomWidths[i]);
            VP.Height   = static_cast<FLOAT>(BloomHeights[i]);
            VP.MinDepth = 0.0f;
            VP.MaxDepth = 1.0f;

            D3D12_RECT Scissor{};
            Scissor.right  = static_cast<LONG>(BloomWidths[i]);
            Scissor.bottom = static_cast<LONG>(BloomHeights[i]);

            CommandList->RSSetViewports(1, &VP);
            CommandList->RSSetScissorRects(1, &Scissor);

            auto RTV = BloomRTVHeap.CpuHandle(5 + i);
            CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);

            u64 CBOffset = 256 + kNumBloomLevels * 256 + i * 256;
            CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress() + CBOffset);

            u32 TableSlot = BloomSRVBase + 9 + i * 2;
            CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(TableSlot));

            CommandList->DrawInstanced(3, 1, 0, 0);

            TransitionResource(CommandList, BloomBlurBuffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        CommandList->SetPipelineState(TonemapPSO.Get());
        CommandList->RSSetViewports(1, &FullVP);
        CommandList->RSSetScissorRects(1, &FullScissor);
        CommandList->OMSetRenderTargets(1, &SwapChainRTV, FALSE, nullptr);

        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());

        if (PostTableStart == kInvalidSlot)
            PostTableStart = SRVHeap.Allocate(2 * FCommandQueue::kFramesInFlight);

        const u32 TableSlot = PostTableStart + FrameSlot * 2;
        {
            ID3D12Device* Device = nullptr;
            SMILE_HR(ResolvedHDR->GetDevice(IID_PPV_ARGS(&Device)));

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.Texture2D.MipLevels     = 1;

            SRVHeap.CreateSRV(Device, ResolvedHDR, SRVDesc, TableSlot); 
            SRVHeap.CreateSRV(Device, BloomBlurBuffers[0].Get(), SRVDesc, TableSlot + 1); 
            Device->Release();
        }

        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(TableSlot));

        CommandList->DrawInstanced(3, 1, 0, 0);
    }

    FPassShaderStems FPostProcessor::ShaderStems() const {
        static const char* const kStems[] = { "BloomExtract.ps", "BloomDownsample.ps", "BloomUpsample.ps", "BloomBlur.ps", "FinalTonemap.ps", "PostProcess.vs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FPostProcessor::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSOs(_Ctx.Device);
    }

}
