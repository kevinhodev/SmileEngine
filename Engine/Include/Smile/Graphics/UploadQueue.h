#pragma once

#include <d3d12.h>
#include <vector>
#include "Smile/Core/Types.h"

namespace Smile {
    // Fila COPY dedicada p/ upload de texturas/meshes: Begin -> gravar copias -> Submit,
    // que NAO bloqueia (fence propria; os stagings ficam retidos ate a GPU passar por
    // eles) — o CPU prepara o proximo batch enquanto a GPU copia o anterior, e a fila
    // direta nunca para. Chame WaitIdle() antes do primeiro consumo dos recursos na
    // fila direta (BLAS/frame). Substitui os ExecuteAndSync de upload (stall total).
    //
    // Estados: recurso usado na fila COPY decai pra COMMON quando o ExecuteCommandLists
    // termina, e PROMOVE implicitamente pro estado de leitura no primeiro uso na fila
    // direta — por isso os uploads daqui nao gravam transition barrier nenhuma (a COPY
    // queue nem aceita transicao pra estado de shader).
    class FUploadQueue {
    public:
        static constexpr u32 kSlots = 2; // batches em voo (Begin espera o slot mais velho)

        void Initialize(ID3D12Device* Device);
        void Shutdown();

        ID3D12GraphicsCommandList* Begin();

        // Fecha e submete o batch. Staging e retido ate a fence do batch completar
        // (liberado em Begin/WaitIdle futuros). Retorna o valor de fence do batch.
        u64  Submit(std::vector<ComPtr<ID3D12Resource>>&& Staging);

        void WaitIdle();

    private:
        void CpuWait(u64 Value);
        void Retire(); // libera stagings cuja fence ja completou

        struct FRetired {
            u64 FenceValue;
            std::vector<ComPtr<ID3D12Resource>> Staging;
        };

        ComPtr<ID3D12CommandQueue>        Queue;
        ComPtr<ID3D12CommandAllocator>    Allocators[kSlots];
        u64                               SlotFence[kSlots] = {};
        ComPtr<ID3D12GraphicsCommandList> List;
        ComPtr<ID3D12Fence>               Fence;
        HANDLE                            Event      = nullptr;
        u64                               FenceValue = 0;
        u32                               Slot       = 0;
        std::vector<FRetired>             Pending;
    };
}
