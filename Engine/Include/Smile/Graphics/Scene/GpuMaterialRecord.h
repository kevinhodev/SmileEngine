#pragma once

// Linha CANONICA de material da GPU scene (Fase 2 do GPU-DRIVEN-PLAN).
//
// Por que existe: hoje TODO campo de material que o ray tracing enxerga esta COPIADO dentro do
// snapshot por instancia (FRTInstanceGeo, RaytracingScene.cpp). Editar a cor de um material
// significa reescrever o registro de TODAS as instancias da cena — 2.479 registros na Emerald
// para mexer num material que talvez pertenca a tres delas — e, como aquele snapshot mora num
// upload heap sem versao por frame em voo, a reescrita ainda exige drenar as duas filas.
//
// A linha aqui e a fonte: a instancia passa a carregar um MaterialId, e uma edicao toca UMA linha.
//
// CPU PURO: nao aloca recurso e nao conhece device. O que cria o buffer, aloca descritor e sobe
// linha suja e a FGpuMaterialTable (GpuSceneTables.h); esta separacao e o que permite testar a
// ABI e a derivacao a partir do FMaterial sem GPU nenhuma.
//
// ATENCAO: layout consumido pela GPU e espelhado em HLSL (Shaders/Scene/GpuSceneTables.hlsli).
// Qualquer mudanca de campo, ordem ou tamanho exige atualizar o espelho e o teste de ABI
// (Tests/GpuSceneTablesTests.cpp) no MESMO commit.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Math/Vec4.h"

#include <cstddef>

namespace Smile {
    // Bits de FGpuMaterialRecord::Flags.
    //
    // Os oito primeiros preservam os valores numericos dos INSTGEO_FLAG_* que ja viajavam no
    // snapshot por instancia — manter o numero e o que permite provar a migracao byte a byte.
    // Os dois ultimos absorvem campos que eram u32 inteiros (HasAlbedo, TwoSidedRT) e que nunca
    // carregaram mais que um bit de informacao.
    inline constexpr u32 kMaterialFlagAlphaTest   = 1u;   // clip por albedo.a vs AlphaCutoff
    inline constexpr u32 kMaterialFlagEmissiveMap = 2u;   // tem mapa emissivo
    inline constexpr u32 kMaterialFlagFoliage     = 4u;   // ShadingModel Foliage
    inline constexpr u32 kMaterialFlagMRMap       = 8u;   // tem mapa metallic-roughness
    inline constexpr u32 kMaterialFlagSpecPacked  = 16u;  // o mapa MR e "Specular" (R=AO,G=rough,B=metal)
    inline constexpr u32 kMaterialFlagMetalMap    = 32u;  // tem mapa Metalness separado
    inline constexpr u32 kMaterialFlagRoughMap    = 64u;  // tem mapa Roughness separado
    inline constexpr u32 kMaterialFlagTranslucent = 128u; // material Blend
    inline constexpr u32 kMaterialFlagAlbedoMap   = 256u; // era o u32 HasAlbedo
    inline constexpr u32 kMaterialFlagTwoSided    = 512u; // era o u32 TwoSidedRT (IsTwoSidedForRT)

    // Deslocamentos dentro da tabela contigua de descritores do material.
    //
    // O FMaterial reserva kMaterialTextureSlots descritores CONSECUTIVOS a partir de
    // AlbedoDescriptorIndex(), entao os cinco indices que o RT usava (albedo, MR, emissivo,
    // metalness, roughness) eram, os cinco, a mesma base somada a uma constante. Guardar so a
    // base devolve 16 B por material e elimina quatro campos que nunca puderam divergir.
    inline constexpr u32 kMaterialTexAlbedo   = 0u;
    inline constexpr u32 kMaterialTexNormal   = 1u;
    inline constexpr u32 kMaterialTexMR       = 2u;
    inline constexpr u32 kMaterialTexAO       = 3u;
    inline constexpr u32 kMaterialTexEmissive = 4u;
    inline constexpr u32 kMaterialTexHeight   = 5u;
    inline constexpr u32 kMaterialTexMetal    = 6u;
    inline constexpr u32 kMaterialTexRough    = 7u;
    static_assert(kMaterialTexRough < kMaterialTextureSlots,
                  "os deslocamentos tem de caber na tabela que o FMaterial reserva");

    struct FGpuMaterialRecord {
        // Base invalida de descritor. Vale quando o material ainda nao foi finalizado (nao ha
        // tabela de SRV) ou quando a instancia nao tem material nenhum. Nesses casos TODOS os
        // bits de mapa ficam desligados, entao nenhum shader chega a somar um deslocamento a
        // este valor — ele existe para que um bug de flag apareca como descritor absurdo em vez
        // de ler o descritor 0, que pertence a outra coisa qualquer.
        static constexpr u32 kInvalidTextureBase = 0xFFFFFFFFu;

        // Os defaults abaixo sao os do renderavel SEM material, e nao zeros arbitrarios: e
        // exatamente o que o FillInstanceGeo escrevia quando R.Material era nulo. Uma linha
        // default-construida e, portanto, a linha do material default.
        Vec4 BaseColor        = { 0.7f, 0.7f, 0.7f, 1.0f };
        // rgb = EmissiveFactor * EmissiveStrength * RTEmissiveScale; w = MetallicFactor.
        // O empacotamento vem do snapshot antigo e e preservado: o .w evita um campo solto de 4 B
        // e ja e como todo shader de hit le o metallic.
        Vec4 EmissiveFactor   = { 0.0f, 0.0f, 0.0f, 0.0f };
        // tint rgb + intensidade em a (mesmo dado do G-buffer).
        Vec4 SubsurfaceColor  = { 1.0f, 1.0f, 1.0f, 0.0f };
        f32  RoughnessFactor  = 0.5f;
        f32  AlphaCutoff      = 0.5f;
        u32  Flags            = 0;
        u32  TextureBase      = kInvalidTextureBase;

        // Deriva a linha do material da engine. Estatica e sem estado: a linha e funcao PURA do
        // FMaterial, e e isso que torna "material mudou" equivalente a "a linha mudou".
        //
        // Material nulo devolve a linha default (ver acima).
        static FGpuMaterialRecord FromMaterial(const FMaterial* Material);

        // A mesma derivacao, recebendo as partes soltas em vez do objeto.
        //
        // Existe para o TESTE, e a razao e concreta: os tres estados que o FMaterial usa para
        // decidir indice de textura — MappedCBV, SRVTableStart, CBSlot — sao PRIVADOS e so sao
        // preenchidos pelo Finalize, que exige ID3D12Device. Sem esta porta, todo caminho de
        // material finalizado (os cinco mapas bindless e o SpecularPacking) ficaria fora de
        // qualquer teste sem GPU, e a migracao do snapshot seria provada apenas por comparacao
        // visual.
        //
        // TextureBase == kInvalidTextureBase significa "material nao finalizado": nenhum bit de
        // mapa e ligado, exatamente como o snapshot antigo fazia sob !IsFinalized().
        static FGpuMaterialRecord FromParts(const MaterialConstants& Constants,
                                            bool TwoSidedForRT, bool Blend, u32 TextureBase);

        // Indice bindless de um mapa. So tem sentido sob o bit correspondente; sem ele devolve
        // kInvalidTextureBase em vez de um vizinho da base invalida.
        u32 TextureIndex(u32 Offset) const {
            return TextureBase == kInvalidTextureBase ? kInvalidTextureBase : TextureBase + Offset;
        }

        friend bool operator==(const FGpuMaterialRecord& A, const FGpuMaterialRecord& B) {
            auto Same4 = [](const Vec4& L, const Vec4& R) {
                return L.X == R.X && L.Y == R.Y && L.Z == R.Z && L.W == R.W;
            };
            return Same4(A.BaseColor, B.BaseColor) && Same4(A.EmissiveFactor, B.EmissiveFactor) &&
                   Same4(A.SubsurfaceColor, B.SubsurfaceColor) &&
                   A.RoughnessFactor == B.RoughnessFactor && A.AlphaCutoff == B.AlphaCutoff &&
                   A.Flags == B.Flags && A.TextureBase == B.TextureBase;
        }
        friend bool operator!=(const FGpuMaterialRecord& A, const FGpuMaterialRecord& B) {
            return !(A == B);
        }
    };

    // 64 B: tres Vec4 alinhados em 4 (a Smile nao usa alignas em Vec4) seguidos de quatro
    // escalares de 4 B. StructuredBuffer nao exige stride multiplo de 16, mas o multiplo sai de
    // graca aqui e mantem cada linha dentro de uma unica cache line de 64 B — que e o acesso do
    // hit de RT: uma linha inteira, uma vez.
    static_assert(sizeof(FGpuMaterialRecord) == 64,
                  "FGpuMaterialRecord e layout de GPU: 64 B, espelhado no HLSL");
    static_assert(alignof(FGpuMaterialRecord) == 4, "sem alinhamento implicito surpresa");
    static_assert(offsetof(FGpuMaterialRecord, BaseColor)       ==  0);
    static_assert(offsetof(FGpuMaterialRecord, EmissiveFactor)  == 16);
    static_assert(offsetof(FGpuMaterialRecord, SubsurfaceColor) == 32);
    static_assert(offsetof(FGpuMaterialRecord, RoughnessFactor) == 48);
    static_assert(offsetof(FGpuMaterialRecord, AlphaCutoff)     == 52);
    static_assert(offsetof(FGpuMaterialRecord, Flags)           == 56);
    static_assert(offsetof(FGpuMaterialRecord, TextureBase)     == 60);
}
