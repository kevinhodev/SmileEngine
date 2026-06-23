#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FTextureSRVHeap;

    // Passe de visualizacao do G-Buffer (Fase 1 da migracao deferred). Fullscreen: le os 3 RTs do
    // G-buffer e pinta o canal escolhido. So roda quando Mode > 0 (debug/validacao).
    class FGBufferDebug {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);

        // GBufferTableStart = primeiro dos 3 SRVs contiguos do G-buffer (FGBuffer::SRVTableStart()).
        // VelocitySRVSlot = SRV do motion vector (t3), num slot separado.
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     u32 GBufferTableStart, u32 VelocitySRVSlot, u32 Mode);

        bool IsInitialized() const { return Initialized; }

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, DXGI_FORMAT RTFormat);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        bool Initialized = false;
    };
}
