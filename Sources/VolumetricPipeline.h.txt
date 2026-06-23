#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    class FVolumetricPipeline {
    public:
        void Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                        u32 _NumSRVs = 4, u32 _NumUAVs = 1, bool _HeapDirectlyIndexed = false);

        void Bind(ID3D12GraphicsCommandList* _CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
