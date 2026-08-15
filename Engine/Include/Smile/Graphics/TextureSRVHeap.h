#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <initializer_list>
#include <span>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FTextureSRVHeap {
    public:
        static constexpr u32 kCapacity    = 16384;
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        void Initialize(ID3D12Device* Device);

        u32  Allocate(u32 Count = 1);
        void Free(u32 Slot, u32 Count = 1);
        void Release(u32& Slot, u32 Count = 1);

        void CopyTable(ID3D12Device* Device, u32 DestinationSlot,
                       std::span<const u32> SourceSlots) const;
        void CopyTable(ID3D12Device* Device, u32 DestinationSlot,
                       std::initializer_list<u32> SourceSlots) const {
            CopyTable(Device, DestinationSlot,
                      std::span<const u32>(SourceSlots.begin(), SourceSlots.size()));
        }

        void CreateSRV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, u32 Slot);

        void CreateUAV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc, u32 Slot);

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(u32 Slot) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(u32 Slot) const;

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleStaging(u32 Slot) const;

        ID3D12DescriptorHeap* Native() const { return Heap.Get(); }

    private:
        struct FFreeRange { u32 Offset; u32 Count; };

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Heap;        
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> StagingHeap; 
        u32 HandleSize = 0;
        std::vector<FFreeRange> FreeList;
    };
} 
