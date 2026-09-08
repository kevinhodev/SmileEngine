// Expansao de ranges em lista de slots (Fase 4 do GPU-DRIVEN-PLAN, decisao D5).
//
// Le `CandidateRanges` + `CandidateIds` e escreve `ExpandedSlots`. Esta fase NAO executa culling:
// todo candidato valido passa. O que ela estabelece e a forma da entrada do culling e a
// contabilidade de quem nao passa — a Fase 6 troca o teste, nao o contrato.
//
// UMA THREAD POR SAIDA, e nao uma por range. Uma por range serializa a cena plana inteira numa
// unica thread, porque a cena plana emite exatamente um range. A thread descobre a que range
// pertence por busca binaria sobre os offsets de saida acumulados, que a CPU ja calcula ao montar
// os ranges (sao poucos; o prefixo na GPU so passa a valer a pena quando o produtor for por
// celula).
//
// O range endereca a LISTA, nunca um intervalo de slots. Ver a nota do D5 no plano.

#include "../Scene/GpuSceneTables.hlsli"

struct GpuInstanceRange {
    uint FirstCandidate; // indice em CandidateIds
    uint CandidateCount;
};

// Contadores. Indices fixos, porque o consumidor e a CPU e um enum compartilhado seria um terceiro
// espelho a manter em dia.
#define SMILE_EXPAND_COUNTER_EXPANDED     0u // slots escritos na saida
#define SMILE_EXPAND_COUNTER_INVALID_SLOT 1u // slot fora da tabela, ou linha sem ocupante
#define SMILE_EXPAND_COUNTER_RT_ONLY      2u // linha marcada RaytracingOnly: o produtor falhou
#define SMILE_EXPAND_COUNTER_OVERFLOW     3u // nao coube na capacidade da saida
#define SMILE_EXPAND_COUNTER_COUNT        4u

// Marca uma posicao de saida que nao recebeu candidato. Nao e zero: zero e um slot legitimo.
#define SMILE_EXPAND_NO_SLOT 0xFFFFFFFFu

cbuffer ExpandCB : register(b0) {
    uint4 ExpandParams;  // x=total de saidas, y=ranges, z=capacidade da saida, w=capacidade da meta
};

StructuredBuffer<GpuInstanceRange> CandidateRanges  : register(t0);
// Offset de saida ACUMULADO de cada range (prefixo exclusivo). Vem da CPU junto dos ranges: a
// alternativa e um dispatch de prefixo so para o caso de um range.
StructuredBuffer<uint>             RangeOutputBase  : register(t1);
StructuredBuffer<uint>             CandidateIds     : register(t2);
StructuredBuffer<GpuInstanceMeta>  InstanceMeta     : register(t3);

RWStructuredBuffer<uint> ExpandedSlots : register(u0);
RWStructuredBuffer<uint> Counters      : register(u1);

// Ultimo range cujo offset de saida e <= Index. Os offsets sao estritamente nao-decrescentes e o
// primeiro e zero, entao a busca sempre acha um.
uint FindRange(uint Index, uint RangeCount) {
    uint Lo = 0;
    uint Hi = RangeCount - 1u;
    while (Lo < Hi) {
        const uint Mid = (Lo + Hi + 1u) / 2u;
        if (RangeOutputBase[Mid] <= Index) Lo = Mid;
        else                               Hi = Mid - 1u;
    }
    return Lo;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint Total       = ExpandParams.x;
    const uint RangeCount  = ExpandParams.y;
    const uint OutCapacity = ExpandParams.z;
    const uint MetaCount   = ExpandParams.w;

    const uint Index = dtid.x;
    if (Index >= Total || RangeCount == 0u) return;

    // Sentinela ANTES de qualquer saida antecipada. Uma posicao nao escrita carregaria o slot do
    // frame anterior, que e um candidato PLAUSIVEL e errado — o pior desfecho possivel para quem
    // le esta lista sem saber que ela tem buraco.
    if (Index >= OutCapacity) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_OVERFLOW], 1u);
        return;
    }
    ExpandedSlots[Index] = SMILE_EXPAND_NO_SLOT;

    const uint RangeIdx = FindRange(Index, RangeCount);
    const GpuInstanceRange Range = CandidateRanges[RangeIdx];
    const uint Local = Index - RangeOutputBase[RangeIdx];
    if (Local >= Range.CandidateCount) {
        // Offsets incoerentes com as contagens. Nao inventa candidato: o buraco fica marcado e
        // conta como invalido, que e o que a regra 6 do plano pede — desenhar demais e aceitavel,
        // desenhar o objeto ERRADO nao.
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_INVALID_SLOT], 1u);
        return;
    }

    const uint Slot = CandidateIds[Range.FirstCandidate + Local];

    // Validacao de slot e de generation.
    //
    // O que ela PROVA: o slot cabe na tabela e a linha tem ocupante (generation 0 e reservada para
    // linha vazia). O que ela NAO prova: que o ocupante e o mesmo de quando a lista foi montada —
    // para isso a lista teria de carregar a generation esperada. Hoje isso e coberto por
    // construcao e nao por checagem: todo evento que recicla um slot passa por Add/Remove, que
    // bumpam a versao de ESTRUTURA, e essa versao reconstroi a lista.
    if (Slot >= MetaCount) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_INVALID_SLOT], 1u);
        return;
    }
    const GpuInstanceMeta Meta = InstanceMeta[Slot];
    if (Meta.Generation == 0u) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_INVALID_SLOT], 1u);
        return;
    }
    // Verificacao CRUZADA da exclusao que o produtor CPU aplica. Se ela falhar, o terreno esta
    // prestes a ser desenhado duas vezes — e este contador e o unico lugar em que isso aparece
    // antes de virar custo silencioso.
    if ((Meta.Flags & SMILE_INSTANCE_FLAG_RT_ONLY) != 0u) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_RT_ONLY], 1u);
        return;
    }

    // Escrita POSICIONAL, e nao append atomico.
    //
    // Esta fase nao rejeita ninguem, entao a expansao e 1:1 e a posicao de saida ja e conhecida:
    // e o proprio Index. Isso deixa a lista da GPU byte a byte comparavel com a referencia CPU,
    // que e o gate "range unico e multiplos ranges sinteticos produzem a mesma lista". Um append
    // por InterlockedAdd devolveria a mesma lista EMBARALHADA, e o gate viraria comparacao de
    // multiconjunto — mais fraca, e por nada, porque a compactacao so ganha sentido quando houver
    // rejeicao de verdade (Fase 6).
    ExpandedSlots[Index] = Slot;
    InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_EXPANDED], 1u);
}
