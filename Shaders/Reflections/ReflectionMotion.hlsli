#ifndef SMILE_REFLECTION_MOTION
#define SMILE_REFLECTION_MOTION

#include "../Temporal/TemporalMotionCommon.hlsli"

// Vetor estocastico do sec. 25.3.2. O ponto secundario e levado para o frame anterior,
// espelhado no plano receptor anterior e observado a partir da camera anterior. Para GGX
// isotropico, a intersecao do lobo e aproximada por uma gaussiana 2D no plano.
float4 ComputeGlossyMotion(uint2 px, float3 receiverPos, float3 receiverNormal,
                           float3 secondaryHit, uint secondaryInstance,
                           float roughness, uint randomStream) {
    if (DebugParams.z < 0.5f) return 0.0f; // surface history ainda nao existe
    const uint instanceCount = (uint)PrevCameraPos.w;
    uint receiverInstance;
    if (!TemporalDecodeInstance(TemporalSurface.Load(int3(px, 0)).w,
                                instanceCount, receiverInstance) ||
        secondaryInstance >= instanceCount)
        return 0.0f;

    const float3 prevS = TemporalTransformPoint(
        receiverPos, TemporalTransforms[receiverInstance].CurrentToPrevious);
    const float3 prevN = TemporalTransformDirection(
        receiverNormal, TemporalTransforms[receiverInstance].CurrentToPrevious);
    const float3 prevH = TemporalTransformPoint(
        secondaryHit, TemporalTransforms[secondaryInstance].CurrentToPrevious);

    const float3 virtualH = prevH - 2.0f * prevN * dot(prevH - prevS, prevN);
    const float3 viewRay = virtualH - PrevCameraPos.xyz;
    const float denom = dot(viewRay, prevN);
    if (abs(denom) <= 1.0e-5f) return 0.0f;
    const float t = dot(prevS - PrevCameraPos.xyz, prevN) / denom;
    if (t <= 0.0f) return 0.0f;
    float3 samplePoint = PrevCameraPos.xyz + viewRay * t;

    if (roughness >= 0.05f) {
        const float2 u = max(GGX_Rand2E(px, (uint)TraceParams.x, randomStream), 1.0e-5f);
        const float radius = sqrt(-2.0f * log(u.x));
        const float angle = 6.28318530718f * u.y;
        const float2 gaussian = radius * float2(cos(angle), sin(angle));
        const float3 tangent = normalize(abs(prevN.z) < 0.999f
            ? cross(prevN, float3(0.0f, 0.0f, 1.0f))
            : cross(prevN, float3(0.0f, 1.0f, 0.0f)));
        const float3 bitangent = cross(prevN, tangent);
        const float sigma = min(roughness * roughness * max(length(prevH - prevS), 0.1f),
                                max(length(samplePoint - PrevCameraPos.xyz) * 0.25f, 0.01f));
        samplePoint += (tangent * gaussian.x + bitangent * gaussian.y) * sigma;
    }

    float2 prevUv;
    if (!TemporalProjectUv(samplePoint, PrevViewProj, prevUv)) return 0.0f;
    const int2 previousPx = clamp(int2(prevUv * ScreenParams.xy), int2(0, 0),
                                  int2(ScreenParams.xy) - 1);
    const float4 actualPrevious = PrevTemporalSurface.Load(int3(previousPx, 0));
    uint previousInstance;
    if (!TemporalDecodeInstance(actualPrevious.w, instanceCount, previousInstance)) return 0.0f;

    const float planeDistance = abs(dot(actualPrevious.xyz - prevS, prevN));
    const float planeSigma = max(0.02f * length(prevS - PrevCameraPos.xyz), 0.01f);
    const float confidence = exp(-0.5f * planeDistance * planeDistance /
                                 (planeSigma * planeSigma));
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    return float4(uv - prevUv, confidence, 1.0f);
}

#endif
