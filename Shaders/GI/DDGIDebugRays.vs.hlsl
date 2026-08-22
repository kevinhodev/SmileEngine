#include "DDGICommon.hlsli"

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;  
    float4 GridCount;       
    float4 AtlasParams;
    float4 DistAtlasParams;
    float4 DebugParams;     
    float4 CameraPos;       
    float4 RayParams;
    float4 SelectedIndices[4];
    float4 SelectedWeights[4];
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
};

Buffer<float4> ProbeData     : register(t0); 
Buffer<uint>   ProbeRayCount : register(t5); 

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;                     
    nointerpolation float2 info : TEXCOORD1;      
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;
    int3   count    = (int3)GridCount.xyz;
    int    cascade  = DDGI_CascadeOfProbe((int)iid, count);
    int3   pc       = DDGI_GeometricCoord(DDGI_ProbeCoord(DDGI_LocalProbeIndex((int)iid, count),
                                                          count),
                                          (int3)GICascadeScrollOffset[cascade].xyz, count);
    float4 casc     = GICascadeGridMinSpacing[cascade];
    float4 pd       = ProbeData[iid];
    float3 probePos = DDGI_ProbeWorldPos(pc, casc.xyz, casc.w) + pd.xyz;

    if (pd.w < 0.0f || distance(probePos, CameraPos.xyz) > RayParams.y) {
        o.pos = float4(2, 2, 2, 1); o.uv = 0; o.info = 0; return o; 
    }

    uint  rayCount  = ProbeRayCount[iid];
    float numDigits = (rayCount >= 100u) ? 3.0f : (rayCount >= 10u) ? 2.0f : 1.0f;

    const float2 quad[6] = { float2(0,0), float2(1,0), float2(1,1),
                             float2(0,0), float2(1,1), float2(0,1) };
    float2 uv = quad[vid];

    float4 center = mul(float4(probePos, 1.0f), ViewProj);
    float  h = 0.045f;                 
    float  w = h * 0.62f * numDigits;  
    center.xy += float2((uv.x - 0.5f) * w, (uv.y - 0.5f) * h) * center.w;
    center.y  += (0.05f) * center.w;   

    o.pos  = center;
    o.uv   = uv;
    o.info = float2((float)rayCount, numDigits);
    return o;
}
