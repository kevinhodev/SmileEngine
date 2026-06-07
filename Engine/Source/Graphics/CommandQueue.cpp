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

        SMILE_HR(_Device->CreateCommandAllocator(_Type, IID_PPV_ARGS(&UploadAllocator)));
        for (u32 i = 0; i < kFramesInFlight; ++i)
            SMILE_HR(_Device->CreateCommandAllocator(_Type, IID_PPV_ARGS(&FrameAllocators[i])));

        SMILE_HR(_Device->CreateCommandList(0, _Type, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
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

    void FCommandQueue::WaitForValue(UINT64 _Value) {
        if (Fence->GetCompletedValue() < _Value) {
            SMILE_HR(Fence->SetEventOnCompletion(_Value, FenceEvent));
            WaitForSingleObject(FenceEvent, INFINITE);
        }
    }

    // --- Caminho sincrono ---

    void FCommandQueue::ResetForRecording() {
        SMILE_HR(UploadAllocator->Reset());
        SMILE_HR(CommandList->Reset(UploadAllocator.Get(), nullptr));
    }

    void FCommandQueue::ExecuteAndSync(ID3D12CommandList* const* _CommandLists, UINT _Count) {
        CommandQueue->ExecuteCommandLists(_Count, _CommandLists);
        Signal();
        WaitForValue(FenceValue);
    }

    void FCommandQueue::Flush() {
        Signal();
        WaitForValue(FenceValue);
    }

    // --- Caminho per-frame (frames in flight) ---

    void FCommandQueue::BeginFrame() {
        CurrentFrame = (CurrentFrame + 1) % kFramesInFlight;
        // Espera a GPU terminar o ultimo frame que usou este allocator antes de reseta-lo.
        WaitForValue(FrameFenceValues[CurrentFrame]);
        SMILE_HR(FrameAllocators[CurrentFrame]->Reset());
        SMILE_HR(CommandList->Reset(FrameAllocators[CurrentFrame].Get(), nullptr));
    }

    void FCommandQueue::EndFrame(ID3D12CommandList* const* _CommandLists, UINT _Count) {
        CommandQueue->ExecuteCommandLists(_Count, _CommandLists);
        Signal();
        FrameFenceValues[CurrentFrame] = FenceValue;
    }
}
