#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    class FComputePipeline {
    public:
        void Initialize(ID3D12Device* Device, const std::string& CSOName,
                        bool SourceIsCube);

        void Bind(ID3D12GraphicsCommandList* CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
