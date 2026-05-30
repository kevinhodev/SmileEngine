#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Smile {
    class FCommandQueue;

    enum class EDefaultTexture {
        White,
        FlatNormal,
        Black,
        MidGrey,
        ORM,
    };

    class FTexture {
    public:
        // Loads a texture from file (JPG/PNG/BMP/TIFF via WIC) and generates a
        // full mip chain via 2x2 box filtering. When IsNormalMap=true, mips are
        // built using vector-aware averaging + re-normalization, and the alpha
        // channel of each mip stores the Toksvig variance factor (T = |sum|/N).
        // T = 1.0 at mip 0 (no loss) and < 1.0 at higher mips where normals
        // disagreed within the source 2x2 footprint. The PS reads .a and bumps
        // roughness accordingly to suppress specular aliasing.
        static FTexture LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path,
                                     bool IsNormalMap = false);

        static FTexture CreateDefault(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      EDefaultTexture Type);

        ID3D12Resource* Resource()  const { return GpuResource.Get(); }
        u32             SRVSlot()   const { return Slot; }
        u32             Width()     const { return TexWidth; }
        u32             Height()    const { return TexHeight; }
        u32             MipCount()  const { return TexMipCount; }
        bool            IsValid()   const { return GpuResource != nullptr; }

    private:
        // One mip level of CPU-side pixel data, R8G8B8A8.
        struct FMipData {
            std::vector<u8> Pixels;
            u32             Width  = 0;
            u32             Height = 0;
        };

        // Uploads all mip levels of a single texture in one staging buffer +
        // one ExecuteAndSync. Caller owns the FMipData chain.
        static FTexture Upload(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap,
                               const std::vector<FMipData>& Mips,
                               DXGI_FORMAT Format);

        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32 Slot        = 0;
        u32 TexWidth    = 0;
        u32 TexHeight   = 0;
        u32 TexMipCount = 1;
    };
}
