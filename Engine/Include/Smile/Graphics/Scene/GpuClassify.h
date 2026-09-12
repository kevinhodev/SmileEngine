#pragma once

// Classificacao dos candidatos do main view (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Entrada: a lista de slots que o `ExpandInstanceRanges` produziu. Saida: quantos comandos cada
// LOTE vai ter, mais a contabilidade de todo candidato que nao virou comando.
//
// Esta fatia so CONTA. O prefixo, a compactacao e a escrita dos argumentos vem depois; nada aqui
// tem consumidor ainda.
//
// ==============================================================================================
// QUEM DESENHA O QUE, que e a decisao que este arquivo carrega
// ==============================================================================================
//
// A regra 7 do plano proibe readback novo decidindo draw, entao a GPU NAO pode dizer a CPU, a tempo
// deste frame, quem ela recusou. Logo a divisao "indireto x fallback" e decidida pela CPU, com o
// MESMO predicado (`GpuIndirectEligible`), sobre dados que ela tambem possui: a classe do material
// e a flag de offset da malha. A CPU desenha o complemento.
//
// Isso parte os desfechos em duas naturezas, e confundi-las e o erro que custa geometria:
//
//   POLITICA  — `NotEligible` e `Unrepresentable`. Sao recusas previstas, e a CPU desenha esses
//               renderaveis pelo caminho classico. Crescem normalmente (masked, two-sided e Blend
//               existem em toda cena). O par (GPU recusa, CPU desenha) e o que torna "exatamente
//               uma vez" verdadeiro: os dois lados avaliam o mesmo predicado sobre a mesma linha.
//
//   CANARIO   — `NoSlot`, `InvalidSlot`, `InvalidMaterialId`, `InvalidMeshId`, `InvalidPoolId` e
//               `Overflow`. Significam que as tabelas discordam do que a CPU acha que existe.
//               Ninguem desenha esses neste frame — e por isso eles TEM de ser zero. Diferente de
//               zero significa que o caminho indireto nao esta utilizavel, e o A/B volta inteiro
//               para a CPU no frame seguinte. Mesma politica do overflow do expand.
//
// Nao ha terceira categoria, e e isso que o teste fixa: a soma dos desfechos e igual ao numero de
// candidatos. Um desfecho que nao incrementasse nada, ou que incrementasse dois, apareceria ali.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include <span>
#include <vector>

namespace Smile {
    // Um desfecho por candidato, exatamente um. Espelhado em
    // Shaders/GpuScene/ClassifyCandidates.cs.hlsl como SMILE_CLASSIFY_*; renumerar um lado sem o
    // outro nao quebra compilacao nenhuma, e o teste WARP le os indices de volta.
    //
    // Separados, e nao somados num "rejeitados", pela mesma razao do FGpuCandidateStats: as causas
    // tem consequencias diferentes. `NotEligible` crescendo e a cena ter folhagem; `InvalidMeshId`
    // crescendo e defeito.
    enum EGpuClassifyOutcome : u32 {
        GpuClassifyIndirect          = 0, // virou comando; contado tambem no lote
        GpuClassifyNoSlot            = 1, // posicao sem candidato (kGpuExpandNoSlot) — CANARIO
        GpuClassifyInvalidSlot       = 2, // slot fora da tabela, ou linha sem ocupante — CANARIO
        GpuClassifyInvalidMaterialId = 3, // CANARIO
        GpuClassifyInvalidMeshId     = 4, // CANARIO
        GpuClassifyInvalidPoolId     = 5, // pool fora da lista publicada — CANARIO
        GpuClassifyNotEligible       = 6, // classe fora do indireto nesta fase — POLITICA
        GpuClassifyUnrepresentable   = 7, // offset da malha fora do contrato do argumento — POLITICA
        GpuClassifyOverflow          = 8, // lote fora da capacidade de BatchCounts — CANARIO
        GpuClassifyOutcomeCount      = 9,
    };

    // O lote a que um comando pertence.
    //
    // `PassClass` NAO entra: ele e eixo do PASSE, e o passe e quem faz o dispatch — carrega-lo no
    // lote daria o produto cartesiano das duas familias dentro de um buffer que ja e por passe.
    //
    // `GeometryPoolId` entra porque um `ExecuteIndirect` amarra UM par VB/IB: lote nao cruza pool.
    struct FGpuBatchKey {
        u32 PipelineClass  = 0;
        u32 GeometryPoolId = 0;
    };

    // Indice denso do lote em BatchCounts. CLASS-MAJOR: os lotes de uma classe ficam contiguos, que
    // e a ordem em que os `ExecuteIndirect` de uma classe vao ler os argumentos.
    //
    // As quatro classes ocupam espaco mesmo com so uma elegivel nesta fase. Custa unidades de dwords
    // (pools sao chunks de 256 MB, nao milhares), poupa uma troca de formato na Fase 7, e da ao
    // teste uma asseracao util: as faixas das classes 1..3 tem de ficar em ZERO, o que prova que a
    // elegibilidade barrou de fato, em vez de o dado nao ter aparecido.
    inline u32 GpuBatchIndex(const FGpuBatchKey& _Key, u32 _PoolCount) {
        return _Key.PipelineClass * _PoolCount + _Key.GeometryPoolId;
    }
    inline u32 GpuBatchSlotCount(u32 _PoolCount) { return kGpuPipelineClassCount * _PoolCount; }

    // Desfecho de UM candidato.
    //
    // Recebe as tabelas por SPAN de proposito: a validacao de indice pertence a esta funcao, e nao
    // ao chamador. Se ela recebesse as linhas ja lidas, a leitura que ela existe para impedir ja
    // teria acontecido no `operator[]` de quem chama.
    //
    // A ordem e slot -> meta -> ids -> linhas -> pool -> elegibilidade, e cada passo valida antes de
    // usar o resultado do anterior.
    //
    // `_OutKey` so e escrita no desfecho `Indirect`.
    inline u32 GpuClassifyCandidate(u32 _Slot,
                                    std::span<const FGpuSceneInstanceMeta> _Meta,
                                    std::span<const FGpuMaterialRecord> _Materials,
                                    std::span<const FGpuMeshRecord> _Meshes,
                                    u32 _PoolCount,
                                    FGpuBatchKey& _OutKey) {
        if (_Slot == kGpuExpandNoSlot) return GpuClassifyNoSlot;
        if (_Slot >= _Meta.size())     return GpuClassifyInvalidSlot;

        const FGpuSceneInstanceMeta& Meta = _Meta[_Slot];
        // Generation zero e linha VAZIA. O expand ja comparou a generation ESPERADA do candidato;
        // aqui so resta perguntar se a linha tem ocupante, que e o invariante que sobra depois dele.
        if (Meta.Generation == 0u) return GpuClassifyInvalidSlot;

        if (Meta.MaterialId >= _Materials.size()) return GpuClassifyInvalidMaterialId;
        if (Meta.MeshId >= _Meshes.size())        return GpuClassifyInvalidMeshId;

        const FGpuMaterialRecord& Material = _Materials[Meta.MaterialId];
        const FGpuMeshRecord&     Mesh     = _Meshes[Meta.MeshId];

        // O pool e validado ANTES da elegibilidade: um pool fora da lista e defeito de tabela, e
        // reporta-lo como "nao elegivel" o esconderia numa contagem que cresce por bons motivos.
        if (Mesh.GeometryPoolId >= _PoolCount) return GpuClassifyInvalidPoolId;

        // A PORTA e o predicado completo, e so ele decide. A decomposicao abaixo nunca decide nada:
        // ela apenas ATRIBUI o motivo do que ja foi recusado. Escrever `if (!classe) ... else if
        // (!flag) ...` deixaria a decisao espalhada em dois testes, e no dia em que um deles sumisse
        // a malha irrepresentavel receberia comando com offset poison.
        if (!GpuIndirectEligible(Material.PipelineClass, Mesh.Flags)) {
            // Com os dois termos fechados, a CLASSE tem precedencia no relato. O desfecho e o mesmo
            // (fallback CPU); o que isto garante e que o motivo seja deterministico.
            return GpuPipelineClassIndirectEligible(Material.PipelineClass)
                       ? GpuClassifyUnrepresentable
                       : GpuClassifyNotEligible;
        }

        _OutKey.PipelineClass  = Material.PipelineClass;
        _OutKey.GeometryPoolId = Mesh.GeometryPoolId;
        return GpuClassifyIndirect;
    }

    // O que o dispatch recebe. Espelha o ClassifyCB do shader.
    struct FGpuClassifyInput {
        std::span<const u32>                   Slots; // saida do ExpandInstanceRanges
        std::span<const FGpuSceneInstanceMeta> Meta;
        std::span<const FGpuMaterialRecord>    Materials;
        std::span<const FGpuMeshRecord>        Meshes;
        u32 PoolCount     = 0;
        u32 BatchCapacity = 0; // slots de BatchCounts; ver GpuBatchSlotCount
    };

    // Referencia CPU do dispatch. E o oraculo do teste WARP e, depois, o braco A/B.
    //
    // As duas saidas sao ZERADAS aqui, e nao acumuladas: o shader tambem parte de contadores zerados,
    // e comparar acumulados de origens diferentes esconderia um desfecho que nunca incrementa.
    //
    // Devolve quantos candidatos viraram comando (== OutCounters[GpuClassifyIndirect]).
    u32 ClassifyCandidates(const FGpuClassifyInput& Input, std::vector<u32>& OutBatchCounts,
                           std::vector<u32>& OutCounters);
}
