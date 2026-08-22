#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FCubeTexture {
    public:
        void Create(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    DXGI_FORMAT Format, u32 Size, u32 MipLevels, bool AllowUAV);

        void Transition(ID3D12GraphicsCommandList* CommandList,
                        D3D12_RESOURCE_STATES After);
        void TransitionMip(ID3D12GraphicsCommandList* CommandList,
                           u32 MipSlice, D3D12_RESOURCE_STATES After);

        ID3D12Resource* Resource()       const { return GpuResource.Get(); }
        u32             SRVSlot()        const { return FaceSRVSlot; }
        u32             UAVSlot(u32 Mip) const { return UAVSlots[Mip]; }
        u32             Size()           const { return TexSize; }
        u32             MipCount()       const { return MipLevels; }
        DXGI_FORMAT     Format()         const { return TexFormat; }
        bool            IsValid()        const { return GpuResource != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32                   FaceSRVSlot  = 0;
        std::vector<u32>      UAVSlots;
        u32                   TexSize      = 0;
        u32                   MipLevels    = 0;
        DXGI_FORMAT           TexFormat    = DXGI_FORMAT_UNKNOWN;
        std::vector<D3D12_RESOURCE_STATES> MipStates;
    };
}
