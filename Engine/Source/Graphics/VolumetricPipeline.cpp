#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderTimer.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <vector>
#include <stdexcept>

namespace Smile {
    void FVolumetricPipeline::Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                                         u32 _NumSRVs, u32 _NumUAVs, bool _HeapDirectlyIndexed,
                                         bool _NvApiExtnSlot) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = _NumSRVs;
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors                    = _NumUAVs;
        UAVRange.BaseShaderRegister                = 0;
        UAVRange.RegisterSpace                     = 0;
        UAVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // Slot falso da extensao HLSL da NVAPI (timer de shader): u999 em space0. Range PROPRIO
        // porque a base do range de UAV acima e 0 — u999 nao cabe nele sem alocar 1000 slots.
        D3D12_DESCRIPTOR_RANGE NvApiRange{};
        NvApiRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        NvApiRange.NumDescriptors                    = 1;
        NvApiRange.BaseShaderRegister                = FShaderTimer::kExtnSlot;
        NvApiRange.RegisterSpace                     = FShaderTimer::kExtnSpace;
        NvApiRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[4]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &UAVRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &NvApiRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].RegisterSpace    = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC RootSigDesc{};
        RootSigDesc.NumParameters     = _NvApiExtnSlot ? 4u : 3u;
        RootSigDesc.pParameters       = RootParams;
        RootSigDesc.NumStaticSamplers = _countof(Samplers);
        RootSigDesc.pStaticSamplers   = Samplers;
        RootSigDesc.Flags             = _HeapDirectlyIndexed
            ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
            : D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> RootBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Volumetric root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, RootBlob->GetBufferPointer(),
                                              RootBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));

        auto CSOBlob = LoadShaderBytecode(_CSOName);

        D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature = RootSignature.Get();
        PSODesc.CS             = { CSOBlob.data(), CSOBlob.size() };
        PSODesc.NodeMask       = 0;
        PSODesc.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;
        SMILE_HR(_Device->CreateComputePipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FVolumetricPipeline::Bind(ID3D12GraphicsCommandList* _CommandList) const {
        _CommandList->SetComputeRootSignature(RootSignature.Get());
        _CommandList->SetPipelineState(PSO.Get());
    }
}
