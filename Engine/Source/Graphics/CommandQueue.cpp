#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FCommandQueue::Initialize(ID3D12Device* _Device, D3D12_COMMAND_LIST_TYPE _Type) {
        D3D12_COMMAND_QUEUE_DESC CommandQueueDesc{};
        CommandQueueDesc.Type     = _Type;
        CommandQueueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        CommandQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        CommandQueueDesc.NodeMask = 0;
        SMILE_HR(_Device->CreateCommandQueue(&CommandQueueDesc, IID_PPV_ARGS(&CommandQueue)));

        SMILE_HR(_Device->CreateCommandAllocator(_Type, IID_PPV_ARGS(&CommandAllocator)));

        SMILE_HR(_Device->CreateCommandList(0, _Type, CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
        SMILE_HR(CommandList->Close());

        SMILE_HR(_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        FenceValue = 0;

        FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!FenceEvent) {
            SMILE_HR(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void FCommandQueue::Signal() {
        ++FenceValue;
        SMILE_HR(CommandQueue->Signal(Fence.Get(), FenceValue));
    }

    void FCommandQueue::WaitForFence() {
        if (Fence->GetCompletedValue() < FenceValue) {
            SMILE_HR(Fence->SetEventOnCompletion(FenceValue, FenceEvent));
            WaitForSingleObject(FenceEvent, INFINITE);
        }
    }

    void FCommandQueue::ExecuteAndSync(ID3D12CommandList* const* _CommandLists, UINT _Count) {
        CommandQueue->ExecuteCommandLists(_Count, _CommandLists);
        Signal();
        WaitForFence();
    }

    void FCommandQueue::Flush() {
        Signal();
        WaitForFence();
    }

    void FCommandQueue::ResetForRecording() {
        SMILE_HR(CommandAllocator->Reset());
        SMILE_HR(CommandList->Reset(CommandAllocator.Get(), nullptr));
    }
} 
