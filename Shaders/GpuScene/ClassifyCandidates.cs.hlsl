// Classificacao e contagem por lote (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Le `ExpandedSlots` (saida do ExpandInstanceRanges) e escreve `BatchCounts` + `Counters`. Esta
// fatia so CONTA: o prefixo, a compactacao e a escrita dos argumentos vem depois.
//
// A DECISAO nao mora aqui: ela e o `GpuClassifyOne` do GpuClassifyShared.hlsli, que o scatter usa
// tambem. A razao esta escrita la — a contagem reserva a posicao do comando, e o scatter que
// avaliasse a pergunta por conta propria poderia recusar um candidato ja contado.
//
// UMA THREAD POR CANDIDATO. A saida nao e posicional — dois candidatos do mesmo lote somam no MESMO
// slot —, entao aqui o InterlockedAdd e a operacao certa: o que se produz e um HISTOGRAMA.
//
// A regra do arquivo, espelhada do Smile/Graphics/Scene/GpuClassify.h: todo candidato incrementa
// EXATAMENTE um contador de desfecho. Quem vira comando incrementa tambem o lote.

#include "GpuClassifyShared.hlsli"

cbuffer ClassifyCB : register(b0) {
    uint4 ClassifyParams;  // x=candidatos, y=linhas de meta, z=linhas de material, w=linhas de malha
    uint4 ClassifyParams2; // x=pools, y=capacidade de BatchCounts; zw reservados
};

StructuredBuffer<uint> ExpandedSlots : register(t0);

RWStructuredBuffer<uint> BatchCounts : register(u0);
RWStructuredBuffer<uint> Counters    : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint Total         = ClassifyParams.x;
    const uint BatchCapacity = ClassifyParams2.y;
    const uint PoolCount     = ClassifyParams2.x;

    const uint Index = dtid.x;
    if (Index >= Total) return;

    const GpuClassifyResult C = GpuClassifyOne(ExpandedSlots[Index], ClassifyParams.y,
                                               ClassifyParams.z, ClassifyParams.w, PoolCount);
    if (C.Outcome != SMILE_CLASSIFY_INDIRECT) {
        InterlockedAdd(Counters[C.Outcome], 1u);
        return;
    }

    // Defesa de ultima instancia, e NAO politica. O `GpuClassifyPreflight` exige
    // `BatchCapacity >= kGpuPipelineClassCount * PoolCount` antes do dispatch, e com o `PoolCount`
    // validado dentro do `GpuClassifyOne` o indice ja esta limitado — sob preflight aprovado este
    // ramo e inalcancavel. Ele fica porque a capacidade viaja separada do PoolCount ate aqui e os
    // dois podem discordar; descobrir falta de espaco NESTE ponto ja seria geometria perdida no
    // frame, e por isso quem impede e o preflight, nao este contador.
    // O MESMO indice que o scatter vai usar para achar a posicao: calculado uma vez, dentro do
    // GpuClassifyOne, e nao aqui e la.
    const uint Batch = C.Batch;
    if (Batch >= BatchCapacity) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_OVERFLOW], 1u);
        return;
    }

    InterlockedAdd(BatchCounts[Batch], 1u);
    InterlockedAdd(Counters[SMILE_CLASSIFY_INDIRECT], 1u);
}
