#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 
#define DDGI_DIST_TILE 14
#define DDGI_DIST_SHARP 50.0f

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing; 
    float4 GridCountRays;  
    float4 AtlasParams;     
    float4 SunDirIntensity;
    float4 SunColorHyst;   
    float4 TraceParams;    
    float4 DistAtlasParams; 
};

Texture2D<float4>   ProbesTrace : register(t0);
RWTexture2D<float2> DistAtlas   : register(u0);

[numthreads(DDGI_DIST_TILE, DDGI_DIST_TILE, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int probeIdx  = (int)Gid.x;
    int numProbes = (int)AtlasParams.w;
    if (probeIdx >= numProbes) return;

    int3 count = (int3)GridCountRays.xyz;
    int3 pc    = DDGI_ProbeCoord(probeIdx, count);
    int  tile  = (int)DistAtlasParams.x;
    int2 tileOrigin = DDGI_TileOrigin(pc, count, tile);
    int2 local      = int2(GTid.xy);

    float2 octUV    = ((float2)local + 0.5f) / (float)tile;
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    // Clamp na convencao do paper (G3D): 1.5 * ||spacing do grid|| = 1.5*sqrt(3)*spacing ~= 2.6.
    // Cobre a diagonal da gaiola 2x2x2 (sqrt(3)*spacing) + bias/offset com folga; hits alem disso
    // nao interessam ao Chebyshev (o ponto amostrado esta sempre dentro da gaiola) e so inflavam
    // media/variancia, deixando o teste permissivo demais (leak atraves de parede) com 4.0.
    float distMax = GridMinSpacing.w * 2.6f;

    uint  frame = (uint)TraceParams.x;
    float sumD = 0.0f, sumD2 = 0.0f, wsum = 0.0f;

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float ad = ProbesTrace[int2(r, probeIdx)].a;
        if (ad < -1e8f) continue; 
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame, (uint)probeIdx);
        float  w    = pow(max(0.0f, dot(texelDir, rdir)), DDGI_DIST_SHARP);
        if (w <= 0.0f) continue;

        float d = min(abs(ad), distMax);
        sumD  += w * d;
        sumD2 += w * d * d;
        wsum  += w;
    }

    float2 result = (wsum > 0.0f) ? float2(sumD / wsum, sumD2 / wsum) : float2(distMax, distMax * distMax);

    int2   texel = tileOrigin + local;
    float2 prev  = DistAtlas[texel];
    float  hyst  = SunColorHyst.w;
    DistAtlas[texel] = lerp(result, prev, hyst);
}
