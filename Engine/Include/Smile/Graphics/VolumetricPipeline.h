#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    class FVolumetricPipeline {
    public:
        // _NvApiExtnSlot: acrescenta o root param 3 com o UAV falso da extensao HLSL da NVAPI
        // (u999, space0). So as permutacoes instrumentadas com timer pedem isso — ver
        // FShaderTimer. O root param existe p/ o shader validar; o driver troca o acesso.
        void Initialize(ID3D12Device* _Device, const std::string& _CSOName,
                        u32 _NumSRVs = 4, u32 _NumUAVs = 1, bool _HeapDirectlyIndexed = false,
                        bool _NvApiExtnSlot = false);

        // Indice do root param do slot da NVAPI, quando a pipeline foi criada com ele.
        static constexpr u32 kNvApiRootParam = 3;

        void Bind(ID3D12GraphicsCommandList* _CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
