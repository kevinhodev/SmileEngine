#pragma once

#include "Smile/Core/Types.h"

// Formato binario proprio cozido a partir do FBX pelo SmileCooker (Fase 2 da
// importacao). O runtime NUNCA le FBX direto — so estes arquivos (padrao Cry/Unreal).
// Dois arquivos por cena:
//   <cena>.smesh  — geometria: por mesh, Vertex[] (stride 32) + indices u32
//   <cena>.sscene — manifesto: tabela de materiais (caminhos de textura + flags) +
//                   tabela de renderaveis (indice de mesh + indice de material)
//
// Layout POD de alinhamento natural. Cooker e engine sao ambos MSVC x64, entao os
// structs casam byte-a-byte; escrita/leitura por memcpy do struct inteiro.

namespace Smile {
    constexpr u32 kSMeshMagic    = 0x48534D53u; // 'S''M''S''H' little-endian
    constexpr u32 kSSceneMagic   = 0x4E435353u; // 'S''S''C''N' little-endian
    constexpr u32 kCookedVersion = 1u;

    constexpr u32 kCookedMaxPath = 256u;
    constexpr u32 kCookedMaxName = 128u;

    // === .smesh ===
    struct SMeshHeader {
        u32 Magic;     // kSMeshMagic
        u32 Version;   // kCookedVersion
        u32 MeshCount;
        u32 Reserved;
        // Segue: SMeshEntry[MeshCount], depois o blob de geometria.
    };

    struct SMeshEntry {
        u32 VertexCount;
        u32 IndexCount;
        f32 AABBMin[3];
        f32 AABBMax[3];
        u64 VertexOffset; // offset (bytes, relativo ao inicio do blob) p/ Vertex[VertexCount]
        u64 IndexOffset;  // offset (bytes, relativo ao inicio do blob) p/ u32[IndexCount]
    };

    // === .sscene ===
    struct SSceneHeader {
        u32 Magic;   // kSSceneMagic
        u32 Version; // kCookedVersion
        u32 MaterialCount;
        u32 RenderableCount;
        // Segue: SSceneMaterial[MaterialCount], depois SSceneRenderable[RenderableCount].
    };

    struct SSceneMaterial {
        char Name[kCookedMaxName];
        // Caminhos de textura relativos ao diretorio da cena (string vazia = sem mapa).
        char BaseColor[kCookedMaxPath]; // RGB albedo + A opacidade. sRGB no load.
        char Specular[kCookedMaxPath];  // R=AO, G=Roughness, B=Metalness (slot MR + SpecularPacking)
        char Normal[kCookedMaxPath];    // BC5 DirectX (reconstroi Z; NormalFlipY no load)
        char Emissive[kCookedMaxPath];  // RGB emissivo. sRGB no load.
        f32  BaseColorFactor[4];
        f32  EmissiveFactor[3];
        f32  EmissiveStrength;
        u32  AlphaTest;   // 1 = material masked (clip por opacidade)
        f32  AlphaCutoff;
        u32  TwoSided;    // 1 = sem back-face cull (folhagem/toldos)
    };

    struct SSceneRenderable {
        u32 MeshIndex;     // indice em SMesh
        u32 MaterialIndex; // indice em SSceneMaterial; 0xFFFFFFFF = sem material (usa default)
    };

    constexpr u32 kNoMaterial = 0xFFFFFFFFu;
}
