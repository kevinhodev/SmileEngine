#include "Smile/Graphics/RHI/TextureSRVHeap.h"
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

        DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        SMILE_HR(_Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(&StagingHeap)));

        HandleSize = _Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        FreeList.clear();
        FreeList.push_back({ 0u, kCapacity });
    }

    u32 FTextureSRVHeap::Allocate(u32 _Count) {
        if (_Count == 0) _Count = 1;

        for (size_t i = 0; i < FreeList.size(); ++i) {
            if (FreeList[i].Count >= _Count) {
                const u32 Slot = FreeList[i].Offset;
                FreeList[i].Offset += _Count;
                FreeList[i].Count  -= _Count;
                if (FreeList[i].Count == 0)
                    FreeList.erase(FreeList.begin() + i);
                return Slot;
            }
        }
        throw std::runtime_error("TextureSRVHeap capacity exceeded");
    }

    void FTextureSRVHeap::Free(u32 _Slot, u32 _Count) {
        if (_Count == 0) return;

        size_t i = 0;
        while (i < FreeList.size() && FreeList[i].Offset < _Slot) ++i;
        FreeList.insert(FreeList.begin() + i, { _Slot, _Count });

        if (i + 1 < FreeList.size() &&
            FreeList[i].Offset + FreeList[i].Count == FreeList[i + 1].Offset) {
            FreeList[i].Count += FreeList[i + 1].Count;
            FreeList.erase(FreeList.begin() + i + 1);
        }
        if (i > 0 &&
            FreeList[i - 1].Offset + FreeList[i - 1].Count == FreeList[i].Offset) {
            FreeList[i - 1].Count += FreeList[i].Count;
            FreeList.erase(FreeList.begin() + i);
        }
    }

    void FTextureSRVHeap::Release(u32& _Slot, u32 _Count) {
        if (_Slot == kInvalidSlot || _Count == 0) return;
        Free(_Slot, _Count);
        _Slot = kInvalidSlot;
    }

    void FTextureSRVHeap::CopyTable(ID3D12Device* _Device, u32 _DestinationSlot,
                                    std::span<const u32> _SourceSlots) const {
        if (_SourceSlots.empty()) return;

        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> SourceHandles;
        SourceHandles.reserve(_SourceSlots.size());
        for (const u32 Slot : _SourceSlots)
            SourceHandles.push_back(CpuHandleStaging(Slot));

        std::vector<UINT> SourceCounts(_SourceSlots.size(), 1u);
        D3D12_CPU_DESCRIPTOR_HANDLE Destination = CpuHandle(_DestinationSlot);
        UINT DestinationCount = static_cast<UINT>(_SourceSlots.size());
        _Device->CopyDescriptors(
            1, &Destination, &DestinationCount,
            static_cast<UINT>(SourceHandles.size()), SourceHandles.data(), SourceCounts.data(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FTextureSRVHeap::CreateSRV(ID3D12Device* _Device, ID3D12Resource* _Resource,
                                     const D3D12_SHADER_RESOURCE_VIEW_DESC& _SRVDesc, u32 _Slot) {
        _Device->CreateShaderResourceView(_Resource, &_SRVDesc, CpuHandle(_Slot));
        _Device->CreateShaderResourceView(_Resource, &_SRVDesc, CpuHandleStaging(_Slot));
    }

    void FTextureSRVHeap::CreateUAV(ID3D12Device* _Device, ID3D12Resource* _Resource,
                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC& _UAVDesc, u32 _Slot) {
        _Device->CreateUnorderedAccessView(_Resource, nullptr, &_UAVDesc, CpuHandle(_Slot));
        _Device->CreateUnorderedAccessView(_Resource, nullptr, &_UAVDesc, CpuHandleStaging(_Slot));
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

    D3D12_CPU_DESCRIPTOR_HANDLE FTextureSRVHeap::CpuHandleStaging(u32 _Slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle =
            StagingHeap->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(_Slot) * HandleSize;
        return Handle;
    }
} 
