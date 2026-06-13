// DDGI Fase 2 — update do atlas de DISTANCIA (SM 6.0). 1 grupo por probe; 14x14 threads = 1
// texel octaedrico cada. Integra a distancia dos raios (ProbesTrace.a) ponderada por
// cos^DistSharpness (mais "afiado" que a irradiancia) -> (mean, mean²) por direcao. Blend
// temporal in-place. Usado pelo teste de visibilidade de Chebyshev na cena (mata light-leak).

#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 // teto; LOD adaptativo marca os nao-tracados com sentinela (pulados)
#define DDGI_DIST_TILE 14
#define DDGI_DIST_SHARP 50.0f

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;  // xyz = grid origin, w = spacing
    float4 GridCountRays;   // xyz = probe counts, w = rays per probe
    float4 AtlasParams;     // x = irr tile, y = irr atlasW, z = irr atlasH, w = numProbes
    float4 SunDirIntensity;
    float4 SunColorHyst;    // w = hysteresis
    float4 TraceParams;     // x = frameIndex, y = maxRayDist
    float4 DistAtlasParams; // x = dist tile, y = dist atlasW, z = dist atlasH, w = -
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
    int  tile  = (int)DistAtlasParams.x; // = DDGI_DIST_TILE
    int2 tileOrigin = DDGI_TileOrigin(pc, count, tile);
    int2 local      = int2(GTid.xy);

    float2 octUV    = ((float2)local + 0.5f) / (float)tile;
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    // Clamp da distancia p/ nao estourar o RG16F no termo² (spacing*4 > dist entre probes vizinhos).
    float distMax = GridMinSpacing.w * 4.0f;

    uint  frame = (uint)TraceParams.x;
    float sumD = 0.0f, sumD2 = 0.0f, wsum = 0.0f;

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float ad = ProbesTrace[int2(r, probeIdx)].a;
        if (ad < -1e8f) continue; // raio nao tracado (sentinela do LOD adaptativo)
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame);
        float  w    = pow(max(0.0f, dot(texelDir, rdir)), DDGI_DIST_SHARP);
        if (w <= 0.0f) continue;
        // .a vem com SINAL do trace (negativo = backface, p/ relocacao); distancia = |.a|.
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
