// ReSTIR DI - Pass B: reuso espacial enviesado + resolve de visibilidade (no maximo 1 raio/pixel).
// O reservoir final NAO e gravado de volta no historico: este marco prefere nao realimentar o
// vies espacial. A/B posterior pode habilitar o caminho do Alg. 5 do paper quando houver medicao.

#include "../GBuffer.hlsli"
#include "../BRDF.hlsli"
#include "../LightsCommon.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "../GI/DDGICommon.hlsli"
#include "ReSTIRDICommon.hlsli"

cbuffer ReSTIRDICB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params;       // x=lightCount, y=frameIndex, z=shadowMask, w=rayEndMargin
    float4 Sampling;     // x=initialCandidates, y=MCap, z=spatialCount, w=spatialRadius
    float4 Reuse;        // x=temporal, y=posRejectScale, z=normalReject, w=maxAge
    float4 TemporalPolicy; // mantem o layout de b0 identico ao Pass A
    float4 RayEpsA;
    float4 RayEpsB;
    row_major float4x4 View; // layout comum; consumido pelo pack do NRD
    row_major float4x4 PrevViewProj;
};

#include "../RayOffset.hlsli"

Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float>  Depth    : register(t3);
RaytracingAccelerationStructure Scene : register(t4);
StructuredBuffer<InstanceGeo> Instances : register(t5);
Texture2D<float4> ResA : register(t6);
Texture2D<float4> ResB : register(t7);
StructuredBuffer<FGPULightFull> Lights : register(t8);
#include "../Temporal/TemporalMotionCommon.hlsli"
StructuredBuffer<FTemporalInstanceTransform> TemporalTransforms : register(t9);
Texture2D<float4> CurrentSurface : register(t10);
Texture2D<float4> PreviousSurface : register(t11);

SamplerState LinearWrap : register(s1);
RWTexture2D<float4> OutDirect : register(u0);
RWTexture2D<float4> OutDiffuse : register(u1);
RWTexture2D<float4> OutSpecular : register(u2);
RWTexture2D<float4> OutShadowMotion : register(u3); // xy=curUV-prevUV, z=confianca, w=valido

#include "../GI/RTAlphaTest.hlsli"

float4 ComputeShadowMotion(int2 px, float3 receiverPos, float3 receiverNormal,
                           float3 blockerPos, uint blockerId, FGPULightFull light) {
    if (TemporalPolicy.y < 0.5f) return 0.0f;
    const uint instanceCount = (uint)CameraPos.w;
    uint receiverId;
    if (!TemporalDecodeInstance(CurrentSurface.Load(int3(px, 0)).w,
                                instanceCount, receiverId) || blockerId >= instanceCount)
        return 0.0f;

    const float3 prevReceiver = TemporalTransformPoint(
        receiverPos, TemporalTransforms[receiverId].CurrentToPrevious);
    const float3 prevNormal = TemporalTransformDirection(
        receiverNormal, TemporalTransforms[receiverId].CurrentToPrevious);
    const float3 prevBlocker = TemporalTransformPoint(
        blockerPos, TemporalTransforms[blockerId].CurrentToPrevious);
    const float3 prevLight = light.PrevPosInvRadius.xyz;

    const float3 blockerLine = prevBlocker - prevLight;
    const float denom = dot(prevNormal, blockerLine);
    if (abs(denom) <= 1.0e-5f) return 0.0f;
    const float t = dot(prevNormal, prevReceiver - prevLight) / denom;
    if (t <= 0.0f) return 0.0f;
    const float3 intersection = prevLight + blockerLine * t;

    float2 prevUv;
    if (!TemporalProjectUv(intersection, PrevViewProj, prevUv)) return 0.0f;

    const int2 prevPx = clamp(int2(prevUv * ScreenParams.xy), int2(0, 0),
                              int2(ScreenParams.xy) - 1);
    const float4 previous = PreviousSurface.Load(int3(prevPx, 0));
    uint previousId;
    if (!TemporalDecodeInstance(previous.w, instanceCount, previousId)) return 0.0f;

    // Eq. 25.4: confianca maxima quando o ponto real anterior se desloca no plano virtual
    // (theta ~= pi/2). A forma gaussiana reduz o peso em receptores curvos/descontinuos.
    const float3 delta = previous.xyz - prevReceiver;
    const float deltaLen = length(delta);
    float confidence = 1.0f;
    if (deltaLen > 1.0e-5f) {
        const float theta = acos(clamp(dot(delta / deltaLen, prevNormal), -1.0f, 1.0f));
        const float angleError = theta - 1.57079632679f;
        confidence = exp(-0.5f * angleError * angleError / (0.1f * 0.1f));
    }
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    return float4(uv - prevUv, confidence, 1.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 upx = dtid.xy;
    if (upx.x >= (uint)ScreenParams.x || upx.y >= (uint)ScreenParams.y) return;
    const int2 px = int2(upx);

    const float deviceZ = Depth.Load(int3(px, 0));
    const uint lightCount = (uint)Params.x;
    if (deviceZ <= 0.0f || lightCount == 0u) {
        OutDirect[px] = OutDiffuse[px] = OutSpecular[px] = 0.0f;
        OutShadowMotion[px] = 0.0f;
        return;
    }

    const GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                        GBufferB.Load(int3(px, 0)),
                                        GBufferC.Load(int3(px, 0)));
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 x1 = wh.xyz / wh.w;
    const float3 n1 = g.WorldNormal;

    uint rng = GGX_SeedE(upx, (uint)Params.y, SMILE_RNG_DI_SPATIAL);
    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = x1;
    r.N1Oct = DDGI_OctEncode(n1);

    ReSTIRDIReservoir self = DI_LoadReservoir(ResA.Load(int3(px, 0)), ResB.Load(int3(px, 0)));
    self.M = min(self.M, Sampling.y);
    if (self.LightIndex < lightCount && self.M > 0.0f && self.W > 0.0f) {
        float3 diff, spec, L; float dist;
        const float target = DI_Evaluate(Lights[self.LightIndex], g, x1, CameraPos.xyz,
                                         diff, spec, L, dist);
        DIResMerge(r, self, target, rng);
    }

    const int K = min((int)Sampling.z, 8);
    const float posReject = Reuse.y * max(length(CameraPos.xyz - x1), 1.0f);
    [loop]
    for (int i = 0; i < K; ++i) {
        const float2 xi = GGX_Rand2E(upx, (uint)Params.y, SMILE_RNG_DI_SPATIAL_TAP + (uint)i);
        const int2 qpx = px + int2(round(GGX_ConcentricDisk(xi) * Sampling.w));
        if (qpx.x < 0 || qpx.y < 0 || qpx.x >= (int)ScreenParams.x || qpx.y >= (int)ScreenParams.y ||
            all(qpx == px)) continue;

        const float qz = Depth.Load(int3(qpx, 0));
        if (qz <= 0.0f) continue;
        const GBufferData qg = DecodeGBuffer(GBufferA.Load(int3(qpx, 0)),
                                             GBufferB.Load(int3(qpx, 0)),
                                             GBufferC.Load(int3(qpx, 0)));
        if (dot(qg.WorldNormal, n1) < Reuse.z) continue;

        ReSTIRDIReservoir nb = DI_LoadReservoir(ResA.Load(int3(qpx, 0)), ResB.Load(int3(qpx, 0)));
        if (nb.LightIndex >= lightCount || nb.M <= 0.0f || nb.W <= 0.0f) continue;
        if (length(nb.X1 - x1) >= posReject ||
            abs(dot(n1, nb.X1 - x1)) >= 0.2f * posReject) continue;
        nb.M = min(nb.M, Sampling.y);

        float3 diff, spec, L; float dist;
        const float target = DI_Evaluate(Lights[nb.LightIndex], g, x1, CameraPos.xyz,
                                         diff, spec, L, dist);
        DIResMerge(r, nb, target, rng);
    }

    if (r.LightIndex >= lightCount) {
        OutDirect[px] = OutDiffuse[px] = OutSpecular[px] = 0.0f;
        OutShadowMotion[px] = 0.0f;
        return;
    }

    const FGPULightFull selected = Lights[r.LightIndex];
    float3 diffuse, specular, L; float dist;
    const float selectedTarget = DI_Evaluate(selected, g, x1, CameraPos.xyz,
                                             diffuse, specular, L, dist);
    DIResFinalize(r, selectedTarget);
    float visibility = 1.0f;
    float4 shadowMotion = 0.0f;

    if (r.W > 0.0f && DI_IsShadowCaster(selected)) {
        const float camDist = length(CameraPos.xyz - x1);
        const float3 origin = OffsetRayGBuffer(x1, n1, L, camDist);
        const float3 toLight = selected.PosInvRadius.xyz - origin;
        const float len = length(toLight);
        const float tMax = len - max(Params.w, 0.0f);
        if (len > 1e-6f && tMax > RayEpsB.x) {
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = toLight / len;
            ray.TMin = RayEpsB.x;
            ray.TMax = tMax;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
            query.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                 (uint)Params.z, ray);
            SMILE_RT_PROCEED(query)
            if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                visibility = 0.0f;
                const float3 blockerPos = ray.Origin + ray.Direction * query.CommittedRayT();
                shadowMotion = ComputeShadowMotion(px, x1, n1, blockerPos,
                                                   query.CommittedInstanceID(), selected);
            }
        }
    }

    const float3 diffuseEstimate = diffuse * (r.W * visibility);
    const float3 specularEstimate = specular * (r.W * visibility);
    // Igual ao RTXDI: hit distance zero quando a amostra nao entrega radiancia valida. Isso evita
    // ensinar ao RELAX uma superficie virtual atraves de um bloqueador ou de um reservoir vazio.
    const float hitDist = (visibility > 0.0f && r.W > 0.0f &&
                           any(diffuseEstimate + specularEstimate > 0.0f)) ? dist : 0.0f;
    OutDirect[px]  = float4(diffuseEstimate + specularEstimate, hitDist);
    OutDiffuse[px] = float4(diffuseEstimate, hitDist);
    OutSpecular[px] = float4(specularEstimate, hitDist);
    OutShadowMotion[px] = shadowMotion;
}
