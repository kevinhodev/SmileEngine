// Observador da GRAVACAO dos `ExecuteIndirect` com dois pools (Fase 5 do GPU-DRIVEN-PLAN).
//
// Relata QUAL VERTICE cada comando leu, e para qual instancia. O vertex buffer de cada pool carrega,
// por vertice, um TAG que diz de que pool ele e e qual a sua posicao na malha. Um lote desenhado com o
// par VB/IB de OUTRO pool nao muda a contagem de primitivas — os comandos sao os mesmos —, muda os
// TAGS. E por isso que pipeline statistics sozinho nao prova a gravacao: trocar os pools mantem o
// numero de primitivas.
//
// A unidade e o par (slot de instancia, tag): `Seen[slot * SMILE_STREAM_PROBE_TAGS + tag] = 1`. A
// escrita e IDEMPOTENTE — o mesmo vertice processado mais de uma vez escreve o mesmo 1 —, entao o
// resultado nao depende de quantas vezes o VS roda por vertice.
//
// Slot acima do teto, que nenhum comando da cena tem, cai na linha `SMILE_STREAM_PROBE_SLOTS`: um
// comando que nao devia ter sido executado aparece ali, e nunca escreve fora do buffer.
//
// Este shader nao entra em nenhum passe.

#include "GpuIndirectDraw.hlsli"

// Espelhados em `kProbeTags` e `kProbeSlots` no teste. Auto-verificaveis: o teste confere o conjunto
// exato de posicoes escritas, e um valor diferente desloca todas elas.
#define SMILE_STREAM_PROBE_TAGS  128u // 64 por pool, dois pools
#define SMILE_STREAM_PROBE_SLOTS 32u  // a linha 32 e a "fora do teto"

cbuffer DrawConstantsCB : register(b4) {
    SMILE_DRAW_CONSTANTS_BLOCK
};

RWStructuredBuffer<uint> Seen : register(u0);

struct FVertexIn {
    uint Tag : TEXCOORD0;
};

float4 main(FVertexIn In) : SV_Position {
    const uint Slot = min(InstanceSlot, SMILE_STREAM_PROBE_SLOTS);
    const uint Tag  = min(In.Tag, SMILE_STREAM_PROBE_TAGS - 1u);
    Seen[Slot * SMILE_STREAM_PROBE_TAGS + Tag] = 1u;
    // Fora do frustum: a sonda so existe pelo efeito colateral do UAV.
    return float4(2.0f, 2.0f, 0.5f, 1.0f);
}
