// Reconstrução temporal half-res -> full-res em quatro fases 2x2.
// Profundidade e normal rejeitam histórico; o bilateral cobre pixels inválidos.

#include "DDGICommon.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 ShadeParams;
    float4 ReuseParams;
    float4 SpatialParams;
    float4 JitterParams;
    row_major float4x4 View;
    float4 RayEpsA;
    float4 RayEpsB;
    float4 PolicyParams;
    float4 GIDistParams;
    float4 GIBiasParams;
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;
    float4 DebugParams;
    float4 HistoryParams;
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 TraceScreenParams;
    float4 ResolutionParams;
};

Texture2D<float4> GIHalf      : register(t0);
Texture2D<float>  Depth       : register(t1);
Texture2D<float4> GBuffer     : register(t2);
Texture2D<float2> Velocity    : register(t3);
Texture2D<float4> GIPrevious  : register(t4);
RWTexture2D<float4> GIResolved : register(u0);

float3 WorldFromDepth(uint2 p, float deviceZ) {
    const float2 uv = (float2(p) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wh.xyz / wh.w;
}

uint2 FullPixelFromTrace(int2 tracePx) {
    const uint scale = max((uint)ResolutionParams.x, 1u);
    return min(uint2(tracePx) * scale + uint2(ResolutionParams.yz),
               uint2(ScreenParams.xy) - 1u);
}

float4 LoadPreviousSurface(int2 p) {
    Texture2D<float4> previousSurface =
        ResourceDescriptorHeap[NonUniformResourceIndex((uint)HistoryParams.x)];
    return previousSurface.Load(int3(clamp(p, int2(0, 0), int2(ScreenParams.xy) - 1), 0));
}

float3 CurrentGeometricNormal(int2 p, float3 shadingNormal) {
    const int2 limit = int2(ScreenParams.xy) - 1;
    const int2 px0 = int2(max(p.x - 1, 0), p.y);
    const int2 px1 = int2(min(p.x + 1, limit.x), p.y);
    const int2 py0 = int2(p.x, max(p.y - 1, 0));
    const int2 py1 = int2(p.x, min(p.y + 1, limit.y));
    const float zx0 = Depth.Load(int3(px0, 0));
    const float zx1 = Depth.Load(int3(px1, 0));
    const float zy0 = Depth.Load(int3(py0, 0));
    const float zy1 = Depth.Load(int3(py1, 0));
    if (min(min(zx0, zx1), min(zy0, zy1)) <= 0.0f) return shadingNormal;
    const float3 dx = WorldFromDepth(px1, zx1) - WorldFromDepth(px0, zx0);
    const float3 dy = WorldFromDepth(py1, zy1) - WorldFromDepth(py0, zy0);
    float3 n = cross(dx, dy);
    const float lengthSquared = dot(n, n);
    if (lengthSquared <= 1.0e-10f) return shadingNormal;
    n *= rsqrt(lengthSquared);
    return dot(n, shadingNormal) < 0.0f ? -n : n;
}

float3 PreviousGeometricNormal(int2 p, out bool valid) {
    const int2 limit = int2(ScreenParams.xy) - 1;
    const int2 px0 = int2(max(p.x - 1, 0), p.y);
    const int2 px1 = int2(min(p.x + 1, limit.x), p.y);
    const int2 py0 = int2(p.x, max(p.y - 1, 0));
    const int2 py1 = int2(p.x, min(p.y + 1, limit.y));
    const float4 sx0 = LoadPreviousSurface(px0);
    const float4 sx1 = LoadPreviousSurface(px1);
    const float4 sy0 = LoadPreviousSurface(py0);
    const float4 sy1 = LoadPreviousSurface(py1);
    valid = min(min(sx0.w, sx1.w), min(sy0.w, sy1.w)) > 0.0f;
    float3 n = cross(sx1.xyz - sx0.xyz, sy1.xyz - sy0.xyz);
    const float lengthSquared = dot(n, n);
    valid = valid && lengthSquared > 1.0e-10f;
    return valid ? n * rsqrt(lengthSquared) : float3(0.0f, 0.0f, 1.0f);
}

float4 SpatialFallback(uint2 px, float z, float4 gb) {
    const float3 n = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    const float rough = gb.b;
    const float3 world = WorldFromDepth(px, z);
    const float cameraDepth = length(world - CameraPos.xyz);
    const float scale = max(ResolutionParams.x, 1.0f);
    const float2 hf = (float2(px) - ResolutionParams.yz) / scale;
    const int2 h0 = int2(floor(hf));
    const float2 f = frac(hf);
    const int2 taps[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
    const float bilinear[4] = {
        (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
        (1.0f - f.x) * f.y, f.x * f.y
    };

    const int2 traceMax = int2(TraceScreenParams.xy) - 1;
    float4 sum = 0.0f;
    float weightSum = 0.0f;
    float4 nearest = 0.0f;
    float nearestScore = 1e30f;
    [unroll] for (int i = 0; i < 4; ++i) {
        const int2 hp = clamp(h0 + taps[i], int2(0, 0), traceMax);
        const uint2 sourcePx = FullPixelFromTrace(hp);
        const float sourceZ = Depth.Load(int3(sourcePx, 0));
        if (sourceZ <= 0.0f) continue;

        const float4 sourceGb = GBuffer.Load(int3(sourcePx, 0));
        const float3 sourceN = DDGI_OctDecode(sourceGb.rg * 2.0f - 1.0f);
        const float sourceDepth = length(WorldFromDepth(sourcePx, sourceZ) - CameraPos.xyz);
        const float depthDelta = abs(sourceDepth - cameraDepth);
        const float depthTolerance = max(0.05f, cameraDepth * 0.02f);
        const float normalAgreement = saturate(dot(n, sourceN));
        const float roughDelta = abs(sourceGb.b - rough);
        const float bilateral = exp2(-4.0f * depthDelta / depthTolerance) *
                                pow(normalAgreement, 24.0f) * exp2(-8.0f * roughDelta);
        const float w = bilinear[i] * bilateral;
        const float4 gi = GIHalf.Load(int3(hp, 0));
        sum += gi * w;
        weightSum += w;

        const float score = depthDelta / depthTolerance +
                            (1.0f - normalAgreement) * 4.0f + roughDelta;
        if (score < nearestScore) { nearestScore = score; nearest = gi; }
    }
    return (weightSum > 1e-5f) ? (sum / weightSum) : nearest;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const float z = Depth.Load(int3(px, 0));
    if (z <= 0.0f) { GIResolved[px] = 0.0f; return; }

    const float4 gb = GBuffer.Load(int3(px, 0));
    const uint2 phase = uint2(ResolutionParams.yz);
    const bool freshPhase = ((px.x & 1u) == phase.x) && ((px.y & 1u) == phase.y);
    if (freshPhase) {
        GIResolved[px] = GIHalf.Load(int3(px >> 1u, 0));
        return;
    }

    if (HistoryParams.y > 0.5f) {
        const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
        const float2 previousUv = uv - Velocity.Load(int3(px, 0));
        if (all(previousUv > 0.0f) && all(previousUv < 1.0f)) {
            const int2 previousPx = clamp(int2(previousUv * ScreenParams.xy), int2(0, 0),
                                          int2(ScreenParams.xy) - 1);
            const float4 previousSurface = LoadPreviousSurface(previousPx);
            if (previousSurface.w > 0.0f) {
                const float3 shadingNormal = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
                const float3 currentNormal = CurrentGeometricNormal(int2(px), shadingNormal);
                bool previousNormalValid;
                const float3 previousNormal = PreviousGeometricNormal(previousPx,
                                                                        previousNormalValid);
                const float3 currentWorld = WorldFromDepth(px, z);
                const float3 delta = previousSurface.xyz - currentWorld;
                const float cameraDepth = length(currentWorld - CameraPos.xyz);
                const float positionTolerance = max(0.05f, cameraDepth * 0.02f);
                const float planeTolerance = max(0.03f, cameraDepth * 0.005f);
                const bool depthValid = length(delta) <= positionTolerance &&
                                        abs(dot(currentNormal, delta)) <= planeTolerance;
                const bool normalValid = previousNormalValid &&
                                         abs(dot(currentNormal, previousNormal)) >= 0.80f;
                if (depthValid && normalValid) {
                    GIResolved[px] = GIPrevious.Load(int3(previousPx, 0));
                    return;
                }
            }
        }
    }

    // Fallback até a fase exata atualizar o pixel.
    GIResolved[px] = SpatialFallback(px, z, gb);
}
