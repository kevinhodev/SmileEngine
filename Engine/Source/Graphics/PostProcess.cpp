#include "Smile/Graphics/PostProcess.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/SwapChain.h"
#include <fstream>
#include <vector>
#include <stdexcept>
#include <algorithm>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadShaderBlob(const std::string& _Filename) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Filename;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir shader pos-processamento: " + FullPath);
                throw std::runtime_error("PostProcess shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }

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
        BloomWidth  = std::max(1u, Width / 4);
        BloomHeight = std::max(1u, Height / 4);

        BloomBuffer.Reset();
        BloomBlurBuffer.Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = BloomWidth;
        Desc.Height           = BloomHeight;
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

        SMILE_HR(Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&BloomBuffer)));

        SMILE_HR(Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&BloomBlurBuffer)));

        // Create RTVs
        if (!BloomRTVHeap.Native())
            BloomRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(BloomBuffer.Get(), &RTVDesc, BloomRTVHeap.CpuHandle(0));
        Device->CreateRenderTargetView(BloomBlurBuffer.Get(), &RTVDesc, BloomRTVHeap.CpuHandle(1));

        // Create SRVs (allocating contiguous table if not already allocated)
        if (BloomSRVSlot == kInvalidSlot)
            BloomSRVSlot = SRVHeap.Allocate(2);
        BloomBlurSRVSlot = BloomSRVSlot + 1;

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;

        SRVHeap.CreateSRV(Device, BloomBuffer.Get(), SRVDesc, BloomSRVSlot);
        SRVHeap.CreateSRV(Device, BloomBlurBuffer.Get(), SRVDesc, BloomBlurSRVSlot);

        // Update constant buffers with new texture sizes
        if (MappedBlurH) {
            MappedBlurH[0] = 1.0f / static_cast<float>(BloomWidth);
            MappedBlurH[1] = 0.0f;
        }
        if (MappedBlurV) {
            MappedBlurV[0] = 0.0f;
            MappedBlurV[1] = 1.0f / static_cast<float>(BloomHeight);
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
        auto VS = LoadShaderBlob("PostProcess.vs_6_0.cso");
        auto PSExtract = LoadShaderBlob("BloomExtract.ps_6_0.cso");
        auto PSBlur    = LoadShaderBlob("BloomBlur.ps_6_0.cso");
        auto PSTonemap = LoadShaderBlob("FinalTonemap.ps_6_0.cso");

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

        // 2. Bloom Blur PSO
        PSODesc.PS = { PSBlur.data(), PSBlur.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&BlurPSO)));

        // 3. Final Tonemap PSO (outputs to SDR backbuffer of SwapChain)
        PSODesc.PS = { PSTonemap.data(), PSTonemap.size() };
        PSODesc.RTVFormats[0] = FSwapChain::kFormat; // R8G8B8A8_UNORM
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TonemapPSO)));
    }

    void FPostProcessor::CreateConstantBuffers(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = 256;
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
        SMILE_HR(CBParams->Map(0, &NoRead, reinterpret_cast<void**>(&MappedParams)));
        
        // Default parameters: bloom intensity = 0.04f, exposure = 1.0f
        MappedParams->BloomIntensity = 0.04f;
        MappedParams->Exposure       = 1.0f;

        // CBBlurH
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CBBlurH)));
        SMILE_HR(CBBlurH->Map(0, &NoRead, reinterpret_cast<void**>(&MappedBlurH)));

        // CBBlurV
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CBBlurV)));
        SMILE_HR(CBBlurV->Map(0, &NoRead, reinterpret_cast<void**>(&MappedBlurV)));
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

        D3D12_VIEWPORT BloomVP{};
        BloomVP.Width    = static_cast<FLOAT>(BloomWidth);
        BloomVP.Height   = static_cast<FLOAT>(BloomHeight);
        BloomVP.MinDepth = 0.0f;
        BloomVP.MaxDepth = 1.0f;

        D3D12_RECT BloomScissor{};
        BloomScissor.right  = static_cast<LONG>(BloomWidth);
        BloomScissor.bottom = static_cast<LONG>(BloomHeight);

        // --- STEP 1: Bloom Extraction ---
        TransitionResource(CommandList, BloomBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        CommandList->SetGraphicsRootSignature(RootSig.Get());
        CommandList->SetPipelineState(ExtractPSO.Get());
        CommandList->RSSetViewports(1, &BloomVP);
        CommandList->RSSetScissorRects(1, &BloomScissor);

        auto BloomRTV0 = BloomRTVHeap.CpuHandle(0);
        CommandList->OMSetRenderTargets(1, &BloomRTV0, FALSE, nullptr);

        // Bind CBParams (though not strictly needed by extraction, keeps layout simple)
        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());
        // Bind HDR texture as input (slot t0)
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(HDRSRVSlot));

        // Draw fullscreen triangle
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 0, nullptr);
        CommandList->IASetIndexBuffer(nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, BloomBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // --- STEP 2: Bloom Horizontal Blur ---
        TransitionResource(CommandList, BloomBlurBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        CommandList->SetPipelineState(BlurPSO.Get());
        auto BloomRTV1 = BloomRTVHeap.CpuHandle(1);
        CommandList->OMSetRenderTargets(1, &BloomRTV1, FALSE, nullptr);

        // Bind Horizontal blur CB
        CommandList->SetGraphicsRootConstantBufferView(0, CBBlurH->GetGPUVirtualAddress());
        // Bind extracted BloomBuffer (slot t0)
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(BloomSRVSlot));

        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, BloomBlurBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // --- STEP 3: Bloom Vertical Blur ---
        TransitionResource(CommandList, BloomBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        CommandList->OMSetRenderTargets(1, &BloomRTV0, FALSE, nullptr);

        // Bind Vertical blur CB
        CommandList->SetGraphicsRootConstantBufferView(0, CBBlurV->GetGPUVirtualAddress());
        // Bind horizontally blurred BloomBlurBuffer (slot t0)
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(BloomBlurSRVSlot));

        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, BloomBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // --- STEP 4: Final Tonemap & Present ---
        // Output goes to SwapChain backbuffer RTV (passed in)
        CommandList->SetPipelineState(TonemapPSO.Get());
        CommandList->RSSetViewports(1, &FullVP);
        CommandList->RSSetScissorRects(1, &FullScissor);
        CommandList->OMSetRenderTargets(1, &SwapChainRTV, FALSE, nullptr);

        // Bind parameters (Exposure + BloomIntensity)
        CommandList->SetGraphicsRootConstantBufferView(0, CBParams->GetGPUVirtualAddress());

        // Bind table [HDRColorBuffer (t0), BloomBuffer (t1)].
        // Wait, HDRSRVSlot and BloomSRVSlot might not be contiguous in the heap!
        // In the root signature, t0 and t1 are declared in a single descriptor table of size 2.
        // D3D12 requires descriptor tables to point to CONTIGUOUS slots in the descriptor heap.
        // Therefore, we must copy HDRSRVSlot and BloomSRVSlot into a new contiguous table!
        // Let's reserve 2 contiguous slots inside the post processor, say PostTableStart.
        // Let's create it once at init, or every frame.
        // Since we copy descriptor handles, let's look at how CopyDescriptors is used in Renderer:
        // CopyDescriptors(NumDestDescriptorRanges, DstStarts, DstSizes, NumSrcDescriptorRanges, SrcStarts, SrcSizes, Type)
        // Let's implement this beautifully by allocating 2 contiguous slots in FTextureSRVHeap
        // once at init and copy them here before binding!
        // This is extremely safe and correct!
        
        static u32 PostTableStart = kInvalidSlot;
        if (PostTableStart == kInvalidSlot) {
            PostTableStart = SRVHeap.Allocate(2);
        }

        ID3D12Device* Device = nullptr;
        SMILE_HR(ResolvedHDR->GetDevice(IID_PPV_ARGS(&Device)));
        
        D3D12_CPU_DESCRIPTOR_HANDLE DstStart = SRVHeap.CpuHandle(PostTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Sources[2] = {
            SRVHeap.CpuHandle(HDRSRVSlot),
            SRVHeap.CpuHandle(BloomSRVSlot)
        };
        UINT DstCount = 2;
        UINT SrcCounts[2] = { 1, 1 };
        Device->CopyDescriptors(1, &DstStart, &DstCount,
                                 2, Sources, SrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Device->Release();

        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(PostTableStart));

        CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
