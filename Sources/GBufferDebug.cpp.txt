#include "Smile/Graphics/GBufferDebug.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <string>

namespace Smile {

    void FGBufferDebug::Initialize(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        BuildPSO(_Device, _RTFormat);
        Initialized = true;
        LogInfo("G-Buffer debug pass inicializado");
    }

    void FGBufferDebug::BuildRootSignature(ID3D12Device* _Device) {
        // Tabela com os 3 SRVs contiguos do G-buffer (t0,t1,t2).
        D3D12_DESCRIPTOR_RANGE Range{};
        Range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Range.NumDescriptors                    = 3;
        Range.BaseShaderRegister                = 0;
        Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // 2a tabela: o motion vector (t3), num SRV separado (nao contiguo ao G-buffer).
        D3D12_DESCRIPTOR_RANGE VelRange{};
        VelRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        VelRange.NumDescriptors                    = 1;
        VelRange.BaseShaderRegister                = 3; // t3
        VelRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[0].Constants.ShaderRegister = 0; // b0 (Mode)
        RootParams[0].Constants.Num32BitValues = 4;
        RootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &Range;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &VelRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 0;
        Desc.pStaticSamplers   = nullptr;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("GBufferDebug root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FGBufferDebug::BuildPSO(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("GBufferDebug.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

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
        PSODesc.RTVFormats[0]         = _RTFormat;
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FGBufferDebug::Execute(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                                u32 _GBufferTableStart, u32 _VelocitySRVSlot, u32 _Mode) {
        if (!Initialized) return;
        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->SetPipelineState(PSO.Get());
        const u32 Consts[4] = { _Mode, 0, 0, 0 };
        _CommandList->SetGraphicsRoot32BitConstants(0, 4, Consts, 0);
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_GBufferTableStart));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_VelocitySRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
