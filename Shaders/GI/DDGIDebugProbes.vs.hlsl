#include "DDGICommon.hlsli"

#define DBG_RINGS 8
#define DBG_SEGS  12

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;  // xyz = origem, w = espacamento
    float4 GridCount;       // xyz = counts, w = numProbes
    float4 AtlasParams;     // x = tile, y = atlasW, z = atlasH
    float4 DistAtlasParams; // x = distTile, y = distW, z = distH, w = maxRayDist
    float4 DebugParams;     // x = mode, y = probeRadius(frac do spacing), z = relocMaxOffset
    float4 CameraPos;       // xyz
};

Buffer<float4> ProbeData  : register(t0); // xyz = offset de relocacao, w = state
Buffer<float4> ProbeStats : register(t1); // x = backRatio, y = minFront, z = meanDist, w = state

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 nrm      : NORMAL;
    nointerpolation float2 irrTile  : TEXCOORD0;
    nointerpolation float2 distTile : TEXCOORD1;
    nointerpolation float4 extra    : TEXCOORD2; // x=relocMag01, y=backRatio, z=meanDist, w=state
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    int3 count = (int3)GridCount.xyz;
    int3 pc    = DDGI_ProbeCoord((int)iid, count);

    float4 pd       = ProbeData[iid];      // xyz = offset relocacao, w = state real (0=inativa/1=ativa)
    float3 offset   = pd.xyz;
    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, GridMinSpacing.w) + offset;

    // Geometria da esfera procedural: 2 triangulos por quad (ring x seg).
    uint quad   = vid / 6u;
    uint corner = vid % 6u;
    uint ring   = quad / DBG_SEGS;
    uint seg    = quad % DBG_SEGS;
    // Passos (dr, ds) dos 6 vertices dos 2 triangulos do quad.
    const float2 off[6] = {
        float2(0,0), float2(1,0), float2(1,1),
        float2(0,0), float2(1,1), float2(0,1)
    };
    float dr = off[corner].x, ds = off[corner].y;
    float theta = (ring + dr) / (float)DBG_RINGS * SMILE_PI;       // 0..PI (polo a polo)
    float phi   = (seg  + ds) / (float)DBG_SEGS  * (2.0f * SMILE_PI);
    float st = sin(theta), ct = cos(theta);
    float3 sphereN = float3(st * cos(phi), ct, st * sin(phi));

    float radius   = GridMinSpacing.w * DebugParams.y;
    float3 worldPos = probePos + sphereN * radius;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0f), ViewProj);
    o.nrm      = sphereN;
    o.irrTile  = (float2)DDGI_TileOrigin(pc, count, (int)AtlasParams.x);
    o.distTile = (float2)DDGI_TileOrigin(pc, count, (int)DistAtlasParams.x);

    float relocMag = length(offset) / max(DebugParams.z, 1e-4f);
    float4 stats   = ProbeStats[iid];
    // extra.w = state REAL e congelado do ProbeData (o que o sampler usa), nao o re-derivado.
    o.extra = float4(saturate(relocMag), stats.x, stats.z, pd.w);
    return o;
}
