#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {

    // Shader-visible CBV/SRV/UAV heap with a simple linear allocator.
    // Each FTexture and FMaterial allocates slots here at load time.
    class FTextureSRVHeap {
    public:
        static constexpr u32 kCapacity = 512;

        void Initialize(ID3D12Device* Device);

        // Returns the descriptor slot index. Never freed — assumes static lifetime for now.
        u32  Allocate(u32 Count = 1);

        void CreateSRV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, u32 Slot);

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(u32 Slot) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(u32 Slot) const;

        ID3D12DescriptorHeap* Native() const { return Heap.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Heap;
        u32 HandleSize  = 0;
        u32 NextSlot    = 0;
    };

} // namespace Smile
