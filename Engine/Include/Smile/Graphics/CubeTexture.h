#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    // Cubemap GPU resource: 6 array slices, optionally N mip levels, optionally
    // bindable as UAV (one per mip) for compute writes during IBL prefiltering.
    class FCubeTexture {
    public:
        // Creates a TextureCube resource. When AllowUAV is true, the resource is
        // bound first in UNORDERED_ACCESS state and one UAV is allocated per mip
        // in the SRV heap (Texture2DArray view over the 6 faces). The TextureCube
        // SRV is always allocated at FaceSRVSlot.
        void Create(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    DXGI_FORMAT Format, u32 Size, u32 MipLevels, bool AllowUAV);

        // Transitions all subresources to the requested state, using the
        // internally-tracked current state. No-op when already in that state.
        void Transition(ID3D12GraphicsCommandList* CommandList,
                        D3D12_RESOURCE_STATES After);

        // Transitions all 6 faces of one mip slice to the requested state.
        // Used by mip-gen passes to ping-pong individual mips between UAV
        // (writing) and SHADER_RESOURCE (reading) without disturbing others.
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
        // Per-subresource state, indexed as MipSlice * 6 + FaceSlice. Avoids
        // any global "unified state" book-keeping; each transition is local.
        std::vector<D3D12_RESOURCE_STATES> MipStates;
    };
}
