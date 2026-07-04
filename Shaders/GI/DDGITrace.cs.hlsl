#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;  
    float4 GridCountRays;   
    float4 AtlasParams;     
    float4 SunDirIntensity;
    float4 SunColorHyst;    
    float4 TraceParams;     
    float4 DistAtlasParams; 
    float4 MiscParams;      
};

RaytracingAccelerationStructure Scene       : register(t0);
Texture2D<float4>               SkyViewLUT  : register(t1);
StructuredBuffer<InstanceGeo>   Instances   : register(t2);
Texture2D<float4>               IrradAtlas  : register(t3); 
StructuredBuffer<DDGIVertex>    Vertices    : register(t4); 
Buffer<uint>                    Indices     : register(t5); 
Buffer<float4>                  ProbeData   : register(t6); 
Buffer<uint>                    ProbeRayCount:register(t7); 

RWTexture2D<float4>             ProbesTrace : register(u0);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "HitShading.hlsli" 

#define DDGI_RAY_UNUSED -1e9f

[numthreads(DDGI_RAYS, 1, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int   probeIdx = (int)Gid.x;
    int   rayIdx   = (int)GTid.x;
    int   numProbes = (int)AtlasParams.w;
    if (probeIdx >= numProbes) return;

    int3  count   = (int3)GridCountRays.xyz;
    float spacing = GridMinSpacing.w;
    int3  pc      = DDGI_ProbeCoord(probeIdx, count);

    int rayCount = (int)ProbeRayCount[probeIdx];
    int stride   = DDGI_RAYS / max(rayCount, 1);
    if ((rayIdx % max(stride, 1)) != 0) {
        ProbesTrace[int2(rayIdx, probeIdx)] = float4(0.0f, 0.0f, 0.0f, DDGI_RAY_UNUSED);
        return;
    }

    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, spacing) + ProbeData[probeIdx].xyz;

    float3 dir = DDGI_RayDirection(rayIdx, DDGI_RAYS, (uint)TraceParams.x);

    float3 sunDir = normalize(SunDirIntensity.xyz);
    float  maxT   = TraceParams.y;

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxT;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = spacing;
    P.Count          = count;               P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColorHyst.rgb;    P.NormalBias   = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = maxT;
    P.AlbedoLOD      = 4.0f; 
    P.RealHitShading = DistAtlasParams.w > 0.5f;

    float3 radiance;
    float  signedDist;

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        radiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                   q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                   ray.Origin, ray.Direction, q.CommittedRayT(), P, signedDist);
    } else {
        radiance   = ShadeSky(dir, sunDir, P.SkyIntensity);
        signedDist = maxT;
    }

    ProbesTrace[int2(rayIdx, probeIdx)] = float4(radiance, signedDist);
}
