#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FPipelineState {
    public:
        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* PSODepthOnly()  const { return PipelineStateDepthOnly.Get(); }
        ID3D12PipelineState* PSODepthNormal() const { return PipelineStateDepthNormal.Get(); }
        // Variantes masked/two-sided do prepass (cull none + PS com alpha-clip): folhagem e
        // grades entram no depth/normal do prepass, entao o GTAO as ve (fim dos speckles).
        ID3D12PipelineState* PSODepthOnlyMasked()   const { return PipelineStateDepthOnlyMasked.Get(); }
        ID3D12PipelineState* PSODepthNormalMasked() const { return PipelineStateDepthNormalMasked.Get(); }
        // Geometry pass do deferred: escreve o G-buffer (MRT 3 RTs). Depth EQUAL (le o depth ja
        // estabelecido). TwoSided = cull none p/ folhagem/alpha-test.
        ID3D12PipelineState* PSOGBuffer()         const { return PipelineStateGBuffer.Get(); }
        ID3D12PipelineState* PSOGBufferTwoSided() const { return PipelineStateGBufferTwoSided.Get(); }
        // Deferred lighting fullscreen (le o G-buffer -> HDR), na root signature principal.
        // Aditivo: soma a luz sobre o emissivo que o geometry pass escreveu no SceneColor.
        // Debug: variante opaca p/ as views de SSAO/GI (o shader substitui a tela inteira).
        ID3D12PipelineState* PSODeferredLighting()      const { return PipelineStateDeferredLighting.Get(); }
        ID3D12PipelineState* PSODeferredLightingDebug() const { return PipelineStateDeferredLightingDebug.Get(); }
        // Forward de translucidos (materiais Blend): alpha-blend premultiplicado sobre o HDR,
        // depth read-only, cull none (vidro two-sided).
        ID3D12PipelineState* PSOForwardBlend() const { return PipelineStateForwardBlend.Get(); }

    private:
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnly;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormal;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnlyMasked;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormalMasked;
        ComPtr<ID3D12PipelineState> PipelineStateGBuffer;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferTwoSided;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLighting;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLightingDebug;
        ComPtr<ID3D12PipelineState> PipelineStateForwardBlend;
    };
} 
