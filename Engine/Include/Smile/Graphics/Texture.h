#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

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
        static FTexture LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path);

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
                               const u8* Pixels, u32 Width, u32 Height,
                               DXGI_FORMAT Format);

        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32 Slot      = 0;
        u32 TexWidth  = 0;
        u32 TexHeight = 0;
    };
} 
