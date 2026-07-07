#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    constexpr u32 kSMeshMagic    = 0x48534D53u; 
    constexpr u32 kSSceneMagic   = 0x4E435353u; 
    constexpr u32 kCookedVersion = 5u; // v5: normais pela inversa-transposta + winding por no espelhado; tambem
                                       //     invalida cozidos anteriores a "fator neutro com textura" no emissivo
                                       // v4: +fatores PBR lidos do material (Metallic/RoughnessFactor) + Blend (alpha translucido)
                                       // v3: +Metalness/Roughness separados (PBR metal/rough nao-packed, ex.: Sponza PNG)
                                       // v2: +SSceneMaterial::Foliage (shading model desacoplado de masked)

    constexpr u32 kCookedMaxPath = 256u;
    constexpr u32 kCookedMaxName = 128u;

    struct SMeshHeader {
        u32 Magic;    
        u32 Version;   
        u32 MeshCount;
        u32 Reserved;
    };

    struct SMeshEntry {
        u32 VertexCount;
        u32 IndexCount;
        f32 AABBMin[3];
        f32 AABBMax[3];
        u64 VertexOffset; // offset (bytes, relativo ao inicio do blob) p/ Vertex[VertexCount]
        u64 IndexOffset;  // offset (bytes, relativo ao inicio do blob) p/ u32[IndexCount]
    };

    struct SSceneHeader {
        u32 Magic;   // kSSceneMagic
        u32 Version; // kCookedVersion
        u32 MaterialCount;
        u32 RenderableCount;
    };

    struct SSceneMaterial {
        char Name[kCookedMaxName];
        char BaseColor[kCookedMaxPath]; // RGB albedo + A opacidade. sRGB no load.
        char Specular[kCookedMaxPath];  // R=AO, G=Roughness, B=Metalness (slot MR + SpecularPacking)
        char Normal[kCookedMaxPath];    // BC5 DirectX (reconstroi Z; NormalFlipY no load)
        char Emissive[kCookedMaxPath];  // RGB emissivo. sRGB no load.
        char Metalness[kCookedMaxPath]; // metalico no canal R (slot t6). Alternativa ao Specular packed.
        char Roughness[kCookedMaxPath]; // rugosidade no canal R (slot t7). Alternativa ao Specular packed.
        f32  BaseColorFactor[4]; // RGB tint + A opacidade (lido do material; A<1 + Blend => translucido)
        f32  EmissiveFactor[3];  // cor emissiva (emission_color * emission_factor)
        f32  EmissiveStrength;
        f32  MetallicFactor;     // usado quando NAO ha mapa metalico/MR (ex.: vidro/lampada sem textura)
        f32  RoughnessFactor;    // idem p/ rugosidade (vidro=0 -> reflexivo; lampada fosca ~0.4)
        u32  AlphaTest;   // 1 = material masked (clip por opacidade)
        f32  AlphaCutoff;
        u32  TwoSided;    // 1 = sem back-face cull (folhagem/toldos/cutouts)
        u32  Foliage;     // 1 = shading model folhagem (transmissao two-sided). Desacoplado de
                          // AlphaTest/TwoSided: um cutout (corrente, grade) e masked mas NAO folhagem.
        u32  Blend;       // 1 = translucido (alpha-blend num passe forward; ex.: dirt_decal). Mutuamente
                          // exclusivo com AlphaTest na pratica (cutout e opaco no GBuffer).
    };

    struct SSceneRenderable {
        u32 MeshIndex;     // indice em SMesh
        u32 MaterialIndex; // indice em SSceneMaterial; 0xFFFFFFFF = sem material (usa default)
    };

    constexpr u32 kNoMaterial = 0xFFFFFFFFu;
}
