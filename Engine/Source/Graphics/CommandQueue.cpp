#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {

void CommandQueue::Initialize(ID3D12Device* _Device, D3D12_COMMAND_LIST_TYPE _Type) {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type     = _Type;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.NodeMask = 0;
    SMILE_HR(_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CommandQueue)));

    SMILE_HR(_Device->CreateCommandAllocator(_Type, IID_PPV_ARGS(&CommandAllocator)));

    // Cria a list ja fechada (estado padrao logo apos criacao seria "aberta para gravacao")
    SMILE_HR(_Device->CreateCommandList(0, _Type, CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
    SMILE_HR(CommandList->Close());

    SMILE_HR(_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
    FenceValue = 0;

    FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!FenceEvent) {
        SMILE_HR(HRESULT_FROM_WIN32(GetLastError()));
    }
}

void CommandQueue::Signal() {
    ++FenceValue;
    SMILE_HR(CommandQueue->Signal(Fence.Get(), FenceValue));
}

void CommandQueue::WaitForFence() {
    if (Fence->GetCompletedValue() < FenceValue) {
        SMILE_HR(Fence->SetEventOnCompletion(FenceValue, FenceEvent));
        WaitForSingleObject(FenceEvent, INFINITE);
    }
}

void CommandQueue::ExecuteAndSync(ID3D12CommandList* const* lists, UINT count) {
    CommandQueue->ExecuteCommandLists(count, lists);
    Signal();
    WaitForFence();
}

void CommandQueue::Flush() {
    Signal();
    WaitForFence();
}

void CommandQueue::ResetForRecording() {
    SMILE_HR(CommandAllocator->Reset());
    SMILE_HR(CommandList->Reset(CommandAllocator.Get(), nullptr));
}

} // namespace Smile
