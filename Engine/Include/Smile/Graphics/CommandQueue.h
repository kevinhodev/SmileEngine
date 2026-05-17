#pragma once

// Command queue + allocator + list + fence (sincronizacao CPU<->GPU simples).
// Versao inicial: 1 frame in flight (CPU espera GPU a cada Present).

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {

class CommandQueue {
public:
    void Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type);

    // Submete a command list e aguarda a GPU terminar (sync simples).
    void ExecuteAndSync(ID3D12CommandList* const* lists, UINT count);

    // Espera ate que a GPU tenha completado todo o trabalho enfileirado.
    void Flush();

    ID3D12CommandQueue*        Native()    const { return CommandQueue.Get(); }
    ID3D12CommandAllocator*    Allocator() const { return CommandAllocator.Get(); }
    ID3D12GraphicsCommandList* List()      const { return CommandList.Get(); }

    // Reseta allocator + list para o proximo frame.
    void ResetForRecording();

private:
    void Signal();
    void WaitForFence();

    ComPtr<ID3D12CommandQueue>        CommandQueue;
    ComPtr<ID3D12CommandAllocator>    CommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> CommandList;
    ComPtr<ID3D12Fence>               Fence;
    UINT64 FenceValue = 0;
    HANDLE FenceEvent = nullptr;
};

} // namespace Smile
