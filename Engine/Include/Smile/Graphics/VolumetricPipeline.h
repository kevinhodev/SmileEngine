#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    // Wider compute pipeline than FComputePipeline, for the atmosphere and cloud
    // passes. Root signature:
    //   b0 = root CBV (the pass's constant buffer; e.g. AtmosphereConstants)
    //   t0..t(NumSRVs-1) = SRV descriptor table (input LUTs / noise / depth)
    //   u0..u(NumUAVs-1) = UAV descriptor table (output targets)
    //   s0 = static linear-clamp sampler, s1 = static linear-wrap sampler
    // The SRV/UAV view dimensions live in the shader, so the same root signature
    // serves 2D, 3D and cube resources. FComputePipeline is intentionally left
    // untouched (its fixed shape is relied on by the five IBL passes).
    class FVolumetricPipeline {
    public:
        // Loads compiled <CSOName> from SMILE_SHADER_DIR and builds the root sig.
        void Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                        u32 _NumSRVs = 4, u32 _NumUAVs = 1);

        // Binds the pipeline + root signature. Caller sets descriptor heaps,
        // then SetComputeRootConstantBufferView(0,...), and the SRV/UAV tables.
        void Bind(ID3D12GraphicsCommandList* _CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
