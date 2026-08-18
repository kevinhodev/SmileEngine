#include "Smile/Graphics/Debug/FlickerHeatmap.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"
#include <cstring>
#include <iterator>

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kOutFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT kStatsFormat = DXGI_FORMAT_R32G32_FLOAT;
    }

    void FFlickerHeatmap::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        if (Initialized) return;
        BuildRootSignature(Device);
        BuildPSO(Device);

        const GpuResources::FUploadBuffer Upload =
            GpuResources::CreateUploadBuffer(Device, sizeof(FlickerConstants));
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;

        CreateBuffers(Device, SRVHeap, W, H);
        Initialized = true;
        LogDebug("FlickerHeatmap (debug: variancia temporal por pixel) inicializado");
    }

    void FFlickerHeatmap::Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        if (!Initialized) return;
        CreateBuffers(Device, SRVHeap, W, H);
    }

    void FFlickerHeatmap::CreateBuffers(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        if (W == 0 || H == 0) return;
        Output.Reset();
        Stats.Reset();

        const FLOAT Clear[4] = { 0, 0, 0, 1 };
        D3D12_CLEAR_VALUE CV{}; CV.Format = kOutFormat; std::memcpy(CV.Color, Clear, sizeof(Clear));
        Output = GpuResources::CreateTex2D(
            Device, W, H, kOutFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, EVramCategory::Misc, &CV,
            1, 1, "Heatmap de flicker");
        OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        Stats = GpuResources::CreateTex2D(
            Device, W, H, kStatsFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, EVramCategory::Misc, nullptr,
            1, 1, "Heatmap de flicker");

        if (!OutputRTVHeap.Native())
            OutputRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        D3D12_RENDER_TARGET_VIEW_DESC RTV{}; RTV.Format = kOutFormat; RTV.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(Output.Get(), &RTV, OutputRTVHeap.CpuHandle(0));

        if (OutputSRVSlot_ == kInvalidSlot) OutputSRVSlot_ = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
        SRV.Format                  = kOutFormat;
        SRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRV.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device, Output.Get(), SRV, OutputSRVSlot_);

        if (StatsUAVSlot == kInvalidSlot) StatsUAVSlot = SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAV{};
        UAV.Format        = kStatsFormat;
        UAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SRVHeap.CreateUAV(Device, Stats.Get(), UAV, StatsUAVSlot);
    }

    void FFlickerHeatmap::BuildRootSignature(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 1;
        SRVRange.BaseShaderRegister                = 0; 
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors                    = 1;
        UAVRange.BaseShaderRegister                = 0; 
        UAVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER Params[3]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &UAVRange;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters = _countof(Params);
        Desc.pParameters   = Params;
        Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("FlickerHeatmap Root Sig Error: ") +
                              static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                             IID_PPV_ARGS(&RootSig)));
    }

    void FFlickerHeatmap::BuildPSO(ID3D12Device* Device) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("FlickerHeatmap.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode = D3D12_FILL_MODE_SOLID;
        Raster.CullMode = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable = FALSE; Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = kOutFormat;
        PSODesc.SampleDesc            = { 1, 0 };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FFlickerHeatmap::Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                                  u32 InputSRVSlot, f32 Mode, f32 HeatScale, f32 Alpha, bool Reset,
                                  u32 Width, u32 Height) {
        if (!Initialized || !Output) return;

        FlickerConstants C{};
        C.Alpha = Alpha; C.HeatScale = HeatScale; C.Mode = Mode; C.Reset = Reset ? 1.0f : 0.0f;
        C.ScrW = Width; C.ScrH = Height;
        std::memcpy(MappedCB, &C, sizeof(C));

        TransitionResource(CommandList, Output.Get(), OutputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        OutputState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_VIEWPORT VP{}; VP.Width = (FLOAT)Width; VP.Height = (FLOAT)Height; VP.MinDepth = 0; VP.MaxDepth = 1;
        D3D12_RECT Sc{}; Sc.right = (LONG)Width; Sc.bottom = (LONG)Height;
        CommandList->RSSetViewports(1, &VP);
        CommandList->RSSetScissorRects(1, &Sc);
        auto RTV = OutputRTVHeap.CpuHandle(0);
        CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);

        CommandList->SetGraphicsRootSignature(RootSig.Get());
        CommandList->SetPipelineState(PSO.Get());
        CommandList->SetGraphicsRootConstantBufferView(0, CB->GetGPUVirtualAddress());
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(InputSRVSlot));
        CommandList->SetGraphicsRootDescriptorTable(2, SRVHeap.GpuHandle(StatsUAVSlot));

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 0, nullptr);
        CommandList->IASetIndexBuffer(nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);

        TransitionResource(CommandList, Output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    FPassShaderStems FFlickerHeatmap::ShaderStems() const {
        static const char* const kStems[] = { "FlickerHeatmap.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FFlickerHeatmap::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSO(_Ctx.Device);
    }

}
