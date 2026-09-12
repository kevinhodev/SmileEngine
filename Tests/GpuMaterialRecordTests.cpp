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

    // === Oraculo do BLOCO DE RASTER (Fase 3) ==================================================
    //
    // Mesma ideia do LegacyFill acima, para o outro consumidor: o que o pixel shader do raster
    // LIA do MaterialCB, transcrito a mao. Nao chama a engine — se a derivacao mudar junto com a
    // implementacao, a comparacao visual da fase acusaria uma diferenca sem dizer se ela veio do
    // acesso bindless ou de um campo calculado diferente.
    //
    // Os valores abaixo sao os EFETIVOS: `Emissive` e o produto que o GBuffer.ps computa na linha
    // 190 (`EmissiveFactor.rgb * EmissiveStrength`), e nao o par de campos crus. E assim que a
    // troca "o shader computa" -> "a linha ja traz computado" fica coberta.
    struct FLegacyMaterialCB {
        f32 EmissiveR = 0.0f, EmissiveG = 0.0f, EmissiveB = 0.0f;
        f32 AOStrength = 1.0f;
        f32 NormalStrength = 1.0f;
        f32 HeightScale = 0.05f;
        f32 ParallaxMinSteps = 8.0f;
        f32 ParallaxMaxSteps = 32.0f;
        f32 ParallaxFadeStart = 3.0f;
        f32 ParallaxFadeRange = 5.0f;
        u32 ParallaxRefineSteps = 5u;
        u32 ShadingModel = 0u;
        u32 HasNormalMap = 0u, HasAOMap = 0u, HasHeightMap = 0u;
        u32 NormalFlipY = 0u, NormalReconstructZ = 0u, ParallaxRefine = 0u;
    };

    // `Finalized` entra porque os TRES bits de mapa dependem da tabela de descritores — sem ela
    // nao ha o que amostrar. Os fatores e passos nao dependem: um material sem textura ainda tem
    // forca de normal e escala de altura.
    FLegacyMaterialCB LegacyCB(const MaterialConstants& C, bool Finalized) {
        FLegacyMaterialCB O{};
        O.EmissiveR = C.EmissiveFactor.X * C.EmissiveStrength;
        O.EmissiveG = C.EmissiveFactor.Y * C.EmissiveStrength;
        O.EmissiveB = C.EmissiveFactor.Z * C.EmissiveStrength;
        O.AOStrength          = C.AOStrength;
        O.NormalStrength      = C.NormalStrength;
        O.HeightScale         = C.HeightScale;
        O.ParallaxMinSteps    = C.ParallaxMinSteps;
        O.ParallaxMaxSteps    = C.ParallaxMaxSteps;
        O.ParallaxFadeStart   = C.ParallaxFadeStart;
        O.ParallaxFadeRange   = C.ParallaxFadeRange;
        O.ParallaxRefineSteps = C.ParallaxRefineSteps;
        O.ShadingModel        = C.ShadingModel;
        O.NormalFlipY         = C.NormalFlipY;
        O.NormalReconstructZ  = C.NormalReconstructZ;
        O.ParallaxRefine      = C.ParallaxRefine;
        if (Finalized) {
            O.HasNormalMap = C.HasNormalMap;
            O.HasAOMap     = C.HasAOMap;
            O.HasHeightMap = C.HasHeightMap;
        }
        return O;
    }

    void CompareRasterCase(const MaterialConstants& C, bool Finalized, u32 Base,
                           const std::string& Where) {
        const FLegacyMaterialCB O = LegacyCB(C, Finalized);
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(
            C, false, false, Finalized ? Base : FGpuMaterialRecord::kInvalidTextureBase);

        Check(R.RasterEmissive.X == O.EmissiveR && R.RasterEmissive.Y == O.EmissiveG &&
                  R.RasterEmissive.Z == O.EmissiveB,
              Where + ": emissivo do raster divergiu (leva EmissiveStrength, nao RTEmissiveScale)");
        Check(R.RasterEmissive.W == O.AOStrength,     Where + ": AOStrength divergiu");
        Check(R.NormalStrength == O.NormalStrength,   Where + ": NormalStrength divergiu");
        Check(R.HeightScale == O.HeightScale,         Where + ": HeightScale divergiu");
        Check(R.ParallaxMinSteps == O.ParallaxMinSteps, Where + ": ParallaxMinSteps divergiu");
        Check(R.ParallaxMaxSteps == O.ParallaxMaxSteps, Where + ": ParallaxMaxSteps divergiu");
        Check(R.ParallaxFadeStart == O.ParallaxFadeStart, Where + ": ParallaxFadeStart divergiu");
        Check(R.ParallaxFadeRange == O.ParallaxFadeRange, Where + ": ParallaxFadeRange divergiu");
        Check(R.ParallaxRefineSteps == O.ParallaxRefineSteps,
              Where + ": ParallaxRefineSteps divergiu");
        Check(R.ShadingModel == O.ShadingModel, Where + ": ShadingModel divergiu");

        Check(((R.Flags & kMaterialFlagNormalMap) != 0u) == (O.HasNormalMap != 0u),
              Where + ": HasNormalMap divergiu");
        Check(((R.Flags & kMaterialFlagAOMap) != 0u) == (O.HasAOMap != 0u),
              Where + ": HasAOMap divergiu");
        Check(((R.Flags & kMaterialFlagHeightMap) != 0u) == (O.HasHeightMap != 0u),
              Where + ": HasHeightMap divergiu");
        Check(((R.Flags & kMaterialFlagNormalFlipY) != 0u) == (O.NormalFlipY != 0u),
              Where + ": NormalFlipY divergiu");
        Check(((R.Flags & kMaterialFlagNormalReconZ) != 0u) == (O.NormalReconstructZ != 0u),
              Where + ": NormalReconstructZ divergiu");
        Check(((R.Flags & kMaterialFlagParallaxRefine) != 0u) == (O.ParallaxRefine != 0u),
              Where + ": ParallaxRefine divergiu");
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
        // Bloco de raster: nenhum campo com o proprio default, e todos diferentes entre si.
        C.AOStrength          = 0.81f;
        C.NormalStrength      = 0.82f;
        C.HeightScale         = 0.83f;
        C.ParallaxMinSteps    = 6.0f;
        C.ParallaxMaxSteps    = 24.0f;
        C.ParallaxFadeStart   = 2.5f;
        C.ParallaxFadeRange   = 4.5f;
        C.ParallaxRefineSteps = 7u;
        return C;
    }
}

int main() {
    std::cout << "Smile.GpuMaterialRecord\n";

    // --- ABI ---------------------------------------------------------------------------------
    // Os static_assert do header ja travam isto em compilacao; repetir aqui e o que faz um
    // relaxamento acidental deles aparecer como teste vermelho, e nao como header que compila.
    Check(sizeof(FGpuMaterialRecord) == 116, "a linha tem de ter 116 B");
    Check(offsetof(FGpuMaterialRecord, BaseColor) == 0, "BaseColor no offset 0");
    Check(offsetof(FGpuMaterialRecord, EmissiveFactor) == 16, "EmissiveFactor no offset 16");
    Check(offsetof(FGpuMaterialRecord, SubsurfaceColor) == 32, "SubsurfaceColor no offset 32");
    Check(offsetof(FGpuMaterialRecord, RoughnessFactor) == 48, "RoughnessFactor no offset 48");
    Check(offsetof(FGpuMaterialRecord, AlphaCutoff) == 52, "AlphaCutoff no offset 52");
    Check(offsetof(FGpuMaterialRecord, Flags) == 56, "Flags no offset 56");
    Check(offsetof(FGpuMaterialRecord, TextureBase) == 60, "TextureBase no offset 60");
    // Bloco de raster (Fase 3). Os OITO offsets acima nao podem se mexer: eles sao o que todo
    // shader de hit de RT le, e a fase que acrescentou o bloco abaixo nao mudou uma letra deles.
    Check(offsetof(FGpuMaterialRecord, RasterEmissive) == 64, "RasterEmissive no offset 64");
    Check(offsetof(FGpuMaterialRecord, NormalStrength) == 80, "NormalStrength no offset 80");
    Check(offsetof(FGpuMaterialRecord, HeightScale) == 84, "HeightScale no offset 84");
    Check(offsetof(FGpuMaterialRecord, ParallaxMinSteps) == 88, "ParallaxMinSteps no offset 88");
    Check(offsetof(FGpuMaterialRecord, ParallaxMaxSteps) == 92, "ParallaxMaxSteps no offset 92");
    Check(offsetof(FGpuMaterialRecord, ParallaxFadeStart) == 96, "ParallaxFadeStart no offset 96");
    Check(offsetof(FGpuMaterialRecord, ParallaxFadeRange) == 100, "ParallaxFadeRange no offset 100");
    Check(offsetof(FGpuMaterialRecord, ParallaxRefineSteps) == 104, "ParallaxRefineSteps no offset 104");
    Check(offsetof(FGpuMaterialRecord, ShadingModel) == 108, "ShadingModel no offset 108");

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
    Check(kMaterialFlagAlbedoMap   == 256u, "ALBEDOMAP = 256");
    Check(kMaterialFlagTwoSided    == 512u, "TWOSIDED = 512");

    // Os seis bits do bloco de raster. Renumerar um deles mantem o layout certo e troca, por
    // exemplo, "tem normal map" por "reconstruir Z" — o pixel shader continuaria compilando.
    Check(kMaterialFlagNormalMap      == 1024u,  "NORMALMAP = 1024");
    Check(kMaterialFlagAOMap          == 2048u,  "AOMAP = 2048");
    Check(kMaterialFlagHeightMap      == 4096u,  "HEIGHTMAP = 4096");
    Check(kMaterialFlagNormalFlipY    == 8192u,  "NORMALFLIPY = 8192");
    Check(kMaterialFlagNormalReconZ   == 16384u, "NORMALRECONZ = 16384");
    Check(kMaterialFlagParallaxRefine == 32768u, "PARALLAXREFINE = 32768");

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

    // --- Matriz do bloco de raster ------------------------------------------------------------
    // Os seis bits novos x finalizado/nao, exaustivo. O oraculo aqui e o MaterialCB: o que o pixel
    // shader lia por um CBV amarrado POR DRAW tem de sair identico da linha canonica.
    //
    // O ShadingModel varre os QUATRO valores e nao so folhagem: o bit kMaterialFlagFoliage
    // responde "e folhagem?", que nao reconstroi o modelo Subsurface. Se o campo sumisse e o
    // shader voltasse a derivar do bit, materiais Subsurface iriam para o G-buffer como
    // DefaultLit.
    {
        constexpr u32 kNormal = 1u, kAO = 2u, kHeight = 4u, kFlipY = 8u, kReconZ = 16u,
                      kRefine = 32u;
        u32 Cases = 0;
        for (u32 Mask = 0; Mask < 64u; ++Mask) {
            for (u32 Fin = 0; Fin < 2u; ++Fin) {
                MaterialConstants C = Distinct();
                C.HasNormalMap       = (Mask & kNormal) ? 1u : 0u;
                C.HasAOMap           = (Mask & kAO)     ? 1u : 0u;
                C.HasHeightMap       = (Mask & kHeight) ? 1u : 0u;
                C.NormalFlipY        = (Mask & kFlipY)  ? 1u : 0u;
                C.NormalReconstructZ = (Mask & kReconZ) ? 1u : 0u;
                C.ParallaxRefine     = (Mask & kRefine) ? 1u : 0u;
                C.ShadingModel       = Mask % 4u;
                CompareRasterCase(C, Fin != 0, 4096u,
                                  "raster mask=" + std::to_string(Mask) + " fin=" +
                                      std::to_string(Fin));
                ++Cases;
            }
        }
        std::cout << "  paridade MaterialCB x bloco de raster: " << Cases << " casos\n";
    }

    // --- O emissivo do raster NAO leva o RTEmissiveScale ---------------------------------------
    // Os dois emissivos da linha existem por causa desta divergencia. Se um dia colapsarem num
    // campo so, este caso e o que fica vermelho — e nao a comparacao visual, meses depois, num
    // material decorativo que passou a lavar a cena ou a apagar na tela.
    {
        MaterialConstants C{};
        C.EmissiveFactor   = { 4.0f, 0.0f, 0.0f, 1.0f };
        C.EmissiveStrength = 3.0f;
        C.RTEmissiveScale  = 0.5f;
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(C, false, false, 0u);
        Check(R.RasterEmissive.X == 12.0f, "emissivo do raster = fator x forca (4 x 3)");
        Check(R.EmissiveFactor.X == 6.0f,  "emissivo do RT = fator x forca x escala (4 x 3 x 0,5)");
    }

    // --- AOStrength viaja no .w do emissivo do raster ------------------------------------------
    // Empacotamento deliberado (evita um campo solto de 4 B), e por isso mesmo o tipo de coisa que
    // uma leitura desatenta troca pelo alpha do emissivo.
    {
        MaterialConstants C{};
        C.AOStrength = 0.375f;
        const FGpuMaterialRecord R = FGpuMaterialRecord::FromParts(C, false, false, 0u);
        Check(R.RasterEmissive.W == 0.375f, "AOStrength no .w do RasterEmissive");
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

    // --- Classe de PIPELINE: as oito combinacoes de Blend/AlphaTest/TwoSided ------------------
    //
    // Este e o eixo pelo qual a Fase 5 agrupa comandos, e ele nasce de tres propriedades que NAO
    // sao independentes: o `IsTwoSidedForRT()` do FMaterial ja vale
    // `TwoSided || AlphaTest || Blend`. Por isso a matriz varre as tres FONTES e passa por um
    // FMaterial de verdade — usar o predicado da engine e o que impede o teste de concordar consigo
    // mesmo.
    //
    // A expectativa e uma TABELA literal, e nao a precedencia recalculada: reescrever a precedencia
    // no oraculo faria o teste passar junto com o bug.
    {
        // Bits da mascara: 1 = Blend, 2 = AlphaTest, 4 = TwoSided.
        constexpr u32 kEsperado[8] = {
            GpuPipelineClassOpaqueOneSided, // - - -
            GpuPipelineClassBlend,          // B - -
            GpuPipelineClassMasked,         // - A -
            GpuPipelineClassBlend,          // B A -   Blend tem precedencia sobre AlphaTest
            GpuPipelineClassOpaqueTwoSided, // - - T
            GpuPipelineClassBlend,          // B - T
            GpuPipelineClassMasked,         // - A T   AlphaTest tem precedencia sobre TwoSided
            GpuPipelineClassBlend,          // B A T
        };
        u32 Elegiveis = 0;
        for (u32 Mask = 0; Mask < 8u; ++Mask) {
            const std::string Onde = "classe mask=" + std::to_string(Mask);
            FMaterial M;
            M.Blend               = (Mask & 1u) != 0;
            M.Constants.AlphaTest = (Mask & 2u) ? 1u : 0u;
            M.TwoSided            = (Mask & 4u) != 0;

            const FGpuMaterialRecord R = FGpuMaterialRecord::FromMaterial(&M);
            Check(R.PipelineClass == kEsperado[Mask], Onde + ": classe pela precedencia declarada");
            // A classe e funcao dos Flags que a GPU LE, e nao das fontes. Se as duas discordarem, o
            // lote sai com a PSO de uma classe e o shader lendo os bits de outra.
            Check(R.PipelineClass == GpuPipelineClassFromFlags(R.Flags),
                  Onde + ": a classe da linha == a classe derivada dos Flags da linha");

            // Elegibilidade == a familia one-sided de PSO que o G-buffer JA escolhe hoje. O loop do
            // G-buffer (RendererPasses.cpp) exclui Blend e depois pergunta IsTwoSidedForRT: a
            // exclusao e transcrita aqui, o predicado e o da engine.
            const bool LegacyNoGBuffer   = !M.Blend;
            const bool LegacyOneSidedPSO = LegacyNoGBuffer && !M.IsTwoSidedForRT();
            Check(GpuPipelineClassIndirectEligible(R.PipelineClass) == LegacyOneSidedPSO,
                  Onde + ": elegivel ao indireto == familia one-sided do G-buffer de hoje");
            if (LegacyOneSidedPSO) ++Elegiveis;

            // A linha FINALIZADA tem de sair com a mesma classe. Nenhum bit de mapa participa dela,
            // mas a derivacao mora depois do bloco de descritores: com um early return ali, todo
            // material nao finalizado sairia com a classe default (zero) em silencio.
            MaterialConstants CF = M.Constants;
            CF.HasAlbedoMap = CF.HasNormalMap = CF.HasMetallicRoughnessMap = 1u;
            const FGpuMaterialRecord F =
                FGpuMaterialRecord::FromParts(CF, M.IsTwoSidedForRT(), M.Blend, 2048u);
            Check(F.PipelineClass == R.PipelineClass,
                  Onde + ": finalizado e nao finalizado dao a mesma classe");
        }
        // Uma das oito, e exatamente a que nao tem nenhuma das tres propriedades.
        Check(Elegiveis == 1u, "das oito combinacoes, uma unica entra no indireto da Fase 5");
        std::cout << "  precedencia Blend > AlphaTest > TwoSided > OpaqueOneSided: 8 casos\n";
    }

    // --- A classe entra na comparacao que decide o upload -------------------------------------
    // Mesma regra do resto da linha: campo que a GPU le e campo que participa da igualdade. Uma
    // linha com a classe errada nao pode passar por "igual" a correta.
    {
        FGpuMaterialRecord A{};
        FGpuMaterialRecord B = A;
        B.PipelineClass = GpuPipelineClassMasked;
        Check(A != B, "duas linhas que diferem SO na classe nao sao iguais");
        Check(A.Flags == B.Flags, "e elas tem os mesmos Flags: a classe sozinha decidiu");
    }

    // --- A linha default e a classe do material default ---------------------------------------
    // Flags zero tem de cair em OpaqueOneSided, que e a classe de valor 0 justamente para que uma
    // linha zerada por acidente nao entre num lote que nao e o dela.
    {
        Check(FGpuMaterialRecord{}.PipelineClass == GpuPipelineClassOpaqueOneSided,
              "a linha default nasce opaca one-sided");
        Check(GpuPipelineClassFromFlags(0u) == GpuPipelineClassOpaqueOneSided,
              "Flags == 0 classifica como opaco one-sided");
    }

    if (Failures == 0) std::cout << "  OK\n";
    else std::cout << "  " << Failures << " falha(s)\n";
    return Failures == 0 ? 0 : 1;
}
