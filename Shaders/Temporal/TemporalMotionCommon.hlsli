#ifndef SMILE_TEMPORAL_MOTION_COMMON
#define SMILE_TEMPORAL_MOTION_COMMON

struct FTemporalInstanceTransform {
    row_major float4x4 CurrentToPrevious;
    row_major float4x4 PreviousToCurrent;
};

bool TemporalDecodeInstance(float encoded, uint instanceCount, out uint instanceId) {
    const uint value = (uint)(encoded + 0.5f);
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
