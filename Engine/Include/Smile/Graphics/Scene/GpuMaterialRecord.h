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
// (Tests/GpuSceneTablesWarpTests.cpp) no MESMO commit.

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

    // Bits do BLOCO DE RASTER (Fase 3). Os dez acima descrevem o que o ray tracing le; estes seis
    // sao os `uint` do MaterialCB que so o raster consulta e que, ate aqui, chegavam ao pixel
    // shader por um CBV amarrado POR DRAW. Continuam sendo um bit cada porque nunca carregaram
    // mais que isso.
    inline constexpr u32 kMaterialFlagNormalMap     = 1024u;  // HasNormalMap
    inline constexpr u32 kMaterialFlagAOMap         = 2048u;  // HasAOMap
    inline constexpr u32 kMaterialFlagHeightMap     = 4096u;  // HasHeightMap (parallax)
    inline constexpr u32 kMaterialFlagNormalFlipY   = 8192u;  // normal map DirectX (Y invertido)
    inline constexpr u32 kMaterialFlagNormalReconZ  = 16384u; // normal BC5 (so RG): reconstruir Z
    inline constexpr u32 kMaterialFlagParallaxRefine = 32768u; // busca binaria no hit do POM

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

    // === Classe de PIPELINE do material (Fase 5) ================================================
    //
    // O eixo pelo qual o caminho indireto agrupa comandos. Propriedade ESTAVEL do material: nao
    // depende do passe, da camera nem do frame. O eixo do PASSE (depth-only, depth-normal,
    // G-buffer) e separado e NAO entra nesta linha — cruzar os dois aqui daria o produto cartesiano
    // das duas familias numa tabela que toda edicao de material reescreve.
    //
    // Deriva dos `Flags` JA MONTADOS da linha, e nao das tres propriedades do FMaterial. A razao e
    // concreta: os bits sao o que a GPU le, e uma classe que reconsultasse as fontes poderia
    // discordar deles — o lote sairia com a PSO de uma classe e o shader lendo os bits de outra.
    //
    // A precedencia e TOTAL e explicita, para que combinacao estranha (Blend junto com AlphaTest,
    // por exemplo) continue deterministica em vez de depender da ordem em que alguem escreveu os
    // ifs:
    //
    //     Blend > AlphaTest > TwoSided > OpaqueOneSided
    //
    // Ela tambem desarma uma armadilha real: `kMaterialFlagTwoSided` NAO e o `TwoSided` cru do
    // material — e o `FMaterial::IsTwoSidedForRT()`, que ja vale `TwoSided || AlphaTest || Blend`.
    // Como Blend e AlphaTest sao testados ANTES, quando a terceira linha e alcancada o bit fundido
    // vale exatamente o `TwoSided` cru. Trocar duas dessas linhas de lugar compila e passa a
    // classificar toda folhagem como opaco two-sided.
    //
    // Os VALORES sao arbitrarios, mas congelados e espelhados no HLSL: eles indexam os contadores
    // por classe e os lotes do classify. `OpaqueOneSided` e 0 porque e a classe de `Flags == 0` —
    // uma linha default-construida tem de sair com a classe do material DEFAULT, e nao com uma
    // classe qualquer que o zero acabou apontando.
    enum EGpuPipelineClass : u32 {
        GpuPipelineClassOpaqueOneSided = 0u,
        GpuPipelineClassOpaqueTwoSided = 1u,
        GpuPipelineClassMasked         = 2u,
        GpuPipelineClassBlend          = 3u,
    };
    inline constexpr u32 kGpuPipelineClassCount = 4u;

    inline u32 GpuPipelineClassFromFlags(u32 _Flags) {
        if (_Flags & kMaterialFlagTranslucent) return GpuPipelineClassBlend;
        if (_Flags & kMaterialFlagAlphaTest)   return GpuPipelineClassMasked;
        if (_Flags & kMaterialFlagTwoSided)    return GpuPipelineClassOpaqueTwoSided;
        return GpuPipelineClassOpaqueOneSided;
    }

    // Esta classe entra no `ExecuteIndirect` da Fase 5?
    //
    // So `OpaqueOneSided`, e isso NAO e um agrupamento novo: `OpaqueOneSided` e exatamente o
    // conjunto que o G-buffer de hoje manda para a familia one-sided de PSO. O loop do G-buffer
    // exclui `Blend` e depois consulta `IsTwoSidedForRT()` (RendererPasses.cpp), entao a familia
    // one-sided e `!Blend && !TwoSided && !AlphaTest` — os tres bits que as tres primeiras linhas da
    // precedencia derrubam. A classificacao aqui e mais EXPLICITA que a ordenacao atual, nao
    // diferente dela.
    //
    // As outras tres continuam no fallback CPU: masked e two-sided ate a Fase 7, Blend nunca —
    // transparencia segue ordenada na CPU.
    //
    // Predicado UNICO para que o classify e o contador de fallback nao possam discordar sobre quem
    // ficou de fora. Regra 6 do plano: o que nao entra tem de ser desenhado por outro caminho, e
    // contado.
    inline bool GpuPipelineClassIndirectEligible(u32 _PipelineClass) {
        return _PipelineClass == GpuPipelineClassOpaqueOneSided;
    }

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

        // ---- Bloco de RASTER (Fase 3) -------------------------------------------------------
        //
        // Tudo daqui para baixo existe para que o pixel shader do raster pare de precisar do CBV
        // de material amarrado POR DRAW (`FMaterial::Bind`, root param 1). Sao exatamente os
        // campos do MaterialCB que algum shader de raster le e que os 64 B de cima nao carregam.
        //
        // Fica na MESMA linha, e nao numa segunda tabela, por tres razoes: a decisao D6 do plano
        // diz que constantes de material moram no FGpuMaterialRecord; uma segunda tabela
        // duplicaria dirty rows, `RowOf` e diff — duas chances de "o material mudou" ficar
        // errado; e os 64 B de cima continuam nos MESMOS offsets, entao a leitura do ray tracing
        // nao muda um byte.
        //
        // Ficam DE FORA `ParallaxSelfShadow` e `ParallaxShadowSteps`: os dois existem no
        // MaterialConstants e no espelho do CBV, e nenhum shader os le (verificado por varredura
        // dos oito shaders que incluem MaterialCB.hlsli). Copia-los para ca seria serializar
        // consumidor imaginario.
        //
        // ⚠️ A linha passou de 64 para 112 B — e para 116 com a classe de pipeline da Fase 5, logo
        // abaixo. Com isso perde a garantia, declarada quando ela tinha 64 B, de que uma linha
        // inteira cabe numa cache line de 64 B. Nunca foi medida, nem antes nem agora; a tabela
        // inteira continua cabendo em dezenas de KB (214 materiais na Emerald = 24 KB). Se algum dia
        // isso aparecer num perfil, a saida e alinhar o stride a 128 e pagar 12 B mortos por
        // material — nao dividir a linha em duas tabelas.

        // rgb = EmissiveFactor * EmissiveStrength, o produto que o G-buffer escreve. Separado do
        // EmissiveFactor de cima porque aquele leva tambem o RTEmissiveScale, que existe
        // justamente para o mundo indireto divergir do raster. Derivar um do outro exigiria
        // dividir pela escala — que pode ser zero.
        // w = AOStrength.
        Vec4 RasterEmissive   = { 0.0f, 0.0f, 0.0f, 1.0f };

        f32  NormalStrength      = 1.0f;
        f32  HeightScale         = 0.05f;
        f32  ParallaxMinSteps    = 8.0f;
        f32  ParallaxMaxSteps    = 32.0f;
        f32  ParallaxFadeStart   = 3.0f;
        f32  ParallaxFadeRange   = 5.0f;
        u32  ParallaxRefineSteps = 5;
        // VALOR, nao bit: sao quatro modelos (SMILE_SHADINGMODEL_*), e o G-buffer o escreve nos
        // dois bits de alpha do RGB10A2. O kMaterialFlagFoliage de cima continua existindo porque
        // e o que o ray tracing pergunta, e responde outra coisa ("este material e folhagem?").
        u32  ShadingModel        = 0;

        // ---- Classe de PIPELINE (Fase 5) ----------------------------------------------------
        //
        // DERIVADA, nao autorada: vale sempre `GpuPipelineClassFromFlags(Flags)`, estampado num
        // lugar so (o fim do `FromParts`). Guardada na linha, e nao derivada pelo consumidor, por
        // duas razoes: e assim que a secao 5.4 do plano descreve a tabela de materiais, e o eixo de
        // agrupamento tem de poder deixar de ser funcao dos bits — uma permutacao de PSO, por
        // exemplo — sem obrigar todo shader que classifica a mudar junto.
        //
        // Sendo exato sobre o que NAO e razao: nao e localidade. O `Flags` mora no offset 56, dentro
        // dos 64 B que o RT ja le; a classe mora no 112. Ler a classe pronta custa o mesmo dword que
        // derivar dos bits custaria, e numa linha de cache pior.
        //
        // ⚠️ A linha passa de 112 para 116 B e o stride deixa de ser multiplo de 16. Os offsets de 0
        // a 108 nao se movem, entao nenhum shader de hit muda uma letra, e StructuredBuffer nao
        // exige stride alinhado (ja dito acima). O que se perde e o alinhamento no INICIO de cada
        // linha: a linha N comeca em 116*N, e os 64 B que o ray tracing le passam a cruzar cache
        // line em parte das linhas. Padding para 128 devolveria as duas coisas por 12 B mortos por
        // material (2,5 KB na Emerald) — nao foi feito porque seria otimizacao sem perfil, e a nota
        // de cima ja registra 128 como a saida no dia em que um perfil pedir.
        u32  PipelineClass       = GpuPipelineClassOpaqueOneSided;

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
                   A.Flags == B.Flags && A.TextureBase == B.TextureBase &&
                   // O bloco de raster entra na comparacao pelo mesmo motivo que o resto: e ela
                   // que decide quais linhas sobem. Um campo esquecido aqui nao quebra nada de
                   // imediato — deixa a GPU com o valor VELHO ate outra edicao qualquer do mesmo
                   // material forcar a copia por acidente.
                   Same4(A.RasterEmissive, B.RasterEmissive) &&
                   A.NormalStrength == B.NormalStrength && A.HeightScale == B.HeightScale &&
                   A.ParallaxMinSteps == B.ParallaxMinSteps &&
                   A.ParallaxMaxSteps == B.ParallaxMaxSteps &&
                   A.ParallaxFadeStart == B.ParallaxFadeStart &&
                   A.ParallaxFadeRange == B.ParallaxFadeRange &&
                   A.ParallaxRefineSteps == B.ParallaxRefineSteps &&
                   A.ShadingModel == B.ShadingModel &&
                   // A classe entra mesmo sendo funcao de `Flags`, que ja esta comparado acima — e
                   // nao e redundancia inutil. Esta comparacao e o que decide quais linhas SOBEM, e
                   // ela recebe dois registros QUAISQUER, nao apenas dois saidos do FromParts: uma
                   // linha montada a mao com a classe errada continuaria "igual" a correta e a GPU
                   // ficaria com a classe velha. E exatamente a divergencia classe/flags que a
                   // derivacao unica existe para impedir; deixar o campo de fora daqui seria abrir
                   // uma segunda porta para ela.
                   A.PipelineClass == B.PipelineClass;
        }
        friend bool operator!=(const FGpuMaterialRecord& A, const FGpuMaterialRecord& B) {
            return !(A == B);
        }
    };

    // 116 B: quatro Vec4 alinhados em 4 (a Smile nao usa alignas em Vec4) mais treze escalares de
    // 4 B. StructuredBuffer nao exige stride multiplo de 16 — e, desde a classe de pipeline, este
    // nao e mais um: ver a nota do campo `PipelineClass`.
    //
    // Os OFFSETS DE 0 A 60 sao os mesmos de quando a linha tinha 64 B, e continuar assim e o que
    // torna esta mudanca invisivel para o ray tracing: nenhum shader de hit precisou mudar uma
    // letra. As asseracoes abaixo existem para que reordenar um campo antigo para "arrumar" o
    // layout falhe na compilacao em vez de trocar o significado de um byte em todo hit.
    static_assert(sizeof(FGpuMaterialRecord) == 116,
                  "FGpuMaterialRecord e layout de GPU: 116 B, espelhado no HLSL");
    static_assert(alignof(FGpuMaterialRecord) == 4, "sem alinhamento implicito surpresa");
    static_assert(offsetof(FGpuMaterialRecord, BaseColor)       ==  0);
    static_assert(offsetof(FGpuMaterialRecord, EmissiveFactor)  == 16);
    static_assert(offsetof(FGpuMaterialRecord, SubsurfaceColor) == 32);
    static_assert(offsetof(FGpuMaterialRecord, RoughnessFactor) == 48);
    static_assert(offsetof(FGpuMaterialRecord, AlphaCutoff)     == 52);
    static_assert(offsetof(FGpuMaterialRecord, Flags)           == 56);
    static_assert(offsetof(FGpuMaterialRecord, TextureBase)     == 60);
    // Bloco de raster: comeca exatamente onde o bloco lido pelo RT termina.
    static_assert(offsetof(FGpuMaterialRecord, RasterEmissive)      ==  64);
    static_assert(offsetof(FGpuMaterialRecord, NormalStrength)      ==  80);
    static_assert(offsetof(FGpuMaterialRecord, HeightScale)         ==  84);
    static_assert(offsetof(FGpuMaterialRecord, ParallaxMinSteps)    ==  88);
    static_assert(offsetof(FGpuMaterialRecord, ParallaxMaxSteps)    ==  92);
    static_assert(offsetof(FGpuMaterialRecord, ParallaxFadeStart)   ==  96);
    static_assert(offsetof(FGpuMaterialRecord, ParallaxFadeRange)   == 100);
    static_assert(offsetof(FGpuMaterialRecord, ParallaxRefineSteps) == 104);
    static_assert(offsetof(FGpuMaterialRecord, ShadingModel)        == 108);
    // A classe de pipeline (Fase 5) entra no FIM, depois do bloco de raster, pela mesma razao que
    // aquele bloco entrou depois dos 64 B do RT: nada acima dela se move.
    static_assert(offsetof(FGpuMaterialRecord, PipelineClass)       == 112);
}
