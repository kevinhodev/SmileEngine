#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FCommandQueue {
    public:
        void Initialize(ID3D12Device* Device, D3D12_COMMAND_LIST_TYPE Type);

        void ExecuteAndSync(ID3D12CommandList* const* CommandLists, UINT Count);

        void Flush();

        ID3D12CommandQueue*        Native()    const { return CommandQueue.Get(); }
        ID3D12CommandAllocator*    Allocator() const { return CommandAllocator.Get(); }
        ID3D12GraphicsCommandList* List()      const { return CommandList.Get(); }

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
} 
