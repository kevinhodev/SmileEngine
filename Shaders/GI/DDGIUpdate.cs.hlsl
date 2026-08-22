#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 
#define DDGI_TILE 6

// Layout completo ate a cauda de cascatas, espelhado por DDGIConstants.
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
    float4 InvalidateMin;     // xyz = min da caixa de invalidacao, w = 1 se ativa
    float4 InvalidateMaxHyst; // xyz = max da caixa, w = hysteresis dentro dela
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 MiscParams3;
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
RWTexture2D<float4> IrradAtlas  : register(u0);

// uint4(srcX, srcY, dstX, dstY) para a borda octaedrica do tile.
#define DDGI_BORDER_COUNT 28
static const uint4 kBorderOffsets[DDGI_BORDER_COUNT] = {
    uint4(6,1, 1,0), uint4(5,1, 2,0), uint4(4,1, 3,0), uint4(3,1, 4,0), uint4(2,1, 5,0), uint4(1,1, 6,0),
    uint4(6,6, 1,7), uint4(5,6, 2,7), uint4(4,6, 3,7), uint4(3,6, 4,7), uint4(2,6, 5,7), uint4(1,6, 6,7),
    uint4(1,1, 0,6), uint4(1,2, 0,5), uint4(1,3, 0,4), uint4(1,4, 0,3), uint4(1,5, 0,2), uint4(1,6, 0,1),
    uint4(6,1, 7,6), uint4(6,2, 7,5), uint4(6,3, 7,4), uint4(6,4, 7,3), uint4(6,5, 7,2), uint4(6,6, 7,1),
    uint4(1,1, 7,7), uint4(6,1, 0,7), uint4(1,6, 7,0), uint4(6,6, 0,0)
};

// Ponto fixo 1/1024 para permitir InterlockedAdd em groupshared.
groupshared uint gChangeAccum;

[numthreads(DDGI_TILE, DDGI_TILE, 1)]
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
    int  tile  = (int)AtlasParams.x;
    int2 tileOrigin = DDGI_TileOrigin(pc, scroll, count, tile,
                                      DDGI_TilesPerRow(AtlasParams.y, tile), cascade);
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
        float4 tr = ProbesTrace[DDGI_TraceTexel(probeIdx, r, numProbes, DDGI_RAYS)];
        if (tr.a < -1e8f) continue; 
        ++realCount;
        if (tr.a < 0.0f) { ++backfaceCount; continue; } 
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame, (uint)probeIdx);
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

    // Historico de sonda nova/relocada pertence a outro ponto; grossa compensa updates pulados.
    const float temporalInterval = (cascade > 0) ? max(GICascadeParams.w, 1.0f) : 1.0f;
    const float temporalHyst = pow(saturate(SunColorHyst.w), temporalInterval);
    float  hyst    = (newlyExposed || ProbeData[probeIdx].w >= 1.0f) ? 0.0f : temporalHyst;

    // Invalidacao regional usa a posicao relocada e o grid da propria cascata.
    hyst = DDGI_RegionalHysteresis(pc, ProbeData[probeIdx].xyz, GICascadeGridMinSpacing[cascade].xyz,
                                   GICascadeGridMinSpacing[cascade].w, InvalidateMin, InvalidateMaxHyst.xyz,
                                   InvalidateMaxHyst.w, hyst);

    // Detector reduz por sonda para um texel ruidoso nao dominar a resposta.
    uint groupIdx = GTid.y * DDGI_TILE + GTid.x;
    if (groupIdx == 0u) gChangeAccum = 0u;
    GroupMemoryBarrierWithGroupSync();
    const bool  adaptive = MiscParams2.w > 0.5f;
    const float relTexel = adaptive ? DDGI_RelChange3(result, prev, 0.02f) : 0.0f;
    InterlockedAdd(gChangeAccum, (uint)(relTexel * 1024.0f + 0.5f));
    GroupMemoryBarrierWithGroupSync();
    const float relProbe = (float)gChangeAccum * (1.0f / (1024.0f * DDGI_TILE * DDGI_TILE));
    const float hystNew  = DDGI_AdaptiveHysteresis(relProbe, hyst);

    // Limita a resposta de sondas enterradas para evitar oscilacao no limiar de backface.
    hyst = occluded ? min(hyst, max(hystNew, 0.90f)) : hystNew;

    float3 blended = lerp(result, prev, hyst);
    IrradAtlas[texel] = float4(blended, 1.0f);

    // Publica o interior antes de copiar a borda octaedrica.
    DeviceMemoryBarrierWithGroupSync();
    int2 padOrigin = tileOrigin - 1;
    [loop]
    for (uint b = groupIdx; b < DDGI_BORDER_COUNT; b += DDGI_TILE * DDGI_TILE) {
        uint4 bo = kBorderOffsets[b];
        IrradAtlas[padOrigin + int2(bo.zw)] = IrradAtlas[padOrigin + int2(bo.xy)];
    }
}
