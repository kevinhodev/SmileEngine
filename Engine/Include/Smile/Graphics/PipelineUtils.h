#pragma once

#include <d3d12.h>
#include <string>
#include <string_view>
#include <wrl/client.h>

namespace Smile::PipelineUtils {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(
        ID3D12Device* Device, const D3D12_ROOT_SIGNATURE_DESC& Desc,
        std::string_view Context);

    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateComputePSO(
        ID3D12Device* Device, ID3D12RootSignature* RootSignature,
        const std::string& CSOName);
}
