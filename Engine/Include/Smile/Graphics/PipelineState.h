#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FPipelineState {
    public:
        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device, u32 SampleCount);

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* PSO()           const { return PipelineState.Get(); }

    private:
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12PipelineState> PipelineState;
    };
} 
