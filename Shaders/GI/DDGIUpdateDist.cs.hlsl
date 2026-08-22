#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 
#define DDGI_DIST_TILE 14
#define DDGI_DIST_SHARP 50.0f

// Layout completo espelhado por DDGIConstants.
cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;
    float4 GridCountRays;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColorHyst;
    float4 TraceParams;
    float4 DistAtlasParams;
    float4 MiscParams;
    float4 MiscParams2;
    float4 RayEpsA;
    float4 RayEpsB;
    float4 GIDistParams;
    float4 GIBiasParams;
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;
    float4 InvalidateMin;     // xyz = min da caixa de invalidacao, w = 1 se ativa (irradiancia)
    float4 InvalidateMaxHyst; // xyz = max da caixa, w = hysteresis regional da IRRADIANCIA
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 MiscParams3;       // x = hysteresis regional DESTE atlas, y = 1 se a janela dele esta
                              // aberta (e mais longa que a da irradiancia — ver DDGI.h)
    // z = prefixo atualizado; w = intervalo temporal da grossa.
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 GICascadeScrollDelta[4];
    float4 ProbeCompactionParams; // x = Trace/Update usam a lista compacta desta passada
};

Texture2D<float4>   ProbesTrace : register(t0);
Buffer<float4>      ProbeData   : register(t1); // w>=1 = probe recem-ativado/relocado
Buffer<uint>        ActiveProbeIndices : register(t2);
Buffer<uint>        ActiveProbeCount   : register(t3);
RWTexture2D<float2> DistAtlas   : register(u0);

// uint4(srcX, srcY, dstX, dstY) para a borda octaedrica do tile.
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
    int numProbes = (int)AtlasParams.w;
    int updateProbes = clamp((int)GICascadeParams.z, 1, numProbes);
    const bool compact = ProbeCompactionParams.x > 0.5f;
    const int workProbes = compact ? min((int)ActiveProbeCount[0], updateProbes) : updateProbes;
    int workIdx = DDGI_ProbeFromGroup(Gid.xy, max(workProbes, 1));
    if (workIdx >= workProbes) return;
    int probeIdx = compact ? (int)ActiveProbeIndices[workIdx] : workIdx;
    if (probeIdx >= updateProbes) return;

    int3 count = (int3)GridCountRays.xyz;
    int  cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    int  localIdx = DDGI_LocalProbeIndex(probeIdx, count);
    int3 scroll = (int3)GICascadeScrollOffset[cascade].xyz;
    int3 pc     = DDGI_GeometricCoord(DDGI_ProbeCoord(localIdx, count), scroll, count);
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);
    int  tile  = (int)DistAtlasParams.x;
    int2 tileOrigin = DDGI_TileOrigin(pc, scroll, count, tile,
                                      DDGI_TilesPerRow(DistAtlasParams.y, tile), cascade);
    int2 local      = int2(GTid.xy);

    float2 octUV    = ((float2)local + 0.5f) / (float)tile;
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    // Limita os momentos à vizinhanca relevante do Chebyshev nesta cascata.
    float distMax = GICascadeGridMinSpacing[cascade].w * 2.6f;

    uint  frame = (uint)TraceParams.x;
    float sumD = 0.0f, sumD2 = 0.0f, wsum = 0.0f;

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float ad = ProbesTrace[DDGI_TraceTexel(probeIdx, r, numProbes, DDGI_RAYS)].a;
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
    // Momentos usam histerese propria; sonda nova/relocada descarta o historico espacial antigo.
    const float temporalInterval = (cascade > 0) ? max(GICascadeParams.w, 1.0f) : 1.0f;
    const float temporalHyst = pow(saturate(DistAtlasParams.w), temporalInterval);
    float  hyst  = (newlyExposed || ProbeData[probeIdx].w >= 1.0f) ? 0.0f : temporalHyst;
    // A caixa e compartilhada, mas a janela e a histerese pertencem ao atlas de distancia.
    const float4 distInvMin = float4(InvalidateMin.xyz, MiscParams3.y);
    hyst = DDGI_RegionalHysteresis(pc, ProbeData[probeIdx].xyz, GICascadeGridMinSpacing[cascade].xyz,
                                   GICascadeGridMinSpacing[cascade].w, distInvMin, InvalidateMaxHyst.xyz,
                                   MiscParams3.x, hyst);
    DistAtlas[texel] = lerp(result, prev, hyst);

    // Publica o interior antes de copiar a borda octaedrica.
    DeviceMemoryBarrierWithGroupSync();
    uint groupIdx  = GTid.y * DDGI_DIST_TILE + GTid.x;
    int2 padOrigin = tileOrigin - 1;
    [loop]
    for (uint b = groupIdx; b < DDGI_BORDER_COUNT; b += DDGI_DIST_TILE * DDGI_DIST_TILE) {
        uint4 bo = kBorderOffsets[b];
        DistAtlas[padOrigin + int2(bo.zw)] = DistAtlas[padOrigin + int2(bo.xy)];
    }
}
