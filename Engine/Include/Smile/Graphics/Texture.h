#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {

    class FCommandQueue;

    enum class EDefaultTexture {
        White,       // (1,1,1,1) — albedo, AO fallback
        FlatNormal,  // (0.5,0.5,1,1) — neutral normal map
        Black,       // (0,0,0,1) — emissive fallback
        MidGrey,     // (0.5,0.5,0.5,1) — height fallback
        ORM,         // (0,0.5,0,1) — metallic=0, roughness=0.5 fallback
    };

    class FTexture {
    public:
        // Loads an image from disk (PNG, JPG, BMP, TIFF) via WIC.
        // Uploads to GPU DEFAULT heap and creates SRV in the provided heap.
        static FTexture LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path);

        // Creates a 1x1 fallback texture of the specified type.
        static FTexture CreateDefault(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      EDefaultTexture Type);

        ID3D12Resource* Resource()  const { return GpuResource.Get(); }
        u32             SRVSlot()   const { return Slot; }
        u32             Width()     const { return TexWidth; }
        u32             Height()    const { return TexHeight; }
        bool            IsValid()   const { return GpuResource != nullptr; }

    private:
        static FTexture Upload(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap,
                               const u8* Pixels, u32 W, u32 H,
                               DXGI_FORMAT Format);

        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32 Slot      = 0;
        u32 TexWidth  = 0;
        u32 TexHeight = 0;
    };

} // namespace Smile
