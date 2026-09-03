#ifndef SMILE_TEMPORAL_MOTION_COMMON
#define SMILE_TEMPORAL_MOTION_COMMON

struct FTemporalInstanceTransform {
    row_major float4x4 CurrentToPrevious;
    row_major float4x4 PreviousToCurrent;
};

// Maior id que sobrevive ao round-trip pelo canal float da superficie.
//
// O valor gravado e (id + 1) num float32, que representa inteiros exatamente ate 2^24. O teto
// aqui e 2^24 - 2 para o id, ou seja 2^24 - 1 para o valor gravado. Quem constroi a TLAS recusa
// contagens acima disso: sem essa recusa, o excesso nao daria erro -- daria o OBJETO ERRADO, em
// silencio.
#define SMILE_TEMPORAL_MAX_INSTANCE_ID 16777214u

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
