#include "../GI/DDGICommon.hlsli"
#include "GGXSample.hlsli"

cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;       
    float4 ScreenParams;    
    float4 ReflectParams;   
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity; 
    float4 SunColor;
    float4 TraceParams;     
    float4 HalfScreenParams;
    row_major float4x4 PrevViewProj;
    float4 TemporalParams;  
};

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
StructuredBuffer<DDGIVertex>    Vertices   : register(t4);
Buffer<uint>                    Indices    : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7); 

RWTexture2D<float4>             RWResolved : register(u0); 

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 px = DTid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  fullResMax = max(TemporalParams.w, 1e-3f);

    if (roughness > fullResMax) return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) return; 

    float combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));
    if (combineAlpha <= 0.0f) return;

    float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 V = normalize(CameraPos.xyz - worldPos);

    float3 R;
    if (roughness < 0.05f) {
        R = reflect(-V, N);                      
    } else {
        float    alpha = max(roughness * roughness, 1e-3f);
        float2   E     = GGX_Rand2((uint2)px, (uint)TraceParams.x);
        E.y *= 1.0f - 0.1f;
        float3x3 basis = GGX_TangentBasis(N);
        float3   Vt    = mul(basis, V);
        float4   ggx   = GGX_SampleVNDF(E, alpha, Vt);
        float3   Hw    = mul(ggx.xyz, basis);
        R = reflect(-V, Hw);
        if (dot(R, N) <= 0.0f) R = reflect(-V, N);
    }

    float3 sunDir = normalize(SunDirIntensity.xyz);

    RayDesc ray;
    ray.Origin    = worldPos + N * max(TraceParams.w, 1e-3f);
    ray.Direction = R;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;

    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
    P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColor.rgb;        P.NormalBias   = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
    P.AlbedoLOD      = ReflectParams.w;
    P.RealHitShading = ReflectParams.z > 0.5f;

    float3 radiance;
    float  hitDist = TraceParams.y;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float sd;
        hitDist  = q.CommittedRayT();
        radiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                   q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                   ray.Origin, ray.Direction, hitDist, P, sd);
    } else {
        radiance = ShadeSky(R, sunDir, P.SkyIntensity);
    }

    RWResolved[px] = float4(radiance, hitDist);
}
