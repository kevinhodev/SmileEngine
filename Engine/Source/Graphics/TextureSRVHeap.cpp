#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include <stdexcept>

namespace Smile {
    void FTextureSRVHeap::Initialize(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc{};
        DescriptorHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        DescriptorHeapDesc.NumDescriptors = kCapacity;
        DescriptorHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DescriptorHeapDesc.NodeMask       = 0;
        SMILE_HR(_Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(&Heap)));

        HandleSize = _Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    u32 FTextureSRVHeap::Allocate(u32 _Count) {
        if (NextSlot + _Count > kCapacity)
            throw std::runtime_error("TextureSRVHeap capacity exceeded");
        u32 Slot = NextSlot;
        NextSlot += _Count;
        return Slot;
    }

    void FTextureSRVHeap::CreateSRV(ID3D12Device* Device, ID3D12Resource* _Resource,
                                     const D3D12_SHADER_RESOURCE_VIEW_DESC& _SRVDesc, u32 _Slot) {
        Device->CreateShaderResourceView(_Resource, &_SRVDesc, CpuHandle(_Slot));
    }

    void FTextureSRVHeap::CreateUAV(ID3D12Device* Device, ID3D12Resource* _Resource,
                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC& _UAVDesc, u32 _Slot) {
        Device->CreateUnorderedAccessView(_Resource, nullptr, &_UAVDesc, CpuHandle(_Slot));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE FTextureSRVHeap::CpuHandle(u32 _Slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(_Slot) * HandleSize;
        return Handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE FTextureSRVHeap::GpuHandle(u32 _Slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetGPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<UINT64>(_Slot) * HandleSize;
        return Handle;
    }
} 
