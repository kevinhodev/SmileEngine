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
Buffer<float4>      ProbeData   : register(t1); // w>=1 = probe recem-ativado/relocado
RWTexture2D<float2> DistAtlas   : register(u0);

// Tabela de copia da borda octaedrica (wrap com fold; do Wicked, mesma que o Flax usa):
// uint4(srcX, srcY, dstX, dstY) em coords do tile COM borda (0..15 p/ interior 14).
#define DDGI_BORDER_COUNT 60
static const uint4 kBorderOffsets[DDGI_BORDER_COUNT] = {
    uint4(14,1, 1,0), uint4(13,1, 2,0), uint4(12,1, 3,0), uint4(11,1, 4,0), uint4(10,1, 5,0),
    uint4(9,1, 6,0),  uint4(8,1, 7,0),  uint4(7,1, 8,0),  uint4(6,1, 9,0),  uint4(5,1, 10,0),
    uint4(4,1, 11,0), uint4(3,1, 12,0), uint4(2,1, 13,0), uint4(1,1, 14,0),

    uint4(14,14, 1,15), uint4(13,14, 2,15), uint4(12,14, 3,15), uint4(11,14, 4,15), uint4(10,14, 5,15),
    uint4(9,14, 6,15),  uint4(8,14, 7,15),  uint4(7,14, 8,15),  uint4(6,14, 9,15),  uint4(5,14, 10,15),
    uint4(4,14, 11,15), uint4(3,14, 12,15), uint4(2,14, 13,15), uint4(1,14, 14,15),

    uint4(1,14, 0,1),  uint4(1,13, 0,2),  uint4(1,12, 0,3),  uint4(1,11, 0,4),  uint4(1,10, 0,5),
    uint4(1,9, 0,6),   uint4(1,8, 0,7),   uint4(1,7, 0,8),   uint4(1,6, 0,9),   uint4(1,5, 0,10),
    uint4(1,4, 0,11),  uint4(1,3, 0,12),  uint4(1,2, 0,13),  uint4(1,1, 0,14),

    uint4(14,14, 15,1), uint4(14,13, 15,2), uint4(14,12, 15,3), uint4(14,11, 15,4), uint4(14,10, 15,5),
    uint4(14,9, 15,6),  uint4(14,8, 15,7),  uint4(14,7, 15,8),  uint4(14,6, 15,9),  uint4(14,5, 15,10),
    uint4(14,4, 15,11), uint4(14,3, 15,12), uint4(14,2, 15,13), uint4(14,1, 15,14),

    uint4(14,14, 0,0), uint4(1,14, 15,0), uint4(14,1, 0,15), uint4(1,1, 15,15)
};

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
    // Probe recem-ativado/relocado: distancias antigas sao de outra posicao — reset (hyst 0).
    float  hyst  = (ProbeData[probeIdx].w >= 1.0f) ? 0.0f : SunColorHyst.w;
    DistAtlas[texel] = lerp(result, prev, hyst);

    // Copia a borda de 1px do tile (fold octaedrico) p/ o bilinear ser continuo na costura.
    DeviceMemoryBarrierWithGroupSync();
    uint groupIdx  = GTid.y * DDGI_DIST_TILE + GTid.x;
    int2 padOrigin = tileOrigin - 1;
    [loop]
    for (uint b = groupIdx; b < DDGI_BORDER_COUNT; b += DDGI_DIST_TILE * DDGI_DIST_TILE) {
        uint4 bo = kBorderOffsets[b];
        DistAtlas[padOrigin + int2(bo.zw)] = DistAtlas[padOrigin + int2(bo.xy)];
    }
}
