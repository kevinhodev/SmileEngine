// Scatter dos argumentos indiretos (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Segunda passada sobre os candidatos. Quem o classify contou como INDIRECT recebe uma posicao
// dentro da REGIAO do seu lote — `Regions[lote].Offset + cursor` — e o comando que o
// `GpuClassifyOne` ja montou.
//
// ⚠️ A REGRA DESTE ARQUIVO: depois de `Outcome == INDIRECT`, NENHUMA RECUSA SILENCIOSA. O classify ja
// contou o candidato, e a contagem reservou a posicao; um candidato recusado aqui deixaria a posicao
// sem argumento, e o `ExecuteIndirect` executaria ali o comando do frame passado. Por isso nao ha
// leitura de tabela neste arquivo — a decisao e o comando vem inteiros do `GpuClassifyOne` —, e todo
// ramo que nao escreve registra FALHA.
//
// As regioes vem da CPU. Nao ha prefixo na GPU: o offset do argumento e parametro da CPU no
// `ExecuteIndirect`, e um offset calculado aqui so chegaria la por readback.
//
// Mesmo padrao do WaterGenerateDraws.cs.hlsl, que ja faz contagem e scatter com `InterlockedAdd` de
// tres argumentos: o valor de saida e o ANTERIOR ao incremento, e serve de indice base-zero dentro da
// regiao. O teste semeia os argumentos com poison e compara o multiconjunto exato de cada regiao,
// entao essa semantica e verificada, e nao presumida — com o valor POSTERIOR, o primeiro comando de
// cada lote cairia uma posicao a frente, e o teste ve o buraco e o residuo.

#include "GpuBatchCounters.hlsli"
#include "GpuClassifyShared.hlsli"

cbuffer ScatterCB : register(b0) {
    uint4 ScatterParams;  // x=candidatos, y=linhas de meta, z=linhas de material, w=linhas de malha
    uint4 ScatterParams2; // x=pools, y=capacidade de IndirectArgs (em COMANDOS); zw reservados
};

StructuredBuffer<uint> ExpandedSlots : register(t0);
// t1..t3 sao as tabelas, declaradas pelo GpuClassifyShared.hlsli.
StructuredBuffer<GpuIndirectRegion> Regions : register(t4);
// As contagens do classify, para LIMITAR o cursor. Ver a nota das falhas no main.
StructuredBuffer<uint> BatchCounts : register(t5);

RWStructuredBuffer<GpuIndirectDrawArgs> IndirectArgs : register(u0);
RWStructuredBuffer<uint>                BatchCursors : register(u1);
RWStructuredBuffer<uint>                Counters     : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint Index = dtid.x;
    if (Index >= ScatterParams.x) return;

    const GpuClassifyResult C = GpuClassifyOne(ExpandedSlots[Index], ScatterParams.y,
                                               ScatterParams.z, ScatterParams.w,
                                               ScatterParams2.x);
    // Nao contado aqui: a contabilidade de quem NAO virou comando e do classify. Contar de novo
    // daria dois registros do mesmo fato, e os dois poderiam discordar.
    if (C.Outcome != SMILE_CLASSIFY_INDIRECT) return;

    // ---- a partir daqui, a posicao ja foi reservada pela contagem ----
    //
    // ⚠️ Nao existe ramo que deixe de escrever SEM AVISAR. Os tres ramos de falha abaixo nao escrevem,
    // e por isso registram FALHA — e a finalizacao, que roda depois deste passo, zera o `DrawCounts`
    // do frame inteiro quando qualquer falha foi registrada. O resultado de uma falha e zero
    // primitivas, e nunca um comando velho.

    uint Cursor;
    InterlockedAdd(BatchCursors[C.Batch], 1u, Cursor); // Cursor = valor ANTERIOR

    // Cursor alem da contagem do lote. Pega cursor SUJO: se o lote comecou em s > 0, os N candidatos
    // levam o cursor ate s + N - 1 >= N, e pelo menos o ultimo cai aqui — todo inicio nao zero num
    // lote com candidatos e detectado. Pega tambem contagem divergente, que a decisao compartilhada
    // torna impossivel e este teste verifica.
    if (Cursor >= BatchCounts[C.Batch]) {
        InterlockedAdd(Counters[SMILE_SCATTER_FAULT_CURSOR], 1u);
        return;
    }

    const GpuIndirectRegion Region = Regions[C.Batch];

    // Cursor alem da capacidade da REGIAO: o frame tem mais comandos neste lote do que a CPU reservou.
    // Escrever mesmo assim poria o comando na primeira posicao da regiao SEGUINTE — que o
    // `ExecuteIndirect` do outro lote desenharia com o par VB/IB do pool errado. A finalizacao
    // invalida o frame por este contador e tambem pela contagem contra a capacidade.
    if (Cursor >= Region.Capacity) {
        InterlockedAdd(Counters[SMILE_SCATTER_FAULT_REGION], 1u);
        return;
    }

    // Regiao que nao cabe em `IndirectArgs`. Inalcancavel sob `GpuRegionPreflight`; se alcancada, a
    // tabela que a GPU recebeu nao e a que a CPU aprovou.
    //
    // Escrito para NAO dar a volta em u32: `Offset + Capacity > Capacidade` somaria antes de comparar,
    // e uma regiao que comeca perto de 0xFFFFFFFF passaria com um fim pequeno. Com `Capacity` ja
    // limitada pela capacidade, a subtracao a direita nao tem como dar a volta. E daqui sai tambem a
    // garantia de que `Offset + Cursor` abaixo cabe: `Cursor < Capacity`.
    const uint Capacidade = ScatterParams2.y;
    if (Region.Capacity > Capacidade || Region.Offset > Capacidade - Region.Capacity) {
        InterlockedAdd(Counters[SMILE_SCATTER_FAULT_CAPACITY], 1u);
        return;
    }

    IndirectArgs[Region.Offset + Cursor] = C.Args;
    InterlockedAdd(Counters[SMILE_SCATTER_EMITTED], 1u);
}
