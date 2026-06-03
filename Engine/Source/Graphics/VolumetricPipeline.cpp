#include "Smile/Graphics/VolumetricPipeline.h"
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

    void FVolumetricPipeline::Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                                         u32 _NumSRVs, u32 _NumUAVs) {
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

        D3D12_ROOT_PARAMETER RootParams[3]{};
        // b0 = root CBV
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // SRV table (t0..)
        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // UAV table (u0..)
        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &UAVRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        // s0 = linear clamp
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
        // s1 = linear wrap
        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC RootSigDesc{};
        RootSigDesc.NumParameters     = _countof(RootParams);
        RootSigDesc.pParameters       = RootParams;
        RootSigDesc.NumStaticSamplers = _countof(Samplers);
        RootSigDesc.pStaticSamplers   = Samplers;
        RootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

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

        auto CSOBlob = LoadCSO(_CSOName);

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
