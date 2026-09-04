#ifndef SMILE_TEMPORAL_MOTION_COMMON
#define SMILE_TEMPORAL_MOTION_COMMON

struct FTemporalInstanceTransform {
    row_major float4x4 CurrentToPrevious;
    row_major float4x4 PreviousToCurrent;
};

// Maior id que sobrevive ao round-trip pelo canal float da superficie.
//
// O valor gravado e (id + 1) num float32, que representa inteiros exatamente ate 2^24 INCLUSIVE --
// entao o maior ID e 2^24 - 1 (0xFFFFFF), cujo encoded e exatamente 2^24. E o mesmo teto do
// InstanceID do DXR, que tem 24 bits.
//
// Quem constroi a TLAS recusa contagens acima de 2^24 (ver kMaxTemporalInstanceCount, no lado da
// CPU): sem essa recusa, o excesso nao daria erro -- daria o OBJETO VIZINHO, em silencio.
#define SMILE_TEMPORAL_MAX_INSTANCE_ID 16777215u

bool TemporalDecodeInstance(float encoded, uint instanceCount, out uint instanceId) {
    // Sem o "+ 0.5f" que estava aqui. Ele parecia arredondamento defensivo e na pratica
    // CORTAVA A FAIXA PELA METADE: a soma em float32 nao e representavel a partir de 2^23,
    // entao o id 8388608 decodificava como 8388609 -- o objeto vizinho, sem erro nenhum.
    // Medido nas duas bordas (2^23 e 2^24 - 2).
    //
    // A defesa nao era necessaria: o valor vem de um Load (sem filtragem) de um canal que
    // recebeu um inteiro exato, entao truncar ja e exato.
    const uint value = (uint)encoded;
    instanceId = value > 0u ? value - 1u : 0u;
    return value > 0u && instanceId < instanceCount;
}

// Entrada da tabela de identidades. MESMO layout do FGpuIdentityEntry no C++, onde ha
// static_assert de tamanho E de offset -- trocar a ordem aqui nao daria erro de compilacao,
// daria dados trocados.
struct FGpuIdentityEntry {
    uint Slot;
    uint Generation;
};

// Traduz um id gravado no frame ANTERIOR para o indice do MESMO objeto no frame ATUAL.
//
// Sem isto, um id antigo era reinterpretado pela lista de agora: com [A,B,C], remover A faz o
// id 1 (que era B) passar a designar C, e o resultado e o transform do objeto errado -- sem
// erro, so movimento errado.
//
// A cadeia inteira: id anterior -> tabela ANTERIOR -> {slot,generation} -> remap -> indice
// atual -> confirmacao contra a tabela ATUAL. O ultimo passo e o que separa "o slot existe" de
// "e o mesmo objeto": um slot reciclado devolve indice valido com OUTRO ocupante, e so a
// generation reprova isso.
bool TemporalResolvePreviousIndex(uint previousId,
                                  StructuredBuffer<FGpuIdentityEntry> previousTable,
                                  StructuredBuffer<uint>              slotToCurrentIndex,
                                  StructuredBuffer<FGpuIdentityEntry> currentTable,
                                  uint slotCapacity,
                                  uint currentCount,
                                  out uint currentIndex) {
    currentIndex = 0u;

    // Generation 0 nunca e alocada (ver GpuSceneHandle.cpp), entao entrada zerada = vazia. E o
    // que a tabela guarda para as posicoes alem da contagem viva.
    const FGpuIdentityEntry was = previousTable[previousId];
    if (was.Generation == 0u || was.Slot >= slotCapacity) return false;

    const uint candidate = slotToCurrentIndex[was.Slot];
    if (candidate >= currentCount) return false;

    const FGpuIdentityEntry now = currentTable[candidate];
    if (now.Slot != was.Slot || now.Generation != was.Generation) return false;

    currentIndex = candidate;
    return true;
}

float3 TemporalTransformPoint(float3 p, row_major float4x4 m) {
    const float4 h = mul(float4(p, 1.0f), m);
    return h.xyz / max(abs(h.w), 1.0e-6f);
}

float3 TemporalTransformDirection(float3 v, row_major float4x4 m) {
    return normalize(mul(float4(v, 0.0f), m).xyz);
}

bool TemporalProjectUv(float3 p, row_major float4x4 vp, out float2 uv) {
    const float4 clip = mul(float4(p, 1.0f), vp);
    if (clip.w <= 1.0e-5f) { uv = 0.0f; return false; }
    const float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return all(uv > 0.0f) && all(uv < 1.0f);
}

#endif
