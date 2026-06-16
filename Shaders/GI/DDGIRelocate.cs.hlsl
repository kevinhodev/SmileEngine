#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 
#define RELOCATE_GROUP 64 

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;
    float4 GridCountRays;   
    float4 AtlasParams;  
    float4 SunDirIntensity;
    float4 SunColorHyst;
    float4 TraceParams;    
    float4 DistAtlasParams;
    float4 MiscParams;      
};

Texture2D<float4> ProbesTrace  : register(t0);
RWBuffer<float4>  ProbeData     : register(u0); 
RWBuffer<uint>    ProbeRayCount : register(u1); 

uint DDGI_DesiredRays(float closestFront, float spacing, int minRays, int maxRays) {
    float ratio = closestFront / max(spacing, 1e-3f);
    int desired = (ratio < 0.5f) ? 256 : (ratio < 1.0f) ? 128 : (ratio < 2.0f) ? 64
                : (ratio < 4.0f) ? 32  : (ratio < 8.0f) ? 16  : 8;
    return (uint)clamp(desired, minRays, maxRays);
}

[numthreads(RELOCATE_GROUP, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int probeIdx  = (int)DTid.x;
    int numProbes = (int)AtlasParams.w;
    if (probeIdx >= numProbes) return;

    int maxRays = (int)MiscParams.z;
    int minRays = (int)MiscParams.w;

    if (MiscParams.x < 0.5f) {
        ProbeData[probeIdx]     = float4(0.0f, 0.0f, 0.0f, 0.0f);
        ProbeRayCount[probeIdx] = (uint)max(maxRays, 1);
        return;
    }

    float  spacing = GridMinSpacing.w;
    uint   frame   = (uint)TraceParams.x;
    float3 offset  = ProbeData[probeIdx].xyz;

    int    backfaceCount   = 0;
    int    realCount       = 0;
    float  closestFront    = 1e27f, farthestFront = 0.0f;
    float3 closestFrontDir = float3(0.0f, 0.0f, 0.0f);
    float3 farthestFrontDir= float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float  d   = ProbesTrace[int2(r, probeIdx)].a;
        if (d < -1e8f) continue; 
        ++realCount;
        float3 dir = DDGI_RayDirection(r, DDGI_RAYS, frame);
        if (d < 0.0f) {
            backfaceCount++;
        } else {
            if (d < closestFront)  { closestFront  = d; closestFrontDir  = dir; }
            if (d > farthestFront) { farthestFront = d; farthestFrontDir = dir; }
        }
    }

    float backRatio = (realCount > 0) ? (float)backfaceCount / (float)realCount : 0.0f;
    float minFront  = spacing * 0.30f; 
    float maxOff    = spacing * 0.45f; 

    float3 target;
    if (backRatio > 0.25f) {
        target = offset + farthestFrontDir * (farthestFront * 0.5f);
    } else if (closestFront < minFront) {
        target = offset - closestFrontDir * (minFront - closestFront);
    } else {
        target = offset;
    }

    float L = length(target);
    if (L > maxOff) target *= (maxOff / L);

    offset = lerp(offset, target, 0.25f);

    float thresh = MiscParams.y;
    bool inactive = (backRatio > thresh) && (backfaceCount >= 6);
    float w = inactive ? -1.0f : backRatio;
    ProbeData[probeIdx] = float4(offset, w);

    float prox = (realCount > 0) ? closestFront : (spacing * 8.0f); 
    ProbeRayCount[probeIdx] = DDGI_DesiredRays(prox, spacing, minRays, maxRays);
}
