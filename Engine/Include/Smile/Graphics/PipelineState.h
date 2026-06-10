#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FPipelineState {
    public:
        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device, u32 SampleCount);

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        // PSO opaca principal: depth EQUAL + write OFF (depende do depth pre-pass ter
        // escrito a profundidade exata) -> shada cada pixel opaco UMA vez (sem overdraw).
        ID3D12PipelineState* PSO()           const { return PipelineState.Get(); }
        // PSO sem back-face culling (materiais two-sided/masked: folhagem do Bistro).
        // Depth LESS + write ON (masked nao entra no pre-pass; testa contra o depth opaco).
        ID3D12PipelineState* PSOTwoSided()   const { return PipelineStateTwoSided.Get(); }
        // PSO depth-only (sem PS, RT write mask 0): o pre-pass de profundidade dos opacos.
        ID3D12PipelineState* PSODepthOnly()  const { return PipelineStateDepthOnly.Get(); }
        // PSO opaca depth LESS + write ON (full PS): usada quando o depth pre-pass esta
        // DESLIGADO (sem pre-pass nao da p/ usar EQUAL). Permite A/B do pre-pass.
        ID3D12PipelineState* PSOOpaqueLess() const { return PipelineStateOpaqueLess.Get(); }

    private:
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12PipelineState> PipelineState;
        ComPtr<ID3D12PipelineState> PipelineStateTwoSided;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnly;
        ComPtr<ID3D12PipelineState> PipelineStateOpaqueLess;
    };
} 
