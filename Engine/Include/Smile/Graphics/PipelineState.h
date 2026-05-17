#pragma once

// Root signature + PSO do triangulo demo. Mantem-se proposital simples.

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {

class PipelineState {
public:
    void Initialize(ID3D12Device* device);
    void RecreatePSO(ID3D12Device* device, u32 sampleCount);

    ID3D12RootSignature* RootSignature() const { return m_rootSignature.Get(); }
    ID3D12PipelineState* PSO()           const { return PipelineState.Get(); }

private:
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> PipelineState;
};

} // namespace Smile
