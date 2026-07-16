#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>

namespace Smile {
    void FUploadQueue::Initialize(ID3D12Device* _Device) {
        D3D12_COMMAND_QUEUE_DESC Desc{};
        Desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        SMILE_HR(_Device->CreateCommandQueue(&Desc, IID_PPV_ARGS(&Queue)));

        for (u32 i = 0; i < kSlots; ++i)
            SMILE_HR(_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                     IID_PPV_ARGS(&Allocators[i])));
        SMILE_HR(_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                            Allocators[0].Get(), nullptr, IID_PPV_ARGS(&List)));
        SMILE_HR(List->Close());

        SMILE_HR(_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!Event) {
            SMILE_HR(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void FUploadQueue::Shutdown() {
        if (Fence) WaitIdle();
        Pending.clear();
        if (Event) { CloseHandle(Event); Event = nullptr; }
    }

    ID3D12GraphicsCommandList* FUploadQueue::Begin() {
        CpuWait(SlotFence[Slot]); // allocator do slot so pode resetar com a GPU alem dele
        Retire();
        SMILE_HR(Allocators[Slot]->Reset());
        SMILE_HR(List->Reset(Allocators[Slot].Get(), nullptr));
        return List.Get();
    }

    u64 FUploadQueue::Submit(std::vector<ComPtr<ID3D12Resource>>&& _Staging) {
        SMILE_HR(List->Close());
        ID3D12CommandList* Lists[] = { List.Get() };
        Queue->ExecuteCommandLists(1, Lists);
        ++FenceValue;
        SMILE_HR(Queue->Signal(Fence.Get(), FenceValue));
        SlotFence[Slot] = FenceValue;
        Slot = (Slot + 1) % kSlots;
        if (!_Staging.empty())
            Pending.push_back({ FenceValue, std::move(_Staging) });
        return FenceValue;
    }

    void FUploadQueue::WaitIdle() {
        CpuWait(FenceValue);
        Retire();
    }

    void FUploadQueue::CpuWait(u64 _Value) {
        if (_Value == 0 || Fence->GetCompletedValue() >= _Value) return;
        SMILE_HR(Fence->SetEventOnCompletion(_Value, Event));
        WaitForSingleObject(Event, INFINITE);
    }

    void FUploadQueue::Retire() {
        const u64 Done = Fence->GetCompletedValue();
        Pending.erase(std::remove_if(Pending.begin(), Pending.end(),
                                     [Done](const FRetired& R) { return R.FenceValue <= Done; }),
                      Pending.end());
    }
}
