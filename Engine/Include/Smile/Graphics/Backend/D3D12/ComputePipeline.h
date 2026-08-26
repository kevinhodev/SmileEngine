#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    class FComputePipeline {
    public:
        // Layout legado: 8 root constants em b0, uma SRV t0, uma UAV u0 e sampler
        // linear-clamp s0. SourceIsCube e mantido para compatibilidade com os callers.
        void Initialize(ID3D12Device* Device, const std::string& CSOName,
                        bool SourceIsCube);

        // Layout configuravel: CBV b0, tabelas SRV/UAV dimensionaveis, samplers
        // linear-clamp s0 e linear-wrap s1, com bindless e slot NVAPI opcionais.
        void Initialize(ID3D12Device* Device, const std::string& CSOName,
                        u32 NumSRVs, u32 NumUAVs, bool HeapDirectlyIndexed = false,
                        bool NvApiExtnSlot = false);

        // Mesmo layout configuravel, com um bloco adicional de root constants. O bloco ocupa
        // o root param 3 e o registrador informado (normalmente b1); callers existentes mantem
        // os indices 0=CBV, 1=SRV e 2=UAV.
        void InitializeWithConstants(ID3D12Device* Device, const std::string& CSOName,
                                     u32 NumSRVs, u32 NumUAVs, u32 Num32BitConstants,
                                     u32 ConstantsRegister = 1,
                                     bool HeapDirectlyIndexed = false);

        // Indice do root param do slot da NVAPI no layout configuravel.
        static constexpr u32 kNvApiRootParam = 3;

        void Bind(ID3D12GraphicsCommandList* CommandList) const;

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }
        ID3D12PipelineState* GetPSO()           const { return PSO.Get(); }

    private:
        void InitializeConfigured(ID3D12Device* Device, const std::string& CSOName,
                                  u32 NumSRVs, u32 NumUAVs, u32 Num32BitConstants,
                                  u32 ConstantsRegister, bool HeapDirectlyIndexed,
                                  bool NvApiExtnSlot);
        void CreatePipeline(ID3D12Device* Device, const std::string& CSOName,
                            const D3D12_ROOT_SIGNATURE_DESC& RootSignatureDesc);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
    };
}
