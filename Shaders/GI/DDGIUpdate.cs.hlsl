#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 
#define DDGI_TILE 6

// Declarado ate o FIM do DDGICB (e nao so o prefixo que este passe usava) porque a invalidacao
// espacial mora nos dois ultimos campos. Prefixo truncado le por offset e funciona, mas qualquer
// campo novo depois passa a exigir esta cadeia inteira mesmo assim.
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
};

Texture2D<float4>   ProbesTrace : register(t0);
Buffer<float4>      ProbeData   : register(t1); // w>=1 = probe recem-ativado/relocado
RWTexture2D<float4> IrradAtlas  : register(u0);

// Tabela de copia da borda octaedrica (wrap com fold; do Wicked, mesma que o Flax usa):
// uint4(srcX, srcY, dstX, dstY) em coords do tile COM borda (0..7 p/ interior 6).
#define DDGI_BORDER_COUNT 28
static const uint4 kBorderOffsets[DDGI_BORDER_COUNT] = {
    uint4(6,1, 1,0), uint4(5,1, 2,0), uint4(4,1, 3,0), uint4(3,1, 4,0), uint4(2,1, 5,0), uint4(1,1, 6,0),
    uint4(6,6, 1,7), uint4(5,6, 2,7), uint4(4,6, 3,7), uint4(3,6, 4,7), uint4(2,6, 5,7), uint4(1,6, 6,7),
    uint4(1,1, 0,6), uint4(1,2, 0,5), uint4(1,3, 0,4), uint4(1,4, 0,3), uint4(1,5, 0,2), uint4(1,6, 0,1),
    uint4(6,1, 7,6), uint4(6,2, 7,5), uint4(6,3, 7,4), uint4(6,4, 7,3), uint4(6,5, 7,2), uint4(6,6, 7,1),
    uint4(1,1, 7,7), uint4(6,1, 0,7), uint4(1,6, 7,0), uint4(6,6, 0,0)
};

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

    // Probe recem-ativado/relocado (marcado pelo Relocate): historia e do lugar antigo (ou
    // preto de dentro da parede) — descarta e toma a estimativa nova inteira.
    float  hyst    = (ProbeData[probeIdx].w >= 1.0f) ? 0.0f : SunColorHyst.w;

    // Invalidacao ESPACIAL (FDDGI::InvalidateRegion): um objeto nasceu ou morreu aqui perto.
    // Diferente do caso acima, a historia desta sonda continua quase toda valida — so a parcela
    // vinda daquele objeto mudou. Por isso troca por uma hysteresis RAPIDA em vez de zerar:
    // converge em ~12 frames sem o pop de amostra unica. Fora da caixa, nada muda — e essa a
    // diferenca para o reset global, que jogava fora dado bom da cena inteira.
    if (InvalidateMin.w > 0.5f) {
        float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, GridMinSpacing.w);
        if (all(probePos >= InvalidateMin.xyz) && all(probePos <= InvalidateMaxHyst.xyz))
            hyst = min(hyst, InvalidateMaxHyst.w);
    }
    float3 blended = lerp(result, prev, hyst);
    IrradAtlas[texel] = float4(blended, 1.0f);

    // Copia a borda de 1px do tile (fold octaedrico) p/ o bilinear ser continuo na costura.
    // Barrier de DEVICE: as fontes sao texels do atlas escritos por outras threads do grupo.
    // Sem divergencia no return de cima: probeIdx e uniforme no grupo inteiro.
    DeviceMemoryBarrierWithGroupSync();
    uint groupIdx  = GTid.y * DDGI_TILE + GTid.x;
    int2 padOrigin = tileOrigin - 1;
    [loop]
    for (uint b = groupIdx; b < DDGI_BORDER_COUNT; b += DDGI_TILE * DDGI_TILE) {
        uint4 bo = kBorderOffsets[b];
        IrradAtlas[padOrigin + int2(bo.zw)] = IrradAtlas[padOrigin + int2(bo.xy)];
    }
}
