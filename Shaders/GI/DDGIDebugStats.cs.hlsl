#define DDGI_RAYS 64

// So pelo DDGI_TraceTexel: o ProbesTrace deixou de ser uma linha por sonda (ver DDGICommon), e
// este passe le exatamente os mesmos texels que o update le.
#include "DDGICommon.hlsli"

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;
    float4 GridCount;       
    float4 AtlasParams;
    float4 DistAtlasParams;
    float4 DebugParams;
    float4 CameraPos;
};

Texture2D<float4> ProbesTrace : register(t0);
RWBuffer<float4>  ProbeStats  : register(u0);

[numthreads(DDGI_RAYS, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int probeIdx  = (int)DTid.x;
    int numProbes = (int)GridCount.w;
    if (probeIdx >= numProbes) return;

    int   backCount = 0;
    int   realCount = 0;
    float minFront  = DistAtlasParams.w; 
    float sumFront  = 0.0f;
    int   frontCount = 0;

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float d = ProbesTrace[DDGI_TraceTexel(probeIdx, r, numProbes, DDGI_RAYS)].a;
        // Raios nao escolhidos pelo adaptive tracing usam este sentinel e nao podem entrar
        // nem no denominador nem como backface.
        if (d < -1e8f) continue;
        realCount++;
        if (d < 0.0f) {
            backCount++;
        } else {
            minFront = min(minFront, d);
            sumFront += d;
            frontCount++;
        }
    }

    float backRatio = realCount > 0 ? (float)backCount / (float)realCount : 0.0f;
    float meanFront = frontCount > 0 ? (sumFront / (float)frontCount) : 0.0f;
    float state     = backRatio > 0.25f ? 0.0f : 1.0f; 

    ProbeStats[probeIdx] = float4(backRatio, minFront, meanFront, state);
}
