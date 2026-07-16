#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    // Fila COMPUTE assincrona: roda trabalho de compute (DDGI) em paralelo com o raster
    // da fila direta. Sincronizacao GPU-GPU por fence: SubmitAfter faz esta fila esperar
    // a fence da direta (inputs prontos) antes de executar, e a direta espera a fence
    // daqui (GpuWait no FCommandQueue) antes do primeiro consumo do resultado.
    //
    // Restricao de estados: lista COMPUTE nao aceita transicao envolvendo estados de
    // graficos (PIXEL_SHADER_RESOURCE, RENDER_TARGET, DEPTH_*) — essas transicoes ficam
    // na fila direta, antes do signal e depois do wait (ver FDDGI::TransitionFor*).
    class FAsyncComputeQueue {
    public:
        static constexpr u32 kSlots = 2; // batches em voo (Begin espera o slot mais velho)

        void Initialize(ID3D12Device* Device);
        void Shutdown();

        ID3D12GraphicsCommandList* Begin();

        // Fecha a list e executa DEPOIS que WaitFence alcancar WaitValue (GPU-side; passe
        // nullptr p/ nao esperar). Sinaliza a fence propria e retorna o valor — a fila
        // direta espera esse valor via GpuWait antes de ler o que o compute escreveu.
        u64  SubmitAfter(ID3D12Fence* WaitFence, u64 WaitValue);

        void WaitIdle();

        ID3D12Fence*        NativeFence() const { return Fence.Get(); }
        ID3D12CommandQueue* Native()      const { return Queue.Get(); }
        // Slot em gravacao (Begin ja esperou o fence dele) — p/ o ring do GPU profiler.
        u32                 CurrentSlot() const { return Slot; }

    private:
        void CpuWait(u64 Value);

        ComPtr<ID3D12CommandQueue>        Queue;
        ComPtr<ID3D12CommandAllocator>    Allocators[kSlots];
        u64                               SlotFence[kSlots] = {};
        ComPtr<ID3D12GraphicsCommandList> List;
        ComPtr<ID3D12Fence>               Fence;
        HANDLE                            Event      = nullptr;
        u64                               FenceValue = 0;
        u32                               Slot       = 0;
    };
}
