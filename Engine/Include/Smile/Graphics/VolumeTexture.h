#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    // 3D (volume) GPU texture: Width x Height x Depth, optionally N mip levels,
    // optionally bindable as UAV (one per mip) for compute writes. Modeled on
    // FCubeTexture — used by the atmosphere aerial-perspective froxel volume and
    // by the volumetric-cloud noise / shadow volumes.
    class FVolumeTexture {
    public:
        // Creates a TEXTURE3D resource. When AllowUAV is true the resource starts
        // in UNORDERED_ACCESS and one Texture3D UAV is allocated per mip in the SRV
        // heap. The Texture3D SRV is always allocated at VolumeSRVSlot.
        void Create(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                    DXGI_FORMAT _Format, u32 _Width, u32 _Height, u32 _Depth,
                    u32 _MipLevels, bool _AllowUAV);

        // Transitions all mips to the requested state (tracked internally).
        void Transition(ID3D12GraphicsCommandList* _CommandList,
                        D3D12_RESOURCE_STATES _After);

        // Transitions a single mip slice (a 3D texture mip is one subresource).
        void TransitionMip(ID3D12GraphicsCommandList* _CommandList,
                           u32 _MipSlice, D3D12_RESOURCE_STATES _After);

        ID3D12Resource* Resource()       const { return GpuResource.Get(); }
        u32             SRVSlot()        const { return VolumeSRVSlot; }
        u32             UAVSlot(u32 Mip) const { return UAVSlots[Mip]; }
        u32             Width()          const { return TexWidth; }
        u32             Height()         const { return TexHeight; }
        u32             Depth()          const { return TexDepth; }
        u32             MipCount()       const { return MipLevels; }
        DXGI_FORMAT     Format()         const { return TexFormat; }
        bool            IsValid()        const { return GpuResource != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32                   VolumeSRVSlot = 0;
        std::vector<u32>      UAVSlots;
        u32                   TexWidth  = 0;
        u32                   TexHeight = 0;
        u32                   TexDepth  = 0;
        u32                   MipLevels = 0;
        DXGI_FORMAT           TexFormat = DXGI_FORMAT_UNKNOWN;
        // Per-subresource state. A 3D texture has no array slices, so the
        // subresource index is simply the mip slice.
        std::vector<D3D12_RESOURCE_STATES> MipStates;
    };
}
