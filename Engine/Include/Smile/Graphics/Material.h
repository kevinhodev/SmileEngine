#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Math/Vec4.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>

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

        u32   HasMetalnessMap         = 0;
        u32   HasRoughnessMap         = 0;

        u32   SpecularPacking         = 0;       // 1 = mapa MR (t2) eh "Specular": R=AO, G=Rough, B=Metal
        u32   AlphaTest               = 0;       // 1 = clip por opacidade no alpha do BaseColor (folhagem)
        float AlphaCutoff             = 0.5f;
        u32   NormalReconstructZ      = 0;       // 1 = normal map BC5 (so RG) -> reconstruir Z; Toksvig=1

        u32   ShadingModel            = 0;       // 0 = DefaultLit, 1 = Foliage (two-sided + transmissao)
        Vec4  SubsurfaceColor         = { 1.0f, 1.0f, 1.0f, 0.0f };

        u8    _Pad[96] = {};
    };
    static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants must be 256 bytes");

    inline constexpr u32 kMaterialTextureSlots = 8;

    class FMaterial {
    public:
        std::string Name; // nome do material cozido (SSceneMaterial::Name) — editor/outliner

        FTexture* Albedo            = nullptr;
        FTexture* Normal            = nullptr; 
        FTexture* MetallicRoughness = nullptr; 
        FTexture* AO                = nullptr;
        FTexture* Emissive          = nullptr;
        FTexture* Height            = nullptr;
        FTexture* Metalness         = nullptr;
        FTexture* Roughness         = nullptr;

        MaterialConstants Constants;

        bool TwoSided = false;
        bool Blend    = false; // translucido -> desenhado no passe forward (alpha-blend), nao no GBuffer

        void Finalize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        void Release(FTextureSRVHeap& SRVHeap);

        void Bind(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap) const;

        void UpdateConstants();

        void UpdateTextureSlot(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 LocalSlot, FTexture* Texture);

        bool IsFinalized() const { return CBV != nullptr; }

        u32  AlbedoDescriptorIndex() const { return SRVTableStart; }
        bool HasAlbedoTexture()      const { return Constants.HasAlbedoMap != 0; }

    private:
        static constexpr u32 kInvalidTable = 0xFFFFFFFFu;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        MaterialConstants* MappedCBV = nullptr;
        u32 SRVTableStart            = kInvalidTable;
    };
} 
