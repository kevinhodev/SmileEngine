// Linha canonica de material (Smile/Graphics/Scene/GpuMaterialRecord.h).
//
// O que este teste existe para impedir: a Fase 2 tira os campos de material de dentro do snapshot
// por instancia do RT e os move para uma tabela de uma linha por material. Se a DERIVACAO mudar
// junto, a comparacao visual do fim da fase acusaria uma diferenca sem dizer se ela veio da nova
// tabela ou de um campo calculado diferente no caminho.
//
// Por isso o oraculo abaixo NAO chama a engine: ele reescreve, a mao, os treze campos de material
// exatamente como o FillInstanceGeo os calculava antes da migracao (RaytracingScene.cpp, commit
// 439e7b4). O teste falha se a linha nova divergir do snapshot antigo em qualquer combinacao.
//
// A matriz cobre finalizado/nao-finalizado porque e a unica bifurcacao real da derivacao: sem
// tabela de descritores nenhum bit de mapa pode ser ligado, e era ali que o snapshot antigo
// gravava indice 0xFFFFFFFF com o bit ligado.

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Smile/Graphics/Scene/GpuMaterialRecord.h"

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    using namespace Smile;

    // O snapshot por instancia como ele era: treze campos de material soltos.
    struct FLegacyGeoMaterial {
        Vec4 BaseColor{ 0.7f, 0.7f, 0.7f, 1.0f };
        u32  AlbedoIndex      = 0;
        u32  HasAlbedo        = 0;
        u32  TwoSidedRT       = 0;
        u32  Flags            = 0;
        f32  AlphaCutoff      = 0.5f;
        f32  RoughnessFactor  = 0.5f;
        Vec4 EmissiveFactor{ 0.0f, 0.0f, 0.0f, 0.0f };
        u32  EmissiveMapIndex = 0;
        u32  MrMapIndex       = 0;
        u32  MetalMapIndex    = 0;
        u32  RoughMapIndex    = 0;
        Vec4 SubsurfaceColor{ 1.0f, 1.0f, 1.0f, 0.0f };
    };

    // Transcricao literal do FillInstanceGeo anterior a Fase 2. Nao refatorar: o valor deste
    // bloco esta em ele ser INDEPENDENTE da implementacao que valida.
    FLegacyGeoMaterial LegacyFill(const MaterialConstants* MC, bool TwoSidedForRT, bool Blend,
                                  bool Finalized, u32 SRVTableStart) {
        FLegacyGeoMaterial g{};
        if (!MC) return g;
        g.BaseColor  = MC->BaseColorFactor;
        g.TwoSidedRT = TwoSidedForRT ? 1u : 0u;
        if (Finalized && MC->HasAlbedoMap != 0) {
            g.AlbedoIndex = SRVTableStart;
            g.HasAlbedo   = 1;
        }
        g.AlphaCutoff     = MC->AlphaCutoff;
        g.RoughnessFactor = MC->RoughnessFactor;
        g.SubsurfaceColor = MC->SubsurfaceColor;
        const f32 EmiRT = MC->EmissiveStrength * MC->RTEmissiveScale;
        g.EmissiveFactor = { MC->EmissiveFactor.X * EmiRT, MC->EmissiveFactor.Y * EmiRT,
                             MC->EmissiveFactor.Z * EmiRT, MC->MetallicFactor };
        if (MC->AlphaTest)          g.Flags |= 1u;
        if (MC->ShadingModel == 1)  g.Flags |= 4u;
        if (Blend)                  g.Flags |= 128u;
        if (Finalized) {
            if (MC->HasEmissiveMap) { g.EmissiveMapIndex = SRVTableStart + 4; g.Flags |= 2u; }
            if (MC->HasMetallicRoughnessMap) {
                g.MrMapIndex = SRVTableStart + 2;
                g.Flags |= 8u;
                if (MC->SpecularPacking) g.Flags |= 16u;
            }
            if (MC->HasMetalnessMap) { g.MetalMapIndex = SRVTableStart + 6; g.Flags |= 32u; }
            if (MC->HasRoughnessMap) { g.RoughMapIndex = SRVTableStart + 7; g.Flags |= 64u; }
        }
        return g;
    }

    bool Same4(const Vec4& A, const Vec4& B) {
        return A.X == B.X && A.Y == B.Y && A.Z == B.Z && A.W == B.W;
    }

    // Um indice de mapa so precisa casar quando o bit que o valida esta ligado; sem ele o
    // snapshot antigo deixava 0 e a linha nova nao guarda campo nenhum.
    void CheckIndex(const FGpuMaterialRecord& R, u32 FlagBit, u32 Offset, u32 Legacy,
                    const std::string& Where) {
        if ((R.Flags & FlagBit) == 0) return;
        Check(R.TextureIndex(Offset) == Legacy, Where + ": indice bindless divergiu");
    }

    void CompareCase(const MaterialConstants& C, bool TwoSided, bool Blend, bool Finalized,
                     u32 Base, const std::string& Where) {
        const FLegacyGeoMaterial L = LegacyFill(&C, TwoSided, Blend, Finalized, Base);
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(
            C, TwoSided, Blend, Finalized ? Base : FGpuMaterialRecord::kInvalidTextureBase);

        Check(Same4(R.BaseColor, L.BaseColor),             Where + ": BaseColor divergiu");
        Check(Same4(R.EmissiveFactor, L.EmissiveFactor),   Where + ": EmissiveFactor divergiu");
        Check(Same4(R.SubsurfaceColor, L.SubsurfaceColor), Where + ": SubsurfaceColor divergiu");
        Check(R.RoughnessFactor == L.RoughnessFactor,      Where + ": RoughnessFactor divergiu");
        Check(R.AlphaCutoff == L.AlphaCutoff,              Where + ": AlphaCutoff divergiu");

        // Os oito bits herdados tem de continuar com o MESMO valor numerico; os dois novos
        // reproduzem os u32 que o snapshot carregava inteiros.
        const u32 Inherited = kMaterialFlagAlphaTest | kMaterialFlagEmissiveMap |
                              kMaterialFlagFoliage | kMaterialFlagMRMap |
                              kMaterialFlagSpecPacked | kMaterialFlagMetalMap |
                              kMaterialFlagRoughMap | kMaterialFlagTranslucent;
        Check((R.Flags & Inherited) == L.Flags, Where + ": bits herdados divergiram");
        Check(((R.Flags & kMaterialFlagTwoSided) != 0u) == (L.TwoSidedRT != 0u),
              Where + ": TwoSidedRT divergiu");
        Check(((R.Flags & kMaterialFlagAlbedoMap) != 0u) == (L.HasAlbedo != 0u),
              Where + ": HasAlbedo divergiu");

        CheckIndex(R, kMaterialFlagAlbedoMap,   kMaterialTexAlbedo,   L.AlbedoIndex,      Where + " albedo");
        CheckIndex(R, kMaterialFlagEmissiveMap, kMaterialTexEmissive, L.EmissiveMapIndex, Where + " emissivo");
        CheckIndex(R, kMaterialFlagMRMap,       kMaterialTexMR,       L.MrMapIndex,       Where + " MR");
        CheckIndex(R, kMaterialFlagMetalMap,    kMaterialTexMetal,    L.MetalMapIndex,    Where + " metal");
        CheckIndex(R, kMaterialFlagRoughMap,    kMaterialTexRough,    L.RoughMapIndex,    Where + " rough");
    }

    // Constantes com valores RECONHECIVEIS por campo: um campo copiado do vizinho aparece como
    // numero errado, e nao como zero que qualquer default tambem produziria.
    MaterialConstants Distinct() {
        MaterialConstants C{};
        C.BaseColorFactor  = { 0.11f, 0.22f, 0.33f, 0.44f };
        C.MetallicFactor   = 0.55f;
        C.RoughnessFactor  = 0.66f;
        C.EmissiveFactor   = { 1.5f, 2.5f, 3.5f, 1.0f };
        C.EmissiveStrength = 2.0f;
        C.RTEmissiveScale  = 0.25f;
        C.AlphaCutoff      = 0.77f;
        C.SubsurfaceColor  = { 0.1f, 0.2f, 0.3f, 0.6f };
        return C;
    }
}

int main() {
    std::cout << "Smile.GpuMaterialRecord\n";

    // --- ABI ---------------------------------------------------------------------------------
    // Os static_assert do header ja travam isto em compilacao; repetir aqui e o que faz um
    // relaxamento acidental deles aparecer como teste vermelho, e nao como header que compila.
    Check(sizeof(FGpuMaterialRecord) == 64, "a linha tem de ter 64 B");
    Check(offsetof(FGpuMaterialRecord, BaseColor) == 0, "BaseColor no offset 0");
    Check(offsetof(FGpuMaterialRecord, EmissiveFactor) == 16, "EmissiveFactor no offset 16");
    Check(offsetof(FGpuMaterialRecord, SubsurfaceColor) == 32, "SubsurfaceColor no offset 32");
    Check(offsetof(FGpuMaterialRecord, RoughnessFactor) == 48, "RoughnessFactor no offset 48");
    Check(offsetof(FGpuMaterialRecord, AlphaCutoff) == 52, "AlphaCutoff no offset 52");
    Check(offsetof(FGpuMaterialRecord, Flags) == 56, "Flags no offset 56");
    Check(offsetof(FGpuMaterialRecord, TextureBase) == 60, "TextureBase no offset 60");

    // Valores dos oito bits herdados. Eles viajam ate o HLSL como INSTGEO_FLAG_*; renumerar
    // qualquer um sem tocar no shader troca o significado de um bit em todo hit de RT.
    Check(kMaterialFlagAlphaTest   == 1u,   "ALPHATEST = 1");
    Check(kMaterialFlagEmissiveMap == 2u,   "EMISSIVE = 2");
    Check(kMaterialFlagFoliage     == 4u,   "FOLIAGE = 4");
    Check(kMaterialFlagMRMap       == 8u,   "MRMAP = 8");
    Check(kMaterialFlagSpecPacked  == 16u,  "SPECPACK = 16");
    Check(kMaterialFlagMetalMap    == 32u,  "METALMAP = 32");
    Check(kMaterialFlagRoughMap    == 64u,  "ROUGHMAP = 64");
    Check(kMaterialFlagTranslucent == 128u, "TRANSLUCENT = 128");

    // --- Linha default ------------------------------------------------------------------------
    // Renderavel sem material tinha defaults PROPRIOS no snapshot (cinza 0.7, subsurface branco);
    // zerar a linha em vez de default-construi-la deixaria a cena escura no indireto.
    {
        const FGpuMaterialRecord D = FGpuMaterialRecord::FromMaterial(nullptr);
        const FLegacyGeoMaterial L = LegacyFill(nullptr, false, false, false, 0);
        Check(Same4(D.BaseColor, L.BaseColor), "material nulo: BaseColor default divergiu");
        Check(Same4(D.SubsurfaceColor, L.SubsurfaceColor), "material nulo: subsurface divergiu");
        Check(Same4(D.EmissiveFactor, L.EmissiveFactor), "material nulo: emissivo divergiu");
        Check(D.RoughnessFactor == L.RoughnessFactor, "material nulo: rough default divergiu");
        Check(D.AlphaCutoff == L.AlphaCutoff, "material nulo: cutoff default divergiu");
        Check(D.Flags == 0u, "material nulo nao tem bit ligado");
        Check(D.TextureBase == FGpuMaterialRecord::kInvalidTextureBase,
              "material nulo nao aponta para descritor");
        Check(D == FGpuMaterialRecord{}, "linha default-construida == linha do material nulo");
    }

    // --- Matriz de paridade -------------------------------------------------------------------
    // 8 flags de mapa/modo x finalizado/nao. Exaustivo e barato: 512 casos, cada um comparando
    // treze campos.
    {
        constexpr u32 kAlbedo = 1u, kEmissive = 2u, kMR = 4u, kMetal = 8u, kRough = 16u,
                      kSpec = 32u, kAlphaTest = 64u, kFoliage = 128u;
        u32 Cases = 0;
        for (u32 Mask = 0; Mask < 256u; ++Mask) {
            for (u32 Fin = 0; Fin < 2u; ++Fin) {
                MaterialConstants C = Distinct();
                C.HasAlbedoMap            = (Mask & kAlbedo)   ? 1u : 0u;
                C.HasEmissiveMap          = (Mask & kEmissive) ? 1u : 0u;
                C.HasMetallicRoughnessMap = (Mask & kMR)       ? 1u : 0u;
                C.HasMetalnessMap         = (Mask & kMetal)    ? 1u : 0u;
                C.HasRoughnessMap         = (Mask & kRough)    ? 1u : 0u;
                C.SpecularPacking         = (Mask & kSpec)     ? 1u : 0u;
                C.AlphaTest               = (Mask & kAlphaTest)? 1u : 0u;
                C.ShadingModel            = (Mask & kFoliage)  ? 1u : 0u;
                // TwoSided/Blend nao vem das Constants; varie-os junto com a mascara para nao
                // deixar um dos dois preso num valor.
                const bool TwoSided = (Mask & kAlphaTest) != 0 || (Mask & 1u) != 0;
                const bool Blend    = (Mask & kFoliage) == 0 && (Mask & 2u) != 0;
                CompareCase(C, TwoSided, Blend, Fin != 0, 4096u,
                            "mask=" + std::to_string(Mask) + " fin=" + std::to_string(Fin));
                ++Cases;
            }
        }
        std::cout << "  paridade snapshot antigo x linha canonica: " << Cases << " casos\n";
    }

    // --- Nao finalizado nao aponta para descritor ---------------------------------------------
    // O caso que o snapshot antigo tratava por acidente: com todos os mapas marcados mas sem
    // tabela, ele gravava indice 0 e bit desligado. A linha tem de recusar a base, e nao herdar
    // uma base de outro material.
    {
        MaterialConstants C = Distinct();
        C.HasAlbedoMap = C.HasEmissiveMap = C.HasMetallicRoughnessMap = 1u;
        C.HasMetalnessMap = C.HasRoughnessMap = C.SpecularPacking = 1u;
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(
            C, true, false, FGpuMaterialRecord::kInvalidTextureBase);
        const u32 MapBits = kMaterialFlagAlbedoMap | kMaterialFlagEmissiveMap |
                            kMaterialFlagMRMap | kMaterialFlagSpecPacked |
                            kMaterialFlagMetalMap | kMaterialFlagRoughMap;
        Check((R.Flags & MapBits) == 0u, "sem tabela de SRV nenhum bit de mapa pode ligar");
        Check(R.TextureBase == FGpuMaterialRecord::kInvalidTextureBase,
              "sem tabela de SRV a base fica invalida");
        Check(R.TextureIndex(kMaterialTexEmissive) == FGpuMaterialRecord::kInvalidTextureBase,
              "deslocar uma base invalida nao pode produzir descritor plausivel");
        // O que continua valendo mesmo sem tabela: cor, cutoff, two-sided e modo.
        Check((R.Flags & kMaterialFlagTwoSided) != 0u, "two-sided independe de finalizacao");
        Check((R.Flags & kMaterialFlagAlphaTest) == 0u, "alpha test segue as Constants");
    }

    // --- SpecularPacking so vale sob o mapa MR ------------------------------------------------
    // Ligado sozinho, ele fazia o hit ler metal do canal B de um mapa que nao existe.
    {
        MaterialConstants C = Distinct();
        C.SpecularPacking = 1u;
        C.HasMetallicRoughnessMap = 0u;
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(C, false, false, 1024u);
        Check((R.Flags & kMaterialFlagSpecPacked) == 0u,
              "SpecularPacking sem mapa MR nao pode ligar");
    }

    // --- RTEmissiveScale multiplica o emissivo do RT ------------------------------------------
    // Campo com um unico consumidor: se ele parar de entrar aqui, o teste de paridade acima ainda
    // passaria caso o oraculo mudasse junto. Este caso fixa o NUMERO.
    {
        MaterialConstants C{};
        C.EmissiveFactor   = { 4.0f, 0.0f, 0.0f, 1.0f };
        C.EmissiveStrength = 3.0f;
        C.RTEmissiveScale  = 0.5f;
        C.MetallicFactor   = 0.25f;
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(C, false, false, 0u);
        Check(R.EmissiveFactor.X == 6.0f, "emissivo do RT = fator * forca * escala");
        Check(R.EmissiveFactor.W == 0.25f, "MetallicFactor viaja no .w do emissivo");
    }

    if (Failures == 0) std::cout << "  OK\n";
    else std::cout << "  " << Failures << " falha(s)\n";
    return Failures == 0 ? 0 : 1;
}
