#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Math/Vec4.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace Smile {

    // GPU-side constant buffer bound at register(b1).
    // Must stay 256-byte aligned (D3D12 CBV requirement).
    struct alignas(256) MaterialConstants {
        Vec4  BaseColorFactor         = { 1.0f, 1.0f, 1.0f, 1.0f };
        float MetallicFactor          = 0.0f;
        float RoughnessFactor         = 0.5f;
        float AOStrength              = 1.0f;
        float EmissiveStrength        = 1.0f;
        Vec4  EmissiveFactor          = { 0.0f, 0.0f, 0.0f, 1.0f };
        u32   HasAlbedoMap            = 0;
        u32   HasNormalMap            = 0;
        u32   HasMetallicRoughnessMap = 0;
        u32   HasAOMap                = 0;
        u32   HasHeightMap            = 0;
        u32   HasEmissiveMap          = 0;
        float NormalStrength          = 1.0f;
        float HeightScale             = 0.05f;
        // DirectX normal maps have the G channel inverted vs OpenGL.
        // Set to 1 to flip Y before applying TBN (files named *NormalDX, *_N_dx, etc.)
        u32   NormalFlipY             = 0;
        // Explicit pad: 84 bytes used → 172 bytes padding
        u8    _pad[172] = {};
    };
    static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants must be 256 bytes");

    // Number of texture slots per material (must match shader registers t0-t5)
    inline constexpr u32 kMaterialTextureSlots = 6;

    class FMaterial {
    public:
        // Texture slots — set before calling Finalize()
        FTexture* Albedo            = nullptr; // t0
        FTexture* Normal            = nullptr; // t1
        FTexture* MetallicRoughness = nullptr; // t2  R=Metallic, G=Roughness (glTF)
        FTexture* AO                = nullptr; // t3
        FTexture* Height            = nullptr; // t4
        FTexture* Emissive          = nullptr; // t5

        MaterialConstants Constants;

        // Allocates kMaterialTextureSlots contiguous SRV slots and uploads CBV.
        // Must be called after all textures are assigned.
        void Finalize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        // Sets root parameters 1 (CBV b1) and 2 (SRV table t0-t5) on the command list.
        void Bind(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap) const;

        // Copies MaterialConstants to the GPU buffer (call when constants change).
        void UpdateConstants();

        // Overwrites a single SRV in the material's allocated block without re-allocating.
        // Safe to call between frames; GPU sees the new descriptor on the next draw.
        // LocalSlot: 0=Albedo 1=Normal 2=MetallicRoughness 3=AO 4=Height 5=Emissive
        void UpdateTextureSlot(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 LocalSlot, FTexture* Tex);

        bool IsFinalized() const { return CBV != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        MaterialConstants* MappedCBV = nullptr; // persistent mapped pointer
        u32 SRVTableStart            = 0;       // first of kMaterialTextureSlots slots
    };

} // namespace Smile
