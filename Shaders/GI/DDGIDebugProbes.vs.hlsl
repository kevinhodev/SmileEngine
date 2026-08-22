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
    // 4+4 vetores cobrem os 16 slots do diagnostico.
    float4 SelectedIndices[4];
    float4 SelectedWeights[4];
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
};

Buffer<float4> ProbeData  : register(t0);
Buffer<float4> ProbeStats : register(t1);

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 nrm      : NORMAL;
    nointerpolation float2 irrTile  : TEXCOORD0;
    nointerpolation float2 distTile : TEXCOORD1;
    nointerpolation float4 extra    : TEXCOORD2;
    // Teto do heatmap pela cascata desta sonda.
    nointerpolation float  distMax  : TEXCOORD3;
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    uint probeIid = iid;
    float selectedWeight = 1.0f;
    float selectedRisk = 0.0f;
    float selectedFocus = 0.0f;
    if ((uint)DebugParams.x == 5u && iid < (uint)RayParams.z) {
        probeIid       = (uint)SelectedIndices[iid >> 2u][iid & 3u];
        selectedWeight = SelectedWeights[iid >> 2u][iid & 3u];
        selectedRisk = RayParams.w >= 0.0f && iid == (uint)RayParams.w ? 1.0f : 0.0f;
    }
    selectedFocus = CameraPos.w >= 0.0f &&
                    probeIid == (uint)CameraPos.w ? 1.0f : 0.0f;

    int3 count    = (int3)GridCount.xyz;
    int  cascade  = DDGI_CascadeOfProbe((int)probeIid, count);
    int3 scroll   = (int3)GICascadeScrollOffset[cascade].xyz;
    int3 pc       = DDGI_GeometricCoord(DDGI_ProbeCoord(DDGI_LocalProbeIndex((int)probeIid, count),
                                                        count), scroll, count);
    float4 casc   = GICascadeGridMinSpacing[cascade];

    float4 pd       = ProbeData[probeIid];
    float3 offset   = pd.xyz;
    float3 probePos = DDGI_ProbeWorldPos(pc, casc.xyz, casc.w) + offset;

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

    // Raio proporcional ao espacamento da cascata; foco maior independe de cor.
    float radius   = casc.w * DebugParams.y *
                     (selectedFocus > 0.5f ? 1.35f : 1.0f);
    float3 worldPos = probePos + sphereN * radius;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0f), ViewProj);
    o.nrm      = sphereN;
    o.irrTile  = (float2)DDGI_TileOrigin(pc, scroll, count, (int)AtlasParams.x,
                                         DDGI_TilesPerRow(AtlasParams.y, (int)AtlasParams.x), cascade);
    o.distMax  = casc.w * 2.6f;
    o.distTile = (float2)DDGI_TileOrigin(pc, scroll, count, (int)DistAtlasParams.x,
                                         DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x), cascade);

    // Normaliza pelo teto de relocacao da cascata.
    float relocMag = length(offset) / max(casc.w * 0.45f, 1e-4f);
    float4 stats   = ProbeStats[probeIid];

    o.extra = (uint)DebugParams.x == 5u
        ? float4(selectedWeight, selectedRisk, selectedFocus, pd.w)
        : float4(saturate(relocMag), stats.x, stats.z, pd.w);
    return o;
}
