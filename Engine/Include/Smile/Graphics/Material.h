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
        u32   NormalFlipY             = 0;
        u32   HasHeightMap            = 0;
        float POMHeightScale          = 0.05f;
        float POMMinSteps             = 8.0f;
        float POMMaxSteps             = 32.0f;
        u32   POMEnableShadows        = 1;
        u32   POMEnableSilhouette     = 0;
        u8    _Pad[156] = {};
    };
    static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants must be 256 bytes");

    inline constexpr u32 kMaterialTextureSlots = 6;

    class FMaterial {
    public:
        FTexture* Albedo            = nullptr; 
        FTexture* Normal            = nullptr; 
        FTexture* MetallicRoughness = nullptr; 
        FTexture* AO                = nullptr; 
        FTexture* Emissive          = nullptr; 
        FTexture* Height            = nullptr;

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
