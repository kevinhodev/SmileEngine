#ifndef SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI
#define SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI

// Os contadores e estruturas que atravessam a cadeia classify -> scatter -> finalizacao (Fase 5).
//
// Moram juntos porque cada passo le os do anterior. Uma copia de indice divergente num deles faria o
// passo ler o numero errado exatamente no frame em que devia invalidar — por exemplo, a finalizacao
// lendo `EMITTED` no lugar de uma falha e publicando contagens sobre argumentos que nao existem.

// Do CLASSIFY. Espelha EGpuClassifyOutcome; o teste WARP le os nove de volta.
//
// Moram aqui, e nao no GpuClassifyShared.hlsli, porque a finalizacao precisa deles e nao pode incluir
// aquele header: ele declara as tabelas em t1..t3, que na finalizacao sao outros buffers.
#define SMILE_CLASSIFY_INDIRECT            0u
#define SMILE_CLASSIFY_NO_SLOT             1u
#define SMILE_CLASSIFY_INVALID_SLOT        2u
#define SMILE_CLASSIFY_INVALID_MATERIAL_ID 3u
#define SMILE_CLASSIFY_INVALID_MESH_ID     4u
#define SMILE_CLASSIFY_INVALID_POOL_ID     5u
#define SMILE_CLASSIFY_NOT_ELIGIBLE        6u
#define SMILE_CLASSIFY_MESH_UNUSABLE       7u
#define SMILE_CLASSIFY_OVERFLOW            8u
#define SMILE_CLASSIFY_OUTCOME_COUNT       9u

// A REGIAO de um lote em `IndirectArgs`: `[Offset, Offset + Capacity)`. Espelha FGpuIndirectRegion.
//
// Vem da CPU, e nao de um prefixo na GPU: o `ExecuteIndirect` recebe o offset do argumento como
// parametro da CPU, entao o inicio de cada lote tem de ser conhecido por ela sem ler nada que a GPU
// escreveu. O porque inteiro esta no GpuClassify.h.
struct GpuIndirectRegion {
    uint Offset;
    uint Capacity;
};

// Da FINALIZACAO: por que o frame foi invalidado. Bits, espelhando EGpuFinalizeReason. Telemetria —
// nenhum draw depende deles; o que suprime e o `DrawCounts` zerado.
#define SMILE_FINALIZE_REGION_OVERFLOW  1u   // contagem de algum lote alem da capacidade da regiao
#define SMILE_FINALIZE_FAULT_CURSOR     2u   // falha de cursor registrada pelo scatter
#define SMILE_FINALIZE_FAULT_REGION     4u   // falha de regiao registrada pelo scatter
#define SMILE_FINALIZE_FAULT_CAPACITY   8u   // regiao fora de IndirectArgs, registrada pelo scatter
#define SMILE_FINALIZE_CLASSIFY_ANCHOR  16u  // contadores do classify != candidatos: sem reset
#define SMILE_FINALIZE_BATCH_SUM        32u  // soma dos lotes != indiretos: BatchCounts sem reset
#define SMILE_FINALIZE_CURSOR_MISMATCH  64u  // cursor != contagem em algum lote
#define SMILE_FINALIZE_SCATTER_SUM      128u // emitidos + falhas != indiretos: contadores sem reset

// Do SCATTER. Espelha EGpuScatterCounter.
#define SMILE_SCATTER_EMITTED        0u
// As tres FALHAS. Qualquer uma delas invalida o frame na finalizacao: o candidato ja estava contado,
// e nao ter escrito o argumento deixa a posicao com o conteudo anterior.
#define SMILE_SCATTER_FAULT_CURSOR   1u // cursor alem da contagem do lote (cursores sujos ou contagem divergente)
#define SMILE_SCATTER_FAULT_REGION   2u // cursor alem da capacidade da regiao do lote
#define SMILE_SCATTER_FAULT_CAPACITY 3u // regiao que nao cabe em IndirectArgs
#define SMILE_SCATTER_COUNT          4u

#endif
