#include "Smile/Graphics/Editor/Picking.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"
#include "Smile/Graphics/Renderer/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <iterator>

namespace Smile {
    static constexpr u32 kReadbackRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; 

    void FObjectPicker::Initialize(ID3D12Device* Device, u32 Width, u32 Height) {
        if (Initialized) return;
        BuildRootSignature(Device);
        BuildPSO(Device);
        CreateTargets(Device, Width, Height);

        Readback = GpuResources::CreateReadbackBuffer(Device, kReadbackRowPitch);

        Initialized = true;
        LogDebug("FObjectPicker (GPU ID-buffer picking) inicializado");
    }

    void FObjectPicker::CreateTargets(ID3D12Device* Device, u32 Width, u32 Height) {
        TargetWidth  = std::max(1u, Width);
        TargetHeight = std::max(1u, Height);
        IDTarget.Reset();

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format = DXGI_FORMAT_R32_UINT;

        IDTarget = GpuResources::CreateTex2D(
            Device, 1, 1, DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::Misc, &Clear, 1, 1, "Alvo de picking");

        if (IDRTVHeap.GetCapacity() == 0)
            IDRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R32_UINT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(IDTarget.Get(), &RTVDesc, IDRTVHeap.CpuHandle(0));

        PickDepth.Reset();
        D3D12_CLEAR_VALUE DepthClear{};
        DepthClear.Format             = DXGI_FORMAT_D32_FLOAT;
        DepthClear.DepthStencil.Depth = kClearDepth;
        PickDepth = GpuResources::CreateTex2D(
            Device, 1, 1, DXGI_FORMAT_D32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            EVramCategory::Misc, &DepthClear, 1, 1, "Depth do picking");
        if (PickDSVHeap.GetCapacity() == 0)
            PickDSVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        Device->CreateDepthStencilView(PickDepth.Get(), &DSVDesc, PickDSVHeap.CpuHandle(0));
    }

    void FObjectPicker::Resize(ID3D12Device* Device, u32 Width, u32 Height) {
        if (!Initialized) return;
        CreateTargets(Device, Width, Height);
        State = EState::Idle;
        ReadyCountdown = -1;
        DiscardResult = false;
    }

    void FObjectPicker::BuildRootSignature(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_RANGE MaterialRange{};
        MaterialRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        MaterialRange.NumDescriptors                    = kMaterialTextureSlots;
        MaterialRange.BaseShaderRegister                = 0;
        MaterialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER Params[4]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 2; 
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        Params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[1].Descriptor.ShaderRegister = 1;
        Params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &MaterialRange;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        Params[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        Params[3].Constants.ShaderRegister = 0;
        Params[3].Constants.Num32BitValues = 4;
        Params[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_ANISOTROPIC;
        Sampler.MaxAnisotropy    = 8;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderRegister   = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters = _countof(Params);
        Desc.pParameters   = Params;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("Picking Root Sig Error: ") +
                              static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                 IID_PPV_ARGS(&RootSig)));
    }

    void FObjectPicker::BuildPSO(ID3D12Device* Device) {
        auto VS = LoadShaderBytecode("SelectionGeom.vs_6_0.cso");
        auto PS = LoadShaderBytecode("ObjectID.ps_6_0.cso");
        auto AlphaPS = LoadShaderBytecode("ObjectIDAlpha.ps_6_0.cso");

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
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = kDepthFuncLessEqual;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R32_UINT;
        PSODesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT; 
        PSODesc.SampleDesc            = { 1, 0 };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
        PSODesc.PS = { AlphaPS.data(), AlphaPS.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&AlphaPSO)));
    }

    void FObjectPicker::RequestPick(u32 X, u32 Y) {
        if (!Initialized || State != EState::Idle) return; 
        PickX = X;
        PickY = Y;
        DiscardResult = false;
        State = EState::Requested;
    }

    void FObjectPicker::RecordIDPass(ID3D12GraphicsCommandList* CmdList,
                                     FTextureSRVHeap& SRVHeap,
                                     const FDrawItem* Items, size_t Count) {
        if (!Initialized || State != EState::Requested) return;

        auto RTV = IDRTVHeap.CpuHandle(0);
        auto DSV = PickDSVHeap.CpuHandle(0);
        const FLOAT ClearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CmdList->OMSetRenderTargets(1, &RTV, FALSE, &DSV);
        CmdList->ClearRenderTargetView(RTV, ClearZero, 0, nullptr);
        CmdList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);

        const u32 px = std::min(PickX, TargetWidth  - 1);
        const u32 py = std::min(PickY, TargetHeight - 1);

        // O alvo tem um unico pixel. Deslocar o viewport preserva a projecao full-resolution,
        // mas rasteriza somente o texel solicitado.
        D3D12_VIEWPORT VP{};
        VP.TopLeftX = -static_cast<FLOAT>(px);
        VP.TopLeftY = -static_cast<FLOAT>(py);
        VP.Width    = static_cast<FLOAT>(TargetWidth);
        VP.Height   = static_cast<FLOAT>(TargetHeight);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = 1; Sci.bottom = 1;
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
        CmdList->SetDescriptorHeaps(1, Heaps);
        CmdList->SetGraphicsRootSignature(RootSig.Get());
        ID3D12PipelineState* BoundPSO = nullptr;
        for (size_t i = 0; i < Count; ++i) {
            const FDrawItem& It = Items[i];
            if (!It.Mesh || !It.Mesh->IsValid()) continue;
            const bool AlphaAware = It.Material &&
                (It.Material->Constants.AlphaTest != 0 || It.Material->Blend);
            ID3D12PipelineState* Wanted = AlphaAware ? AlphaPSO.Get() : PSO.Get();
            if (Wanted != BoundPSO) {
                CmdList->SetPipelineState(Wanted);
                BoundPSO = Wanted;
            }
            u32 Constants[4] = { It.ObjectId, 0, 0, 0 };
            if (AlphaAware) {
                It.Material->Bind(CmdList, SRVHeap);
                Constants[1] = It.Material->Blend ? 2u : 1u;
            }
            CmdList->SetGraphicsRootConstantBufferView(0, It.ObjectCBAddress);
            CmdList->SetGraphicsRoot32BitConstants(3, 4, Constants, 0);
            It.Mesh->Draw(CmdList);
        }

        D3D12_RESOURCE_BARRIER ToCopy{};
        ToCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ToCopy.Transition.pResource   = IDTarget.Get();
        ToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        CmdList->ResourceBarrier(1, &ToCopy);

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource        = IDTarget.Get();
        Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource                          = Readback.Get();
        Dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint.Offset             = 0;
        Dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R32_UINT;
        Dst.PlacedFootprint.Footprint.Width    = 1;
        Dst.PlacedFootprint.Footprint.Height   = 1;
        Dst.PlacedFootprint.Footprint.Depth    = 1;
        Dst.PlacedFootprint.Footprint.RowPitch = kReadbackRowPitch;

        D3D12_BOX SrcBox{};
        SrcBox.left = 0; SrcBox.top = 0; SrcBox.front = 0;
        SrcBox.right = 1; SrcBox.bottom = 1; SrcBox.back = 1;
        CmdList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &SrcBox);

        ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        CmdList->ResourceBarrier(1, &ToCopy);

        State          = EState::Copied;
        ReadyCountdown = static_cast<int>(FCommandQueue::kFramesInFlight);
        DiscardResult  = false;
    }

    void FObjectPicker::Tick() {
        if (State == EState::Copied && ReadyCountdown > 0)
            --ReadyCountdown;
        if (State == EState::Copied && ReadyCountdown <= 0 && DiscardResult) {
            State = EState::Idle;
            ReadyCountdown = -1;
            DiscardResult = false;
        }
    }

    bool FObjectPicker::TryResolve(FPickIdentity& _OutIdentity) {
        if (State != EState::Copied || ReadyCountdown > 0) return false;
        if (DiscardResult) {
            State = EState::Idle;
            ReadyCountdown = -1;
            DiscardResult = false;
            return false;
        }

        u32 Value = 0;
        D3D12_RANGE ReadRange{ 0, sizeof(u32) };
        void* Mapped = nullptr;
        if (SUCCEEDED(Readback->Map(0, &ReadRange, &Mapped)) && Mapped) {
            Value = *reinterpret_cast<const u32*>(Mapped);
            D3D12_RANGE NoWrite{ 0, 0 };
            Readback->Unmap(0, &NoWrite);
        }

        State          = EState::Idle;
        ReadyCountdown = -1;
        DiscardResult  = false;
        // O valor indexa a tabela de identidades DESTE passe, e nao a lista de renderaveis. Fora
        // de faixa nao e "sem hit": e um readback que nao corresponde a tabela atual, o que so
        // acontece se o passe foi regravado no meio -- tratar como vazio e a resposta segura.
        _OutIdentity = DecodeCapturedIdentity(PickIdentities, Value);
        return true;
    }

    FPassShaderStems FObjectPicker::ShaderStems() const {
        static const char* const kStems[] = { "SelectionGeom.vs", "ObjectID.ps", "ObjectIDAlpha.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FObjectPicker::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSO(_Ctx.Device);
    }

}
