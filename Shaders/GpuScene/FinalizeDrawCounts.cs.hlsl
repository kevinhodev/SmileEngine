// Publicacao do COUNT BUFFER do `ExecuteIndirect` (Fase 5 do GPU-DRIVEN-PLAN).
//
// O ultimo passo da cadeia classify -> scatter -> ESTE. Ele existe separado por uma razao de ordem: o
// count buffer tem de ser a ultima palavra, escrita pelo ultimo passo capaz de descobrir uma falha.
// Publicado antes do scatter, ele ja estaria no ar quando o scatter falhasse — e o `ExecuteIndirect`
// executaria a contagem sobre uma posicao que o scatter nao escreveu.
//
// `DrawCounts[b]` = `BatchCounts[b]` quando o frame fechou inteiro, e ZERO em todas as entradas
// quando QUALQUER falha foi registrada: contagem alem da regiao, falha do scatter, ou algum elo da
// verificacao de reset. Count buffer zerado faz o command processor executar zero comandos, e essa e
// a unica forma de suprimir o draw sem readback — a decisao acontece na GPU, dentro do buffer que ele
// le.
//
// UMA THREAD: sao unidades a poucas centenas de lotes.

#include "GpuBatchCounters.hlsli"

cbuffer FinalizeCB : register(b0) {
    uint4 FinalizeParams; // x = slots de lote, y = candidatos DESTA execucao; zw reservados
};

StructuredBuffer<uint>              BatchCounts      : register(t0);
StructuredBuffer<GpuIndirectRegion> Regions          : register(t1);
StructuredBuffer<uint>              ScatterCounters  : register(t2);
StructuredBuffer<uint>              BatchCursors     : register(t3);
StructuredBuffer<uint>              ClassifyCounters : register(t4);

RWStructuredBuffer<uint> DrawCounts : register(u0);
// [0] = 1 quando o frame fechou, 0 quando foi invalidado. [1] = bits SMILE_FINALIZE_* do motivo.
// Telemetria: nenhum draw depende deles, e le-los na CPU para decidir algo seria readback.
RWStructuredBuffer<uint> Status     : register(u1);

[numthreads(1, 1, 1)]
void main() {
    const uint Slots      = FinalizeParams.x;
    const uint Candidatos = FinalizeParams.y;
    uint Motivo = 0u;

    if (ScatterCounters[SMILE_SCATTER_FAULT_CURSOR] != 0u)   Motivo |= SMILE_FINALIZE_FAULT_CURSOR;
    if (ScatterCounters[SMILE_SCATTER_FAULT_LAYOUT_VERSION] != 0u)
        Motivo |= SMILE_FINALIZE_FAULT_LAYOUT_VERSION;
    if (ScatterCounters[SMILE_SCATTER_FAULT_REGION] != 0u)   Motivo |= SMILE_FINALIZE_FAULT_REGION;
    if (ScatterCounters[SMILE_SCATTER_FAULT_CAPACITY] != 0u) Motivo |= SMILE_FINALIZE_FAULT_CAPACITY;

    // ---- a verificacao dos RESETS, a partir de uma ANCORA que nao acumula ----
    //
    // Comparar acumuladores entre si nao basta: se o reset inteiro for esquecido, os quatro crescem
    // em sincronia e toda igualdade entre eles continua valendo — as contagens dobram, os cursores
    // continuam de onde pararam e terminam iguais a elas. A ancora e `Candidatos`, que vem do cbuffer
    // e e escrito do zero a cada execucao. A cadeia de igualdades a propaga ate o ultimo acumulador,
    // e cada elo pega o reset de um deles.
    //
    // A capacidade das regioes tambem nao acumula, mas NAO substitui a ancora: com folga na regiao —
    // instancia escondida hoje, culling na Fase 6 —, a contagem dobrada cabe, e so a ancora a ve.

    // 1. Os contadores do classify somam exatamente os candidatos desta execucao.
    uint SomaClassify = 0u;
    for (uint K = 0u; K < SMILE_CLASSIFY_OUTCOME_COUNT; ++K) SomaClassify += ClassifyCounters[K];
    if (SomaClassify != Candidatos) Motivo |= SMILE_FINALIZE_CLASSIFY_ANCHOR;

    // 2. Os lotes somam exatamente os indiretos. 3. Cada lote recebeu exatamente a sua contagem de
    //    cursores: abaixo e contagem inflada, acima e cursor sujo. E, por lote, a REGRA DE PUBLICACAO
    //    das regioes: contagem alem da capacidade invalida o frame inteiro, e nao so o lote.
    const uint Indiretos = ClassifyCounters[SMILE_CLASSIFY_INDIRECT];
    uint SomaLotes = 0u;
    for (uint I = 0u; I < Slots; ++I) {
        SomaLotes += BatchCounts[I];
        if (BatchCursors[I] != BatchCounts[I])    Motivo |= SMILE_FINALIZE_CURSOR_MISMATCH;
        if (BatchCounts[I] > Regions[I].Capacity) Motivo |= SMILE_FINALIZE_REGION_OVERFLOW;
    }
    if (SomaLotes != Indiretos) Motivo |= SMILE_FINALIZE_BATCH_SUM;

    // 4. Todo indireto que o scatter viu terminou em exatamente um desfecho dele.
    const uint Tratados = ScatterCounters[SMILE_SCATTER_EMITTED] +
                          ScatterCounters[SMILE_SCATTER_FAULT_CURSOR] +
                          ScatterCounters[SMILE_SCATTER_FAULT_LAYOUT_VERSION] +
                          ScatterCounters[SMILE_SCATTER_FAULT_REGION] +
                          ScatterCounters[SMILE_SCATTER_FAULT_CAPACITY];
    if (Tratados != Indiretos) Motivo |= SMILE_FINALIZE_SCATTER_SUM;

    const bool Falhou = Motivo != 0u;
    // TODOS os slots, sempre. Uma entrada nao escrita guardaria a contagem do frame anterior.
    for (uint J = 0u; J < Slots; ++J) DrawCounts[J] = Falhou ? 0u : BatchCounts[J];
    Status[0] = Falhou ? 0u : 1u;
    Status[1] = Motivo;
}
