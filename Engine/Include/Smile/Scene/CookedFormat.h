#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    constexpr u32 kSMeshMagic    = 0x48534D53u; 
    constexpr u32 kSSceneMagic   = 0x4E435353u; 
    constexpr u32 kCookedVersion = 2u; // v2: +SSceneMaterial::Foliage (shading model desacoplado de masked)

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
        f32  BaseColorFactor[4];
        f32  EmissiveFactor[3];
        f32  EmissiveStrength;
        u32  AlphaTest;   // 1 = material masked (clip por opacidade)
        f32  AlphaCutoff;
        u32  TwoSided;    // 1 = sem back-face cull (folhagem/toldos/cutouts)
        u32  Foliage;     // 1 = shading model folhagem (transmissao two-sided). Desacoplado de
                          // AlphaTest/TwoSided: um cutout (corrente, grade) e masked mas NAO folhagem.
    };

    struct SSceneRenderable {
        u32 MeshIndex;     // indice em SMesh
        u32 MaterialIndex; // indice em SSceneMaterial; 0xFFFFFFFF = sem material (usa default)
    };

    constexpr u32 kNoMaterial = 0xFFFFFFFFu;
}
