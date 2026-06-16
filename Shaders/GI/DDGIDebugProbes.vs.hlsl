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
    int3 count = (int3)GridCount.xyz;
    int3 pc    = DDGI_ProbeCoord((int)iid, count);

    float4 pd       = ProbeData[iid];      
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

    float radius   = GridMinSpacing.w * DebugParams.y;
    float3 worldPos = probePos + sphereN * radius;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0f), ViewProj);
    o.nrm      = sphereN;
    o.irrTile  = (float2)DDGI_TileOrigin(pc, count, (int)AtlasParams.x);
    o.distTile = (float2)DDGI_TileOrigin(pc, count, (int)DistAtlasParams.x);

    float relocMag = length(offset) / max(DebugParams.z, 1e-4f);
    float4 stats   = ProbeStats[iid];

    o.extra = float4(saturate(relocMag), stats.x, stats.z, pd.w);
    return o;
}
