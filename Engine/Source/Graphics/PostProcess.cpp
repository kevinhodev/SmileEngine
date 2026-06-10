#include "Smile/Graphics/PostProcess.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/SwapChain.h"
#include <vector>
#include <stdexcept>
#include <algorithm>

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
        LogInfo("PostProcessor (HDR + Bloom + ACES Filmic) inicializado com sucesso");
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

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Clear values for RT
        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        for (int i = 0; i < kNumBloomLevels; ++i) {
            Desc.Width  = BloomWidths[i];
            Desc.Height = BloomHeights[i];
            SMILE_HR(Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
                IID_PPV_ARGS(&BloomBuffers[i])));
        }

        for (int i = 0; i < kNumBloomLevels - 1; ++i) {
            Desc.Width  = BloomWidths[i];
            Desc.Height = BloomHeights[i];
            SMILE_HR(Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
                IID_PPV_ARGS(&BloomBlurBuffers[i])));
        }

        // Create RTVs (5 for BloomBuffers, 4 for BloomBlurBuffers)
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

        // Create SRVs (allocating contiguous table of size 17: 5 BloomBuffers + 4 BloomBlurBuffers + 8 upsample tables)
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

        // Upsample tables: 2 SRVs each (t0 = LowRes, t1 = HighRes)
        // Up 3 (i=3): LowRes = BloomBuffers[4] (slot 4), HighRes = BloomBuffers[3] (slot 3)
        SRVHeap.CreateSRV(Device, BloomBuffers[4].Get(), SRVDesc, BloomSRVBase + 9 + 3 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[3].Get(), SRVDesc, BloomSRVBase + 9 + 3 * 2 + 1);

        // Up 2 (i=2): LowRes = BloomBlurBuffers[3] (slot 8), HighRes = BloomBuffers[2] (slot 2)
        SRVHeap.CreateSRV(Device, BloomBlurBuffers[3].Get(), SRVDesc, BloomSRVBase + 9 + 2 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[2].Get(), SRVDesc, BloomSRVBase + 9 + 2 * 2 + 1);

        // Up 1 (i=1): LowRes = BloomBlurBuffers[2] (slot 7), HighRes = BloomBuffers[1] (slot 1)
        SRVHeap.CreateSRV(Device, BloomBlurBuffers[2].Get(), SRVDesc, BloomSRVBase + 9 + 1 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[1].Get(), SRVDesc, BloomSRVBase + 9 + 1 * 2 + 1);

        // Up 0 (i=0): LowRes = BloomBlurBuffers[1] (slot 6), HighRes = BloomBuffers[0] (slot 0)
        SRVHeap.CreateSRV(Device, BloomBlurBuffers[1].Get(), SRVDesc, BloomSRVBase + 9 + 0 * 2);
        SRVHeap.CreateSRV(Device, BloomBuffers[0].Get(), SRVDesc, BloomSRVBase + 9 + 0 * 2 + 1);

        // Force tonemap table to update next frame
        CachedHDRForTable = nullptr;

        // Update constant buffers with new texture sizes
        if (MappedParamsBase) {
            // Downsample params: Down 1 (L0 -> L1, i=1) uses L0 texel size.
            for (int i = 1; i < kNumBloomLevels; ++i) {
                float* DParams = reinterpret_cast<float*>(MappedParamsBase + 256 + i * 256);
                DParams[0] = 1.0f / static_cast<float>(BloomWidths[i - 1]);
                DParams[1] = 1.0f / static_cast<float>(BloomHeights[i - 1]);
            }
            // Upsample params: Up i (L[i+1] -> L[i]) uses FilterRadius relative to L[i]
            for (int i = 0; i < kNumBloomLevels - 1; ++i) {
                float* UParams = reinterpret_cast<float*>(MappedParamsBase + 256 + kNumBloomLevels * 256 + i * 256);
                UParams[0] = 0.0f; // unused padding
                UParams[1] = 0.0f; // unused padding
                UParams[2] = 1.0f / static_cast<float>(BloomWidths[i]); // FilterRadius
            }
        }
    }

    void FPostProcessor::BuildRootSignature(ID3D12Device* Device) {
        // [0] CBV b0: PostParams or Blur Params
        // [1] Table t0..t1: input texture SRVs

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

        // Static linear clamp sampler
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
        Depth.DepthEnable    = FALSE; // no depth testing for fullscreen post-process
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
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // Outputting to HDR Bloom target
        PSODesc.SampleDesc            = { 1, 0 };

        // 1. Bloom Extract PSO
        PSODesc.PS = { PSExtract.data(), PSExtract.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&ExtractPSO)));

        // 2. Bloom Downsample PSO
        PSODesc.PS = { PSDownsample.data(), PSDownsample.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&DownsamplePSO)));

        // 3. Bloom Upsample PSO
        PSODesc.PS = { PSUpsample.data(), PSUpsample.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&UpsamplePSO)));

        // 4. Final Tonemap PSO (outputs to SDR backbuffer of SwapChain)
        PSODesc.PS = { PSTonemap.data(), PSTonemap.size() };
        PSODesc.RTVFormats[0] = FSwapChain::kFormat; // R8G8B8A8_UNORM
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TonemapPSO)));
    }

    void FPostProcessor::CreateConstantBuffers(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = 4096; // Extended to fit PostParams and Downsample/Upsample level params
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        // CBParams: PostParams
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CBParams)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CBParams->Map(0, &NoRead, reinterpret_cast<void**>(&MappedParamsBase)));
        
        MappedParams = reinterpret_cast<PostParams*>(MappedParamsBase);

        // Default parameters: bloom intensity = 0.04f, exposure = 1.0f
        MappedParams->BloomIntensity = 0.04f;
        MappedParams->Exposure       = 1.0f;
    }

    void FPostProcessor::Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                                 ID3D12Resource* ResolvedHDR, D3D12_CPU_DESCRIPTOR_HANDLE SwapChainRTV,
                                 u32 HDRSRVSlot, u32 Width, u32 Height) {
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

        // --- STEP 1: Bloom Extraction ---
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

        // Bind CBParams (tonemap settings at offset 0)
        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());
        // Bind HDR texture as input (slot t0)
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(HDRSRVSlot));

        // Draw fullscreen triangle
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 0, nullptr);
        CommandList->IASetIndexBuffer(nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, BloomBuffers[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // --- STEP 2: Downsampling Chain ---
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

            // Bind constant buffer view for downsample level parameters
            u64 CBOffset = 256 + i * 256;
            CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress() + CBOffset);
            // Bind BloomBuffers[i - 1] as input
            CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(BloomSRVBase + (i - 1)));

            CommandList->DrawInstanced(3, 1, 0, 0);

            TransitionResource(CommandList, BloomBuffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // --- STEP 3: Upsampling Chain ---
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

            // Bind constant buffer view for upsample level parameters
            u64 CBOffset = 256 + kNumBloomLevels * 256 + i * 256;
            CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress() + CBOffset);

            // Bind the descriptor table for upsample level inputs (t0 = LowRes, t1 = HighRes)
            u32 TableSlot = BloomSRVBase + 9 + i * 2;
            CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(TableSlot));

            CommandList->DrawInstanced(3, 1, 0, 0);

            TransitionResource(CommandList, BloomBlurBuffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // --- STEP 4: Final Tonemap & Present ---
        CommandList->SetPipelineState(TonemapPSO.Get());
        CommandList->RSSetViewports(1, &FullVP);
        CommandList->RSSetScissorRects(1, &FullScissor);
        CommandList->OMSetRenderTargets(1, &SwapChainRTV, FALSE, nullptr);

        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());

        if (PostTableStart == kInvalidSlot)
            PostTableStart = SRVHeap.Allocate(2);

        if (ResolvedHDR != CachedHDRForTable) {
            ID3D12Device* Device = nullptr;
            SMILE_HR(ResolvedHDR->GetDevice(IID_PPV_ARGS(&Device)));

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.Texture2D.MipLevels     = 1;

            SRVHeap.CreateSRV(Device, ResolvedHDR, SRVDesc, PostTableStart); // t0 = HDR
            SRVHeap.CreateSRV(Device, BloomBlurBuffers[0].Get(), SRVDesc, PostTableStart + 1); // t1 = Bloom (Blur0)
            Device->Release();

            CachedHDRForTable = ResolvedHDR;
        }

        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(PostTableStart));

        CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
