#ifndef SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI
#define SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI

// Os contadores que atravessam a cadeia prefixo -> scatter -> finalizacao (Fase 5).
//
// Moram juntos porque cada passo le os do anterior. Uma copia de indice divergente num deles faria o
// passo ler o numero errado exatamente no frame em que devia se abster — por exemplo, a finalizacao
// lendo o TOTAL no lugar da flag de estouro e publicando contagens sobre argumentos que nao existem.

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

// Da FINALIZACAO: por que o frame foi invalidado. Bits, espelhando EGpuFinalizeReason. Telemetria —
// nenhum draw depende deles; o que suprime e o `DrawCounts` zerado.
#define SMILE_FINALIZE_PREFIX_OVERFLOW  1u   // o prefixo nao fechou
#define SMILE_FINALIZE_FAULT_CURSOR     2u   // falha de cursor registrada pelo scatter
#define SMILE_FINALIZE_FAULT_CAPACITY   4u   // falha de capacidade registrada pelo scatter
#define SMILE_FINALIZE_CLASSIFY_ANCHOR  8u   // contadores do classify != candidatos: sem reset
#define SMILE_FINALIZE_BATCH_SUM        16u  // soma dos lotes != indiretos: BatchCounts sem reset
#define SMILE_FINALIZE_CURSOR_MISMATCH  32u  // cursor != contagem em algum lote
#define SMILE_FINALIZE_SCATTER_SUM      64u  // emitidos + falhas != indiretos: contadores sem reset

// Do PREFIXO. Espelha EGpuPrefixCounter.
#define SMILE_PREFIX_TOTAL    0u
#define SMILE_PREFIX_OVERFLOW 1u
#define SMILE_PREFIX_COUNT    2u

// Do SCATTER. Espelha EGpuScatterCounter.
#define SMILE_SCATTER_EMITTED        0u
#define SMILE_SCATTER_SUPPRESSED     1u // prefixo nao fechou: o scatter se absteve
// As duas FALHAS. Qualquer uma delas invalida o frame na finalizacao: o candidato ja estava contado,
// e nao ter escrito o argumento deixa a posicao com o conteudo anterior.
#define SMILE_SCATTER_FAULT_CURSOR   2u // cursor alem da contagem do lote (cursores sujos ou contagem divergente)
#define SMILE_SCATTER_FAULT_CAPACITY 3u // posicao fora de IndirectArgs
#define SMILE_SCATTER_COUNT          4u

// Offset gravado em toda entrada que o prefixo nao conseguiu calcular. Espelha
// kGpuBatchOffsetPoison. Nao e zero: zero e o offset legitimo do primeiro lote.
#define SMILE_BATCH_OFFSET_POISON 0xFFFFFFFFu

#endif
