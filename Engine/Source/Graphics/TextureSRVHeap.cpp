#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include <stdexcept>

namespace Smile {

    void FTextureSRVHeap::Initialize(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_HEAP_DESC Desc{};
        Desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        Desc.NumDescriptors = kCapacity;
        Desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Desc.NodeMask       = 0;
        SMILE_HR(Device->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&Heap)));

        HandleSize = Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    u32 FTextureSRVHeap::Allocate(u32 Count) {
        if (NextSlot + Count > kCapacity)
            throw std::runtime_error("TextureSRVHeap capacity exceeded");
        u32 Slot = NextSlot;
        NextSlot += Count;
        return Slot;
    }

    void FTextureSRVHeap::CreateSRV(ID3D12Device* Device, ID3D12Resource* Resource,
                                     const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, u32 Slot) {
        Device->CreateShaderResourceView(Resource, &Desc, CpuHandle(Slot));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE FTextureSRVHeap::CpuHandle(u32 Slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(Slot) * HandleSize;
        return Handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE FTextureSRVHeap::GpuHandle(u32 Slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetGPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<UINT64>(Slot) * HandleSize;
        return Handle;
    }

} // namespace Smile
