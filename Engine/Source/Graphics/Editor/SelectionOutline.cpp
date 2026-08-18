#include "Smile/Graphics/Editor/SelectionOutline.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/SwapChain.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace Smile {
    static constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8_UNORM;
    static constexpr u32 kFIF = FCommandQueue::kFramesInFlight;

    void FSelectionOutline::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                       u32 Width, u32 Height) {
        if (Initialized) return;
        BuildRootSignatures(Device);
        BuildPSOs(Device);
        CreateConstantBuffer(Device);
        CreateTargets(Device, SRVHeap, Width, Height);
        Initialized = true;
        LogDebug("FSelectionOutline (mascara + edge-detect) inicializado");
    }

    void FSelectionOutline::CreateTargets(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                          u32 Width, u32 Height) {
        MaskW = std::max(1u, Width)  * kMaskSS;
        MaskH = std::max(1u, Height) * kMaskSS;
        Width  = MaskW;
        Height = MaskH;
        MaskTarget.Reset();

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format = kMaskFormat;

        MaskTarget = GpuResources::CreateTex2D(
            Device, Width, Height, kMaskFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET, EVramCategory::Misc, &Clear,
            1, 1, "Mascara de outline");
        MaskState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        if (MaskRTVHeap.GetCapacity() == 0)
            MaskRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = kMaskFormat;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(MaskTarget.Get(), &RTVDesc, MaskRTVHeap.CpuHandle(0));

        if (MaskSRVSlot == 0xFFFFFFFFu)
            MaskSRVSlot = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = kMaskFormat;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device, MaskTarget.Get(), SRVDesc, MaskSRVSlot);
    }

    void FSelectionOutline::Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                   u32 Width, u32 Height) {
        if (!Initialized) return;
        CreateTargets(Device, SRVHeap, Width, Height);
    }

    void FSelectionOutline::BuildRootSignatures(ID3D12Device* Device) {
        {
            D3D12_ROOT_PARAMETER P{};
            P.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            P.Descriptor.ShaderRegister = 2; 
            P.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters = 1;
            Desc.pParameters   = &P;
            Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> Blob, Err;
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
            if (FAILED(Hr)) {
                if (Err) LogError(std::string("Outline Mask Root Sig: ") +
                                  static_cast<const char*>(Err->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                     IID_PPV_ARGS(&MaskRootSig)));
        }

        {
            D3D12_DESCRIPTOR_RANGE SRVRange{};
            SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            SRVRange.NumDescriptors                    = 1;
            SRVRange.BaseShaderRegister                = 0; 
            SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER P[2]{};
            P[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            P[0].Descriptor.ShaderRegister = 0; 
            P[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            P[1].DescriptorTable.NumDescriptorRanges = 1;
            P[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
            P[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC Sampler{};
            Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.ShaderRegister   = 0; // s0
            Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters     = _countof(P);
            Desc.pParameters       = P;
            Desc.NumStaticSamplers = 1;
            Desc.pStaticSamplers   = &Sampler;
            Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> Blob, Err;
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
            if (FAILED(Hr)) {
                if (Err) LogError(std::string("Outline Root Sig: ") +
                                  static_cast<const char*>(Err->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                     IID_PPV_ARGS(&OutlineRootSig)));
        }
    }

    void FSelectionOutline::BuildPSOs(ID3D12Device* Device) {
        {
            auto VS = LoadShaderBytecode("SelectionGeom.vs_6_0.cso");
            auto PS = LoadShaderBytecode("SelectionMask.ps_6_0.cso");

            D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };

            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode        = D3D12_FILL_MODE_SOLID;
            Raster.CullMode        = D3D12_CULL_MODE_NONE; 
            Raster.DepthClipEnable = TRUE;

            D3D12_DEPTH_STENCIL_DESC Depth{};
            Depth.DepthEnable    = FALSE; 
            Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

            D3D12_BLEND_DESC Blend{};
            Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature        = MaskRootSig.Get();
            PSODesc.VS                    = { VS.data(), VS.size() };
            PSODesc.PS                    = { PS.data(), PS.size() };
            PSODesc.BlendState            = Blend;
            PSODesc.SampleMask            = UINT_MAX;
            PSODesc.RasterizerState       = Raster;
            PSODesc.DepthStencilState     = Depth;
            PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
            PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            PSODesc.NumRenderTargets      = 1;
            PSODesc.RTVFormats[0]         = kMaskFormat;
            PSODesc.SampleDesc            = { 1, 0 };
            SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&MaskPSO)));
        }

        {
            auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
            auto PS = LoadShaderBytecode("SelectionOutline.ps_6_0.cso");

            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode        = D3D12_FILL_MODE_SOLID;
            Raster.CullMode        = D3D12_CULL_MODE_NONE;
            Raster.DepthClipEnable = TRUE;

            D3D12_DEPTH_STENCIL_DESC Depth{};
            Depth.DepthEnable    = FALSE;
            Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

            D3D12_BLEND_DESC Blend{};
            Blend.RenderTarget[0].BlendEnable           = TRUE;
            Blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            Blend.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            Blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
            Blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
            Blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            Blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature        = OutlineRootSig.Get();
            PSODesc.VS                    = { VS.data(), VS.size() };
            PSODesc.PS                    = { PS.data(), PS.size() };
            PSODesc.BlendState            = Blend;
            PSODesc.SampleMask            = UINT_MAX;
            PSODesc.RasterizerState       = Raster;
            PSODesc.DepthStencilState     = Depth;
            PSODesc.InputLayout           = { nullptr, 0 };
            PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            PSODesc.NumRenderTargets      = 1;
            PSODesc.RTVFormats[0]         = FSwapChain::kFormat; 
            PSODesc.SampleDesc            = { 1, 0 };
            SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OutlinePSO)));
        }
    }

    void FSelectionOutline::CreateConstantBuffer(ID3D12Device* Device) {
        // Passo de 256 explicito nos dois: e o alinhamento minimo de CBV, e o indexador de
        // ambos ja assume esse passo.
        const GpuResources::FUploadBuffer Params =
            GpuResources::CreateUploadBuffer(Device, 256, kFIF);
        ParamsCB     = Params.Resource;
        MappedParams = Params.Mapped;

        const GpuResources::FUploadBuffer Objects =
            GpuResources::CreateUploadBuffer(Device, 256, kMaxOutlineObjects * kFIF);
        ObjectCB       = Objects.Resource;
        MappedObjectCB = Objects.Mapped;
    }

    void FSelectionOutline::RecordMask(ID3D12GraphicsCommandList* CmdList,
                                       const FDrawItem* Items, size_t Count, u32 FrameSlot) {
        if (!Initialized) return;

        if (MaskState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = MaskTarget.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = MaskState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            CmdList->ResourceBarrier(1, &B);
            MaskState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        auto RTV = MaskRTVHeap.CpuHandle(0);
        const FLOAT ClearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CmdList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
        CmdList->ClearRenderTargetView(RTV, ClearZero, 0, nullptr);

        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(MaskW); VP.Height = static_cast<FLOAT>(MaskH);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = static_cast<LONG>(MaskW); Sci.bottom = static_cast<LONG>(MaskH);
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        CmdList->SetGraphicsRootSignature(MaskRootSig.Get());
        CmdList->SetPipelineState(MaskPSO.Get());
        const size_t N = std::min(Count, static_cast<size_t>(kMaxOutlineObjects));
        const D3D12_GPU_VIRTUAL_ADDRESS CBBase = ObjectCB->GetGPUVirtualAddress();

        const u32 FrameBase = (FrameSlot % kFIF) * kMaxOutlineObjects;
        for (size_t i = 0; i < N; ++i) {
            const FDrawItem& It = Items[i];
            if (!It.Mesh || !It.Mesh->IsValid()) continue;

            const u64 Slot = static_cast<u64>(FrameBase + i);
            std::memcpy(MappedObjectCB + Slot * 256, &It.MVP, sizeof(Mat44));
            CmdList->SetGraphicsRootConstantBufferView(0, CBBase + Slot * 256);
            It.Mesh->Draw(CmdList);
        }
    }

    void FSelectionOutline::RecordOutline(ID3D12GraphicsCommandList* CmdList, FTextureSRVHeap& SRVHeap,
                                          D3D12_CPU_DESCRIPTOR_HANDLE TargetRTV, u32 Width, u32 Height,
                                          u32 FrameSlot) {
        if (!Initialized) return;

        OutlineParams P{};
        P.InvSizeX     = 1.0f / static_cast<f32>(std::max(1u, Width));
        P.InvSizeY     = 1.0f / static_cast<f32>(std::max(1u, Height));
        P.Thickness    = Thickness;
        P.FillStrength = FillStrength;
        P.ColorR       = Color.X; P.ColorG = Color.Y; P.ColorB = Color.Z;
        P.Alpha        = Intensity;
        const u64 ParamsOffset = static_cast<u64>(FrameSlot % kFIF) * 256;
        std::memcpy(MappedParams + ParamsOffset, &P, sizeof(P));

        if (MaskState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = MaskTarget.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = MaskState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CmdList->ResourceBarrier(1, &B);
            MaskState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        CmdList->OMSetRenderTargets(1, &TargetRTV, FALSE, nullptr);
        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(Width); VP.Height = static_cast<FLOAT>(Height);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = static_cast<LONG>(Width); Sci.bottom = static_cast<LONG>(Height);
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        CmdList->SetGraphicsRootSignature(OutlineRootSig.Get());
        CmdList->SetPipelineState(OutlinePSO.Get());
        CmdList->SetGraphicsRootConstantBufferView(0, ParamsCB->GetGPUVirtualAddress() + ParamsOffset);
        CmdList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(MaskSRVSlot));
        CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CmdList->DrawInstanced(3, 1, 0, 0);
    }

    FPassShaderStems FSelectionOutline::ShaderStems() const {
        static const char* const kStems[] = { "SelectionGeom.vs", "SelectionMask.ps", "SelectionOutline.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FSelectionOutline::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSOs(_Ctx.Device);
    }

}
