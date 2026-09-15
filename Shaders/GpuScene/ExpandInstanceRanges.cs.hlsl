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

#include "GpuBatchCounters.hlsli"
#include "../Scene/GpuSceneTables.hlsli"

struct GpuInstanceRange {
    uint FirstCandidate; // indice em CandidateIds
    uint CandidateCount;
};

// Espelho de Smile::FGpuCandidateId. A generation viaja com o slot porque sem ela nao ha validacao
// de generation nenhuma: da para perguntar "esta linha tem ALGUM ocupante?", que e outra coisa.
struct GpuCandidateId {
    uint Slot;
    uint Generation; // 0 = invalido, mesma convencao do FGpuSceneHandle
};

// Os indices dos contadores (SMILE_EXPAND_COUNTER_*) moram no GpuBatchCounters.hlsli: a finalizacao da
// cadeia os le, e um candidato recusado aqui invalida o frame la.

// Marca uma posicao de saida que nao recebeu candidato. Nao e zero: zero e um slot legitimo.
#define SMILE_EXPAND_NO_SLOT 0xFFFFFFFFu

cbuffer ExpandCB : register(b0) {
    uint4 ExpandParams;  // x=total de saidas, y=ranges, z=capacidade da saida, w=capacidade da meta
    uint4 ExpandParams2; // x=tamanho de CandidateIds; yzw reservados
};

StructuredBuffer<GpuInstanceRange> CandidateRanges  : register(t0);
// Offset de saida ACUMULADO de cada range (prefixo exclusivo). Vem da CPU junto dos ranges: a
// alternativa e um dispatch de prefixo so para o caso de um range.
StructuredBuffer<uint>             RangeOutputBase  : register(t1);
StructuredBuffer<GpuCandidateId>   CandidateIds     : register(t2);
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
    const uint Total          = ExpandParams.x;
    const uint RangeCount     = ExpandParams.y;
    const uint OutCapacity    = ExpandParams.z;
    const uint MetaCount      = ExpandParams.w;
    const uint CandidateCount = ExpandParams2.x;

    const uint Index = dtid.x;
    if (Index >= Total || RangeCount == 0u) return;

    // Sentinela ANTES de qualquer saida antecipada. Uma posicao nao escrita carregaria o slot do
    // frame anterior, que e um candidato PLAUSIVEL e errado — o pior desfecho possivel para quem
    // le esta lista sem saber que ela tem buraco.
    //
    // ⚠️ O overflow aqui e CANARIO, e nao politica. Um shader nao tem como cair no fallback CPU, e
    // uma saida truncada seria geometria sumindo — o que a regra 6 do plano proibe. Quem impede
    // isso e o dimensionamento: a CPU conhece `Stats().Candidates` (que conta inclusive quem nao
    // coube) e aloca a saida por ele. Este contador tem de ser ZERO; diferente de zero, a lista
    // deste frame nao pode ser consumida — e a finalizacao da cadeia zera o count buffer do frame.
    // O mesmo vale para os outros dois canarios deste arquivo: slot invalido e RaytracingOnly.
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

    // Range fora da LISTA, recusado antes do Load.
    //
    // Sem isto, o mesmo range que a referencia CPU ignora produz aqui uma leitura fora do
    // StructuredBuffer. Ela nao trava — o D3D12 devolve zero —, e por isso e pior: o candidato
    // vira o slot 0, que e plausivel, e a GPU passa a discordar da referencia em silencio.
    //
    // A comparacao e por SUBTRACAO no lado seguro (`Count - First`) em vez de somar: a soma pode
    // dar a volta em 32 bits e transformar um range absurdo num que cabe. E o mesmo cuidado que a
    // referencia CPU toma promovendo para u64.
    if (Range.FirstCandidate >= CandidateCount ||
        Range.CandidateCount > CandidateCount - Range.FirstCandidate) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_INVALID_SLOT], 1u);
        return;
    }

    const GpuCandidateId Candidate = CandidateIds[Range.FirstCandidate + Local];

    // Validacao de slot e de generation.
    //
    // A generation ESPERADA vem no proprio candidato. Sem ela so daria para perguntar se a linha
    // tem algum ocupante, e um candidato de uma lista que ficou para tras aceitaria em silencio o
    // ocupante NOVO de um slot reciclado — desenhando o objeto errado com o transform errado.
    //
    // Hoje isso e coberto por construcao (reciclar slot passa por Add/Remove, que bumpam a versao
    // de estrutura e reconstroem a lista), e e por isso que a checagem vale: ela VERIFICA o
    // invariante em vez de presumi-lo.
    // A generation ZERO do candidato e recusada ANTES da comparacao. Sem isso, um candidato
    // invalido (generation 0) apontando para uma linha VAZIA (generation 0 tambem) casaria — dois
    // erros se cancelando num acerto aparente.
    if (Candidate.Slot >= MetaCount || Candidate.Generation == 0u) {
        InterlockedAdd(Counters[SMILE_EXPAND_COUNTER_INVALID_SLOT], 1u);
        return;
    }
    const uint Slot = Candidate.Slot;
    const GpuInstanceMeta Meta = InstanceMeta[Slot];
    if (Meta.Generation != Candidate.Generation) {
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
