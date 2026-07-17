#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Smile {
    class FUploadQueue;

    enum class EDefaultTexture {
        White,
        FlatNormal,
        Black,
        MidGrey,
        ORM,
    };

    struct FMipData {
        std::vector<u8> Pixels;
        u32             Width  = 0;
        u32             Height = 0;
    };

    struct FTextureCPUData {
        std::vector<FMipData> Mips;
        u32         Width   = 0;
        u32         Height  = 0;
        DXGI_FORMAT Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool        IsNormalMap = false;
        bool Valid() const { return !Mips.empty(); }
    };

    class FTexture {
    public:
        static FTexture LoadFromFile(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path,
                                     bool IsNormalMap = false);

        static FTextureCPUData LoadCPU(const std::wstring& Path, bool IsNormalMap = false, bool sRGB = false);

        // Category permite contabilizar a VRAM fora de SceneTextures (ex.: heightmap do
        // terreno na categoria Terrain).
        static FTexture CreateFromCPU(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      const FTextureCPUData& Data,
                                      EVramCategory Category = EVramCategory::SceneTextures);

        static FTextureCPUData LoadDDSCPU(const std::wstring& Path, bool sRGB);
        static FTexture        LoadDDS(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                       FTextureSRVHeap& SRVHeap,
                                       const std::wstring& Path, bool sRGB);

        static std::vector<FTexture> CreateBatchFromCPU(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                                        FTextureSRVHeap& SRVHeap,
                                                        const std::vector<FTextureCPUData>& Data);

        static FTexture CreateDefault(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      EDefaultTexture Type);

        void Release(FTextureSRVHeap& SRVHeap);

        ID3D12Resource* Resource()  const { return GpuResource.Get(); }
        DXGI_FORMAT     Format()    const { return TexFormat; }
        u32             SRVSlot()   const { return Slot; }
        u32             Width()     const { return TexWidth; }
        u32             Height()    const { return TexHeight; }
        u32             MipCount()  const { return TexMipCount; }
        bool            IsValid()   const { return GpuResource != nullptr; }

    private:
        static FTexture Upload(ID3D12Device* Device, FUploadQueue& UploadQueue,
                               FTextureSRVHeap& SRVHeap,
                               const std::vector<FMipData>& Mips,
                               DXGI_FORMAT Format,
                               EVramCategory Category = EVramCategory::SceneTextures);

        static FTexture RecordUpload(ID3D12Device* Device, ID3D12GraphicsCommandList* CommandList,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::vector<FMipData>& Mips, DXGI_FORMAT Format,
                                     std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& StagingOut,
                                     EVramCategory Category = EVramCategory::SceneTextures);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        Microsoft::WRL::ComPtr<ID3D12Resource> GpuResource;
        u32 Slot        = kInvalidSlot;
        u32 TexWidth    = 0;
        u32 TexHeight   = 0;
        u32 TexMipCount = 1;
        DXGI_FORMAT TexFormat = DXGI_FORMAT_R8G8B8A8_UNORM; 
    };
}
