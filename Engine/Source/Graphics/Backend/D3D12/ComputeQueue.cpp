#include "Smile/Graphics/Backend/D3D12/ComputeQueue.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FAsyncComputeQueue::Initialize(ID3D12Device* _Device) {
        D3D12_COMMAND_QUEUE_DESC Desc{};
        Desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        SMILE_HR(_Device->CreateCommandQueue(&Desc, IID_PPV_ARGS(&Queue)));

        for (u32 i = 0; i < kSlots; ++i)
            SMILE_HR(_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                     IID_PPV_ARGS(&Allocators[i])));
        SMILE_HR(_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                            Allocators[0].Get(), nullptr, IID_PPV_ARGS(&List)));
        SMILE_HR(List->Close());

        SMILE_HR(_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!Event) {
            SMILE_HR(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void FAsyncComputeQueue::Shutdown() {
        if (Fence) WaitIdle();
        if (Event) { CloseHandle(Event); Event = nullptr; }
    }

    ID3D12GraphicsCommandList* FAsyncComputeQueue::Begin() {
        CpuWait(SlotFence[Slot]); // allocator do slot so pode resetar com a GPU alem dele
        SMILE_HR(Allocators[Slot]->Reset());
        SMILE_HR(List->Reset(Allocators[Slot].Get(), nullptr));
        return List.Get();
    }

    u64 FAsyncComputeQueue::SubmitAfter(ID3D12Fence* _WaitFence, u64 _WaitValue) {
        SMILE_HR(List->Close());
        if (_WaitFence) SMILE_HR(Queue->Wait(_WaitFence, _WaitValue));
        ID3D12CommandList* Lists[] = { List.Get() };
        Queue->ExecuteCommandLists(1, Lists);
        ++FenceValue;
        SMILE_HR(Queue->Signal(Fence.Get(), FenceValue));
        SlotFence[Slot] = FenceValue;
        Slot = (Slot + 1) % kSlots;
        return FenceValue;
    }

    void FAsyncComputeQueue::WaitIdle() {
        CpuWait(FenceValue);
    }

    void FAsyncComputeQueue::CpuWait(u64 _Value) {
        if (_Value == 0 || Fence->GetCompletedValue() >= _Value) return;
        SMILE_HR(Fence->SetEventOnCompletion(_Value, Event));
        WaitForSingleObject(Event, INFINITE);
    }
}
