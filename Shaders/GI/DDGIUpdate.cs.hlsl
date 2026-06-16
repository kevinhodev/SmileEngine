#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 
#define DDGI_TILE 6

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing; 
    float4 GridCountRays;  
    float4 AtlasParams;     
    float4 SunDirIntensity;
    float4 SunColorHyst;    
    float4 TraceParams;   
};

Texture2D<float4>   ProbesTrace : register(t0);
RWTexture2D<float4> IrradAtlas  : register(u0);

[numthreads(DDGI_TILE, DDGI_TILE, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int probeIdx  = (int)Gid.x;
    int numProbes = (int)AtlasParams.w;
    if (probeIdx >= numProbes) return;

    int3 count = (int3)GridCountRays.xyz;
    int3 pc    = DDGI_ProbeCoord(probeIdx, count);
    int  tile  = (int)AtlasParams.x; 
    int2 tileOrigin = DDGI_TileOrigin(pc, count, tile);
    int2 local      = int2(GTid.xy);

    float2 octUV = ((float2)local + 0.5f) / (float)tile; 
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    uint   frame = (uint)TraceParams.x;
    float3 sum   = float3(0.0f, 0.0f, 0.0f);
    float  wsum  = 0.0f;

    int  backfaceCount = 0;
    int  realCount     = 0; 

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float4 tr = ProbesTrace[int2(r, probeIdx)];
        if (tr.a < -1e8f) continue; 
        ++realCount;
        if (tr.a < 0.0f) { ++backfaceCount; continue; } 
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame);
        float  w    = max(0.0f, dot(texelDir, rdir));
        if (w <= 0.0f) continue;
        sum  += tr.rgb * w;
        wsum += w;
    }

    bool occluded = (realCount > 0) && (backfaceCount > (int)(realCount * 0.35f));
    float3 result = (occluded || wsum <= 0.0f) ? float3(0.0f, 0.0f, 0.0f) : (sum / wsum);
    result = pow(max(result, 0.0f), 1.0f / DDGI_IRRADIANCE_GAMMA);

    int2   texel = tileOrigin + local;
    float3 prev  = IrradAtlas[texel].rgb; 
    
    float  hyst    = SunColorHyst.w;
    float3 blended = lerp(result, prev, hyst);
    IrradAtlas[texel] = float4(blended, 1.0f);
}
