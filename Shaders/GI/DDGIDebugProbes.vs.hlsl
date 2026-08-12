#include "DDGICommon.hlsli"

#define DBG_RINGS 8
#define DBG_SEGS  12

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing; 
    float4 GridCount;       
    float4 AtlasParams;     
    float4 DistAtlasParams; 
    float4 DebugParams;     
    float4 CameraPos;       
    float4 RayParams;       // z = selected count, w = risk slot
    float4 SelectedIndices0;
    float4 SelectedIndices1;
    float4 SelectedWeights0;
    float4 SelectedWeights1;
};

Buffer<float4> ProbeData  : register(t0); 
Buffer<float4> ProbeStats : register(t1); 

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 nrm      : NORMAL;
    nointerpolation float2 irrTile  : TEXCOORD0;
    nointerpolation float2 distTile : TEXCOORD1;
    nointerpolation float4 extra    : TEXCOORD2; 
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    uint probeIid = iid;
    float selectedWeight = 1.0f;
    float selectedRisk = 0.0f;
    float selectedFocus = 0.0f;
    if ((uint)DebugParams.x == 5u && iid < (uint)RayParams.z) {
        probeIid = iid < 4u
            ? (uint)SelectedIndices0[iid]
            : (uint)SelectedIndices1[iid - 4u];
        selectedWeight = iid < 4u
            ? SelectedWeights0[iid]
            : SelectedWeights1[iid - 4u];
        selectedRisk = RayParams.w >= 0.0f && iid == (uint)RayParams.w ? 1.0f : 0.0f;
    }
    selectedFocus = CameraPos.w >= 0.0f &&
                    probeIid == (uint)CameraPos.w ? 1.0f : 0.0f;

    int3 count = (int3)GridCount.xyz;
    int3 pc    = DDGI_ProbeCoord((int)probeIid, count);

    float4 pd       = ProbeData[probeIid];
    float3 offset   = pd.xyz;
    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, GridMinSpacing.w) + offset;

    uint quad   = vid / 6u;
    uint corner = vid % 6u;
    uint ring   = quad / DBG_SEGS;
    uint seg    = quad % DBG_SEGS;

    const float2 off[6] = {
        float2(0,0), float2(1,0), float2(1,1),
        float2(0,0), float2(1,1), float2(0,1)
    };
    float dr = off[corner].x, ds = off[corner].y;
    float theta = (ring + dr) / (float)DBG_RINGS * SMILE_PI;       
    float phi   = (seg  + ds) / (float)DBG_SEGS  * (2.0f * SMILE_PI);
    float st = sin(theta), ct = cos(theta);
    float3 sphereN = float3(st * cos(phi), ct, st * sin(phi));

    // O foco e maior para continuar identificavel sem depender de cor.
    float radius   = GridMinSpacing.w * DebugParams.y *
                     (selectedFocus > 0.5f ? 1.35f : 1.0f);
    float3 worldPos = probePos + sphereN * radius;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0f), ViewProj);
    o.nrm      = sphereN;
    o.irrTile  = (float2)DDGI_TileOrigin(pc, count, (int)AtlasParams.x,
                                         DDGI_TilesPerRow(AtlasParams.y, (int)AtlasParams.x), 0);
    o.distTile = (float2)DDGI_TileOrigin(pc, count, (int)DistAtlasParams.x,
                                         DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x), 0);

    float relocMag = length(offset) / max(DebugParams.z, 1e-4f);
    float4 stats   = ProbeStats[probeIid];

    o.extra = (uint)DebugParams.x == 5u
        ? float4(selectedWeight, selectedRisk, selectedFocus, pd.w)
        : float4(saturate(relocMag), stats.x, stats.z, pd.w);
    return o;
}
