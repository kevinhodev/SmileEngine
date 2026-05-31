#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Math/Vec4.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace Smile {
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
        u32   HasEmissiveMap          = 0;
        float NormalStrength          = 1.0f;
        u32   NormalFlipY             = 0;       // 0 = normal map OpenGL/GL (default); 1 = DirectX

        // Parallax Occlusion Mapping (height map in texture slot 5)
        u32   HasHeightMap            = 0;
        float HeightScale             = 0.05f;   // max UV displacement at depth 1
        float ParallaxMinSteps        = 8.0f;    // samples head-on
        float ParallaxMaxSteps        = 32.0f;   // samples at grazing angles
        u32   ParallaxSelfShadow      = 0;       // 1 = trace soft self-shadow
        float ParallaxShadowSteps     = 16.0f;
        float ParallaxFadeStart       = 3.0f;    // height-map mip where POM begins to fade
        float ParallaxFadeRange       = 5.0f;    // mips over which POM fades to flat (0 = off)
        u32   ParallaxRefine          = 0;       // 1 = binary-search refine the hit (sharper)
        u32   ParallaxRefineSteps     = 5;       // binary-search iterations (1 = cheapest)

        // Separate metalness/roughness maps (slots 6/7), alternative to combined MR (slot 2)
        u32   HasMetalnessMap         = 0;
        u32   HasRoughnessMap         = 0;

        u8    _Pad[132] = {};
    };
    static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants must be 256 bytes");

    inline constexpr u32 kMaterialTextureSlots = 8;

    class FMaterial {
    public:
        FTexture* Albedo            = nullptr; 
        FTexture* Normal            = nullptr; 
        FTexture* MetallicRoughness = nullptr; 
        FTexture* AO                = nullptr;
        FTexture* Emissive          = nullptr;
        FTexture* Height            = nullptr;
        FTexture* Metalness         = nullptr;
        FTexture* Roughness         = nullptr;

        MaterialConstants Constants;

        void Finalize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        void Bind(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap) const;

        void UpdateConstants();

        void UpdateTextureSlot(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 LocalSlot, FTexture* Texture);

        bool IsFinalized() const { return CBV != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        MaterialConstants* MappedCBV = nullptr; 
        u32 SRVTableStart            = 0;       
    };
} 
