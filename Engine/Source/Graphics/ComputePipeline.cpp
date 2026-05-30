#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <fstream>
#include <vector>
#include <stdexcept>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadCSO(const std::string& _Name) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Name;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir compute shader: " + FullPath);
                throw std::runtime_error("Compute shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    void FComputePipeline::Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                                       bool /*_SourceIsCube*/) {
        // Root signature: b0 (root constants, 8 dwords) + descriptor table SRV(t0)
        // + descriptor table UAV(u0) + static sampler s0. SRV view dimension is
        // declared in the shader itself (Texture2D vs TextureCube); the root sig
        // only allocates a slot, so the same layout works for both source types.
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

        Microsoft::WRL::ComPtr<ID3DBlob> RootBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Compute root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, RootBlob->GetBufferPointer(),
                                              RootBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));

        auto CSOBlob = LoadCSO(_CSOName);

        D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature = RootSignature.Get();
        PSODesc.CS             = { CSOBlob.data(), CSOBlob.size() };
        PSODesc.NodeMask       = 0;
        PSODesc.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;
        SMILE_HR(_Device->CreateComputePipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FComputePipeline::Bind(ID3D12GraphicsCommandList* _CommandList) const {
        _CommandList->SetComputeRootSignature(RootSignature.Get());
        _CommandList->SetPipelineState(PSO.Get());
    }
}
