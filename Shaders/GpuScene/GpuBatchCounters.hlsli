#ifndef SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI
#define SMILE_GPUSCENE_GPUBATCHCOUNTERS_HLSLI

// Os contadores e estruturas que atravessam a cadeia expand -> classify -> scatter -> finalizacao
// (Fases 4 e 5).
//
// Moram juntos porque cada passo le os do anterior. Uma copia de indice divergente num deles faria o
// passo ler o numero errado exatamente no frame em que devia invalidar — por exemplo, a finalizacao
// lendo `EMITTED` no lugar de uma falha e publicando contagens sobre argumentos que nao existem.

// Do EXPAND. Espelha EGpuExpandCounter; o teste WARP le os quatro de volta.
//
// Moram aqui, e nao no ExpandInstanceRanges.cs.hlsl, porque a finalizacao os le.
#define SMILE_EXPAND_COUNTER_EXPANDED     0u // slots escritos na saida
#define SMILE_EXPAND_COUNTER_INVALID_SLOT 1u // slot fora da tabela/lista, ou generation que nao bate
#define SMILE_EXPAND_COUNTER_RT_ONLY      2u // linha marcada RaytracingOnly: o produtor falhou
#define SMILE_EXPAND_COUNTER_OVERFLOW     3u // nao coube na capacidade da saida
#define SMILE_EXPAND_COUNTER_COUNT        4u

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
//
// `Version` e a do layout que produziu a regiao. O scatter a compara com a versao do layout que vai
// GRAVAR os `ExecuteIndirect`; zero nunca coincide, porque e o que memoria zerada contem.
struct GpuIndirectRegion {
    uint Offset;
    uint Capacity;
    uint Version;
};

// Da FINALIZACAO: por que o frame foi invalidado. Bits, espelhando EGpuFinalizeReason. Telemetria —
// nenhum draw depende deles; o que suprime e o `DrawCounts` zerado.
#define SMILE_FINALIZE_REGION_OVERFLOW        1u   // contagem de algum lote alem da capacidade da regiao
#define SMILE_FINALIZE_FAULT_CURSOR           2u   // falha de cursor registrada pelo scatter
#define SMILE_FINALIZE_FAULT_LAYOUT_VERSION   4u   // regiao de outro layout, registrada pelo scatter
#define SMILE_FINALIZE_FAULT_REGION           8u   // falha de regiao registrada pelo scatter
#define SMILE_FINALIZE_FAULT_CAPACITY         16u  // regiao fora de IndirectArgs, registrada pelo scatter
#define SMILE_FINALIZE_CLASSIFY_ANCHOR        32u  // contadores do classify != candidatos: sem reset
#define SMILE_FINALIZE_BATCH_SUM              64u  // soma dos lotes != indiretos: BatchCounts sem reset
#define SMILE_FINALIZE_CURSOR_MISMATCH        128u // cursor != contagem em algum lote
#define SMILE_FINALIZE_SCATTER_SUM            256u // emitidos + falhas != indiretos: contadores sem reset
#define SMILE_FINALIZE_CLASSIFY_CANARY        512u // algum desfecho do classify que nao e comando nem politica
#define SMILE_FINALIZE_EXPAND_CANARY          1024u // algum candidato recusado pelo expand
#define SMILE_FINALIZE_EXPAND_ANCHOR          2048u // contadores do expand != candidatos: sem reset

// Do SCATTER. Espelha EGpuScatterCounter.
#define SMILE_SCATTER_EMITTED              0u
// As quatro FALHAS. Qualquer uma delas invalida o frame na finalizacao: o candidato ja estava contado,
// e nao ter escrito o argumento deixa a posicao com o conteudo anterior.
#define SMILE_SCATTER_FAULT_CURSOR         1u // cursor alem da contagem do lote (cursores sujos ou contagem divergente)
#define SMILE_SCATTER_FAULT_LAYOUT_VERSION 2u // regiao de outro layout: tabela atrasada, frame slot trocado, upload esquecido
#define SMILE_SCATTER_FAULT_REGION         3u // cursor alem da capacidade da regiao do lote
#define SMILE_SCATTER_FAULT_CAPACITY       4u // regiao que nao cabe em IndirectArgs
#define SMILE_SCATTER_COUNT                5u

#endif
