#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Backend/D3D12/PipelineUtils.h"
#include "Smile/Graphics/Debug/ShaderTimer.h"

namespace Smile {
    void FComputePipeline::Initialize(ID3D12Device* _Device, const std::string& _CSOName, bool) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 1;
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors                    = 1;
        UAVRange.BaseShaderRegister                = 0;
        UAVRange.RegisterSpace                     = 0;
        UAVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[0].Constants.ShaderRegister = 0;
        RootParams[0].Constants.RegisterSpace  = 0;
        RootParams[0].Constants.Num32BitValues = 8;
        RootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &UAVRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC StaticSampler{};
        StaticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        StaticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.MaxAnisotropy    = 1;
        StaticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        StaticSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        StaticSampler.MinLOD           = 0.0f;
        StaticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
        StaticSampler.ShaderRegister   = 0;
        StaticSampler.RegisterSpace    = 0;
        StaticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC RootSigDesc{};
        RootSigDesc.NumParameters     = _countof(RootParams);
        RootSigDesc.pParameters       = RootParams;
        RootSigDesc.NumStaticSamplers = 1;
        RootSigDesc.pStaticSamplers   = &StaticSampler;
        RootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        CreatePipeline(_Device, _CSOName, RootSigDesc);
    }

    void FComputePipeline::Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                                      u32 _NumSRVs, u32 _NumUAVs,
                                      bool _HeapDirectlyIndexed, bool _NvApiExtnSlot) {
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

        D3D12_DESCRIPTOR_RANGE NVAPIRange{};
        NVAPIRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        NVAPIRange.NumDescriptors                    = 1;
        NVAPIRange.BaseShaderRegister                = FShaderTimer::kExtnSlot;
        NVAPIRange.RegisterSpace                     = FShaderTimer::kExtnSpace;
        NVAPIRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

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
        RootParams[3].DescriptorTable.pDescriptorRanges   = &NVAPIRange;
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

        CreatePipeline(_Device, _CSOName, RootSigDesc);
    }

    void FComputePipeline::CreatePipeline(
        ID3D12Device* _Device, const std::string& _CSOName,
        const D3D12_ROOT_SIGNATURE_DESC& _RootSignatureDesc) {
        RootSignature = PipelineUtils::CreateRootSignature(
            _Device, _RootSignatureDesc, "ComputePipeline");
        PSO = PipelineUtils::CreateComputePSO(_Device, RootSignature.Get(), _CSOName);
    }

    void FComputePipeline::Bind(ID3D12GraphicsCommandList* _CommandList) const {
        _CommandList->SetComputeRootSignature(RootSignature.Get());
        _CommandList->SetPipelineState(PSO.Get());
    }
}
