// Publicacao do COUNT BUFFER do `ExecuteIndirect` (Fase 5 do GPU-DRIVEN-PLAN).
//
// O ultimo passo da cadeia expand -> classify -> scatter -> ESTE. Ele existe separado por uma razao de
// ordem: o count buffer tem de ser a ultima palavra, escrita pelo ultimo passo capaz de descobrir uma
// falha. Publicado antes do scatter, ele ja estaria no ar quando o scatter falhasse — e o
// `ExecuteIndirect` executaria a contagem sobre uma posicao que o scatter nao escreveu.
//
// `DrawCounts[b]` = `BatchCounts[b]` quando o frame fechou inteiro, e ZERO em todas as entradas
// quando QUALQUER falha foi registrada: contagem alem da regiao, falha do scatter, algum elo da
// verificacao de reset, ou um CANARIO do expand ou do classify. Count buffer zerado faz o command
// processor executar zero comandos, e essa e a unica forma de suprimir o draw sem readback — a decisao
// acontece na GPU, dentro do buffer que ele le.
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
StructuredBuffer<uint>              ExpandCounters   : register(t5);

RWStructuredBuffer<uint> DrawCounts : register(u0);
// [0] = 1 quando o frame fechou, 0 quando foi invalidado. [1] = bits SMILE_FINALIZE_* do motivo.
// Telemetria: nenhum draw depende deles, e le-los na CPU para decidir algo seria readback.
RWStructuredBuffer<uint> Status     : register(u1);

// Soma um contador sem deixar a aritmetica modular transformar acumulador corrompido em valor
// plausivel. `Limite` e uma ancora desta execucao (candidatos ou indiretos), portanto qualquer termo
// ou parcial acima dele ja e falha; a subtracao so acontece depois de `Valor <= Limite`.
bool SomaAte(inout uint Soma, uint Valor, uint Limite) {
    if (Valor > Limite || Soma > Limite - Valor) return false;
    Soma += Valor;
    return true;
}

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
    // Comparar acumuladores entre si nao basta: se o reset inteiro for esquecido, os acumuladores crescem
    // em sincronia e toda igualdade entre eles continua valendo — as contagens dobram, os cursores
    // continuam de onde pararam e terminam iguais a elas. A ancora e `Candidatos`, que vem do cbuffer
    // e e escrito do zero a cada execucao. A cadeia de igualdades a propaga ate o ultimo acumulador,
    // e cada elo pega o reset de um deles.
    //
    // A capacidade das regioes tambem nao acumula, mas NAO substitui a ancora: com folga na regiao —
    // instancia escondida hoje, culling na Fase 6 —, a contagem dobrada cabe, e so a ancora a ve.

    // 1. Os contadores do expand e os do classify somam, cada um, exatamente os candidatos desta
    //    execucao: os dois passos dao um desfecho por posicao de saida.
    uint SomaExpand = 0u;
    bool SomaExpandValida = true;
    for (uint E = 0u; E < SMILE_EXPAND_COUNTER_COUNT; ++E)
        SomaExpandValida = SomaAte(SomaExpand, ExpandCounters[E], Candidatos) && SomaExpandValida;
    if (!SomaExpandValida || SomaExpand != Candidatos) Motivo |= SMILE_FINALIZE_EXPAND_ANCHOR;

    uint SomaClassify = 0u;
    bool SomaClassifyValida = true;
    for (uint K = 0u; K < SMILE_CLASSIFY_OUTCOME_COUNT; ++K)
        SomaClassifyValida = SomaAte(SomaClassify, ClassifyCounters[K], Candidatos) &&
                             SomaClassifyValida;
    if (!SomaClassifyValida || SomaClassify != Candidatos) Motivo |= SMILE_FINALIZE_CLASSIFY_ANCHOR;

    // ---- CANARIOS: um candidato que ninguem desenha ----
    //
    // A soma dos desfechos prova que cada candidato foi CONTADO; ela nao separa politica de corrupcao.
    // A CPU desenha pelo caminho classico exatamente quem o predicado de elegibilidade recusa — e so
    // isso. Um candidato elegivel que o expand ou o classify recusou por outro motivo nao entra no
    // indireto nem no fallback: some. Publicar os outros lotes seria sumir com ele em silencio, num frame
    // que fecha; zerar tudo troca isso por um frame que nao desenha nada, que e defeito visivel.
    //
    // A POLITICA e uma lista FECHADA — comando, `NotEligible` e `MeshUnusable` — e todo o resto e
    // canario. Um desfecho novo nasce canario ate alguem declara-lo politica: o erro por omissao zera o
    // frame, e nao some com geometria.
    for (uint K = 0u; K < SMILE_CLASSIFY_OUTCOME_COUNT; ++K) {
        const bool MantemFrame = K == SMILE_CLASSIFY_INDIRECT ||
                                 K == SMILE_CLASSIFY_NOT_ELIGIBLE ||
                                 K == SMILE_CLASSIFY_MESH_UNUSABLE ||
                                 K == SMILE_CLASSIFY_FRUSTUM_REJECTED ||
                                 K == SMILE_CLASSIFY_OCCLUSION_REJECTED;
        if (!MantemFrame && ClassifyCounters[K] != 0u)
            Motivo |= SMILE_FINALIZE_CLASSIFY_CANARY;
    }

    // No expand, so `Expanded` e desfecho normal. O `Overflow` e o pior dos tres: a posicao alem da saida
    // nao recebe nem a sentinela, e o classify poderia ler ali um slot plausivel e errado.
    if (ExpandCounters[SMILE_EXPAND_COUNTER_INVALID_SLOT] != 0u ||
        ExpandCounters[SMILE_EXPAND_COUNTER_RT_ONLY] != 0u ||
        ExpandCounters[SMILE_EXPAND_COUNTER_OVERFLOW] != 0u)
        Motivo |= SMILE_FINALIZE_EXPAND_CANARY;

    // 2. Os lotes somam exatamente os indiretos. 3. Cada lote recebeu exatamente a sua contagem de
    //    cursores: abaixo e contagem inflada, acima e cursor sujo. E, por lote, a REGRA DE PUBLICACAO
    //    das regioes: contagem alem da capacidade invalida o frame inteiro, e nao so o lote.
    const uint Indiretos = ClassifyCounters[SMILE_CLASSIFY_INDIRECT];
    uint SomaLotes = 0u;
    bool SomaLotesValida = true;
    for (uint I = 0u; I < Slots; ++I) {
        SomaLotesValida = SomaAte(SomaLotes, BatchCounts[I], Indiretos) && SomaLotesValida;
        if (BatchCursors[I] != BatchCounts[I])    Motivo |= SMILE_FINALIZE_CURSOR_MISMATCH;
        if (BatchCounts[I] > Regions[I].Capacity) Motivo |= SMILE_FINALIZE_REGION_OVERFLOW;
    }
    if (!SomaLotesValida || SomaLotes != Indiretos) Motivo |= SMILE_FINALIZE_BATCH_SUM;

    // 4. Todo indireto que o scatter viu terminou em exatamente um desfecho dele.
    uint Tratados = 0u;
    bool TratadosValidos = true;
    for (uint S = 0u; S < SMILE_SCATTER_COUNT; ++S)
        TratadosValidos = SomaAte(Tratados, ScatterCounters[S], Indiretos) && TratadosValidos;
    if (!TratadosValidos || Tratados != Indiretos) Motivo |= SMILE_FINALIZE_SCATTER_SUM;

    const bool Falhou = Motivo != 0u;
    // TODOS os slots, sempre. Uma entrada nao escrita guardaria a contagem do frame anterior.
    for (uint J = 0u; J < Slots; ++J) DrawCounts[J] = Falhou ? 0u : BatchCounts[J];
    Status[0] = Falhou ? 0u : 1u;
    Status[1] = Motivo;
}
