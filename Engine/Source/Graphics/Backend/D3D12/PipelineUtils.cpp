#include "Smile/Graphics/Backend/D3D12/PipelineUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"

namespace Smile::PipelineUtils {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(
        ID3D12Device* _Device, const D3D12_ROOT_SIGNATURE_DESC& _Desc,
        std::string_view _Context) {
        Microsoft::WRL::ComPtr<ID3DBlob> RootBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
        const HRESULT Hr = D3D12SerializeRootSignature(
            &_Desc, D3D_ROOT_SIGNATURE_VERSION_1, &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob) {
                std::string Message(_Context);
                if (!Message.empty()) Message += " root sig error: ";
                else                  Message  = "Root sig error: ";
                Message += static_cast<const char*>(ErrorBlob->GetBufferPointer());
                LogError(Message);
            }
            SMILE_HR(Hr);
        }

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        SMILE_HR(_Device->CreateRootSignature(
            0, RootBlob->GetBufferPointer(), RootBlob->GetBufferSize(),
            IID_PPV_ARGS(&RootSignature)));
        return RootSignature;
    }

    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateComputePSO(
        ID3D12Device* _Device, ID3D12RootSignature* _RootSignature,
        const std::string& _CSOName) {
        const auto CSOBlob = LoadShaderBytecode(_CSOName);

        D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature = _RootSignature;
        Desc.CS             = { CSOBlob.data(), CSOBlob.size() };
        Desc.NodeMask       = 0;
        Desc.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        SMILE_HR(_Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&PSO)));
        return PSO;
    }
}
