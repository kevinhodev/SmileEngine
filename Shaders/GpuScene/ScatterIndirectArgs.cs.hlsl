// Scatter dos argumentos indiretos (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Segunda passada sobre os candidatos. Quem o classify contou como INDIRECT recebe uma posicao
// dentro do seu lote — `BatchOffsets[lote] + cursor` — e o comando que o `GpuClassifyOne` ja montou.
//
// ⚠️ A REGRA DESTE ARQUIVO: depois de `Outcome == INDIRECT`, NENHUMA RECUSA. O classify ja contou o
// candidato, e a contagem reservou a posicao; um candidato recusado aqui deixaria a posicao sem
// argumento, e o `ExecuteIndirect` executaria ali o comando do frame passado. Por isso nao ha
// leitura de tabela neste arquivo — a decisao e o comando vem inteiros do `GpuClassifyOne`, e o que
// sobra aqui e reservar e copiar.
//
// Mesmo padrao do WaterGenerateDraws.cs.hlsl, que ja faz contagem/prefixo/scatter com
// `InterlockedAdd` de tres argumentos: o valor de saida e o ANTERIOR ao incremento, e serve de
// indice base-zero dentro do lote. O teste semeia os argumentos com poison e compara o multiconjunto
// exato de cada lote, entao essa semantica e verificada, e nao presumida — com o valor POSTERIOR, o
// primeiro comando de cada lote cairia uma posicao a frente, e o teste ve o buraco e o residuo.

#include "GpuBatchCounters.hlsli"
#include "GpuClassifyShared.hlsli"

cbuffer ScatterCB : register(b0) {
    uint4 ScatterParams;  // x=candidatos, y=linhas de meta, z=linhas de material, w=linhas de malha
    uint4 ScatterParams2; // x=pools, y=capacidade de IndirectArgs (em COMANDOS); zw reservados
};

StructuredBuffer<uint> ExpandedSlots  : register(t0);
// t1..t3 sao as tabelas, declaradas pelo GpuClassifyShared.hlsli.
StructuredBuffer<uint> BatchOffsets   : register(t4);
StructuredBuffer<uint> PrefixCounters : register(t5);
// As contagens do classify, para LIMITAR o cursor. Ver a nota das falhas no main.
StructuredBuffer<uint> BatchCounts    : register(t6);

RWStructuredBuffer<GpuIndirectDrawArgs> IndirectArgs : register(u0);
RWStructuredBuffer<uint>                BatchCursors : register(u1);
RWStructuredBuffer<uint>                Counters     : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint Index = dtid.x;
    if (Index >= ScatterParams.x) return;

    // Prefixo que nao fechou: o frame inteiro fica sem comando.
    //
    // ⚠️ Isto NAO e o que suprime o draw — quem faz isso e o `DrawCounts` zerado que a finalizacao
    // publica, porque o `ExecuteIndirect` le o count buffer e nao este shader. Este teste existe
    // por outro motivo: sob estouro os offsets estao envenenados, e u32 da a volta. O cursor 0 cairia
    // em 0xFFFFFFFF (fora, recusado), mas o cursor 1 cairia em 0xFFFFFFFF + 1 = 0 — e escreveria em
    // cima do primeiro comando do primeiro lote. O poison sozinho nao basta.
    if (PrefixCounters[SMILE_PREFIX_OVERFLOW] != 0u) {
        InterlockedAdd(Counters[SMILE_SCATTER_SUPPRESSED], 1u);
        return;
    }

    const GpuClassifyResult C = GpuClassifyOne(ExpandedSlots[Index], ScatterParams.y,
                                               ScatterParams.z, ScatterParams.w,
                                               ScatterParams2.x);
    // Nao contado aqui: a contabilidade de quem NAO virou comando e do classify. Contar de novo
    // daria dois registros do mesmo fato, e os dois poderiam discordar.
    if (C.Outcome != SMILE_CLASSIFY_INDIRECT) return;

    // ---- a partir daqui, a posicao ja foi reservada pela contagem ----
    //
    // ⚠️ Nao existe ramo que deixe de escrever SEM AVISAR. Um candidato contado e nao escrito deixa a
    // posicao com o conteudo anterior, e o `ExecuteIndirect` o executaria. Os dois ramos de falha
    // abaixo nao escrevem, e por isso registram FALHA — e a finalizacao, que roda depois deste passo,
    // zera o `DrawCounts` do frame inteiro quando qualquer falha foi registrada. O resultado de uma
    // falha e zero primitivas, e nunca um comando velho.

    uint Cursor;
    InterlockedAdd(BatchCursors[C.Batch], 1u, Cursor); // Cursor = valor ANTERIOR

    // Cursor alem da contagem do lote. Pega cursor SUJO: se o lote comecou em s > 0, os N candidatos
    // levam o cursor ate s + N - 1 >= N, e pelo menos o ultimo cai aqui — todo inicio nao zero num
    // lote com candidatos e detectado. Pega tambem contagem divergente, que a decisao compartilhada
    // torna impossivel e este teste verifica.
    //
    // Sem ele, um cursor sujo nem sempre estouraria a capacidade: poderia so deslocar comandos para
    // dentro do intervalo do lote seguinte, sem contador nenhum disparar.
    if (Cursor >= BatchCounts[C.Batch]) {
        InterlockedAdd(Counters[SMILE_SCATTER_FAULT_CURSOR], 1u);
        return;
    }

    // Posicao fora do recurso. Com o cursor dentro da contagem, a posicao fica abaixo do total do
    // prefixo, e o `GpuScatterPreflight` garante total <= capacidade: inalcancavel sob preflight. Fica
    // porque um indice fora do recurso nao tem onde ser escrito — e, pelo mesmo motivo do ramo acima,
    // vira falha que invalida o frame, e nao um salto silencioso.
    const uint Position = BatchOffsets[C.Batch] + Cursor;
    if (Position >= ScatterParams2.y) {
        InterlockedAdd(Counters[SMILE_SCATTER_FAULT_CAPACITY], 1u);
        return;
    }

    IndirectArgs[Position] = C.Args;
    InterlockedAdd(Counters[SMILE_SCATTER_EMITTED], 1u);
}
