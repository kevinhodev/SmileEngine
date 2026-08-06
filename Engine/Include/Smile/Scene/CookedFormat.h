#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    constexpr u32 kSMeshMagic    = 0x48534D53u; 
    constexpr u32 kSSceneMagic   = 0x4E435353u; 
    constexpr u32 kCookedVersion = 7u; // v7: o transform do no NAO e mais bakeado no vertice. A geometria
                                       //     sai em espaco LOCAL (AABB da SMeshEntry idem) e cada renderavel
                                       //     carrega nome do no, TRS de mundo e indice do pai. Com isso o
                                       //     cooker passa a DEDUPLICAR mesh por (ufbx_mesh, parte): N nos que
                                       //     compartilham a mesma malha viram N instancias de UMA geometria
                                       //     (Emerald Square: 2479 -> 281 partes). Medido antes de escrever:
                                       //     TRS reproduz geometry_to_world com erro <= 7e-12 nas 3 cenas
                                       //     (nenhuma tem shear nem determinante negativo).
                                       // v6: vidro por nome -> Blend translucido (alpha 0.4, two-sided) p/ o
                                       //     passe forward; vidro emissivo segue opaco (glow no deferred)
                                       // v5: normais pela inversa-transposta + winding por no espelhado; tambem
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
        // LOCAL desde a v7 (era de mundo). Quem precisa da caixa de mundo transforma pelo TRS do
        // renderavel — e o loader faz isso no load, porque o culling e o HiZ leem mundo.
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
        u32 MeshIndex;     // indice em SMesh. VARIOS renderaveis podem apontar para o MESMO: e o
                           // instancing que a v7 destrava (dedup por (ufbx_mesh, parte) no cooker).
        u32 MaterialIndex; // indice em SSceneMaterial; 0xFFFFFFFF = sem material (usa default)

        // Transform de MUNDO do no, no formato do FTransform da engine (S * Rx*Ry*Rz * T,
        // row-vector, rotacao em RADIANOS). Mundo e nao local porque na v7 o runtime ainda nao
        // tem hierarquia — o transform vai direto como matriz de modelo. O ParentIndex abaixo ja
        // e gravado para a fase da hierarquia converter mundo->local sem re-cozinhar.
        f32 Position[3];
        f32 RotationEuler[3];
        f32 Scale[3];

        // Primeiro renderavel do ANCESTRAL mais proximo que tambem virou renderavel; -1 = raiz.
        // ⚠️ Nos de GRUPO (sem mesh) colapsam: a arvore aqui e a dos nos que renderizam. Guardar
        // os grupos exige uma tabela de nos propria, que e trabalho da fase de hierarquia.
        i32 ParentIndex;

        char Name[kCookedMaxName]; // nome do NO (a v6 nao guardava, e o outliner caia no do material)
    };

    constexpr u32 kNoMaterial = 0xFFFFFFFFu;
}
