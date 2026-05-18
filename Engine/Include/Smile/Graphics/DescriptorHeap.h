#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FDescriptorHeap {
    public:
        void Initialize(ID3D12Device* Device,
                        D3D12_DESCRIPTOR_HEAP_TYPE Type,
                        UINT Capacity,
                        bool ShaderVisible);

        ID3D12DescriptorHeap* Native() const { return DescriptorHeap.Get(); }
        UINT GetHandleSize() const { return HandleSize; }
        UINT GetCapacity() const { return Capacity; }

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index) const;

    private:
        ComPtr<ID3D12DescriptorHeap> DescriptorHeap;
        UINT HandleSize = 0;
        UINT Capacity   = 0;
        bool ShaderVisible = false;
    };
} 
