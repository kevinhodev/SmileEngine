#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    // Compute pipeline for IBL passes. All four CS shaders share the same root
    // signature shape:
    //   b0 = root constants (up to 8 x u32: face index, mip, roughness, sizes)
    //   t0 = source SRV (2D for equirect/BRDF, cube for irradiance/specular)
    //   u0 = destination UAV (Texture2DArray over cube faces, one mip)
    //   s0 = static sampler (linear clamp, all-mip)
    class FComputePipeline {
    public:
        // Loads compiled <CSOName>.cs_6_0.cso from SMILE_SHADER_DIR.
        // SourceIsCube=true binds t0 as TextureCube; false as Texture2D.
        void Initialize(ID3D12Device* Device, const std::string& CSOName,
                        bool SourceIsCube);

        // Bind this pipeline + its root signature on the command list.
        // Caller is responsible for SetDescriptorHeaps before binding tables.
        void Bind(ID3D12GraphicsCommandList* CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
