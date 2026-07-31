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
    // Nao usados aqui; declarados p/ os offsets do CB baterem ate o perfil de epsilons.
    float4 DebugParams;
    row_major float4x4 View;
    float4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
    float4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
    float4 PolicyParams;            // x = politica deste passe (backface/culling)
    // Gather do 2o bounce (contrato do HitShading.hlsli).
    float4 GIDistParams;            // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;            // x=escala do bias, y=teto em metros, zw=-
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
};

// Ver ReflectionTrace.cs.hlsl: politica por passe, no molde do Context.CullingMode do Lumen.
uint ReflectionCullFlags() {
    return (PolicyParams.x > 0.5f) ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE;
}

#include "../RayOffset.hlsli" // depois do cbuffer: le RayEpsA/RayEpsB

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
// t4/t5: atlas de distancia e ProbeData do DDGI — o 2o bounce usa o gather COMPLETO
// (Chebyshev + bias + skip), igual ao deferred. Antes eram filler do VB/IB bindless.
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8); // F5: luzes puntuais nos hits

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
        float2   E     = GGX_Rand2E((uint2)px, (uint)TraceParams.x, SMILE_RNG_REFL_MIRROR);
        E.y *= 1.0f - 0.1f;
        float3x3 basis = GGX_TangentBasis(N);
        float3   Vt    = mul(basis, V);
        float4   ggx   = GGX_SampleVNDF(E, alpha, Vt);
        float3   Hw    = mul(ggx.xyz, basis);
        R = reflect(-V, Hw);
        if (dot(R, N) <= 0.0f) R = reflect(-V, N);
    }

    float3 sunDir = normalize(SunDirIntensity.xyz);

    // Offset robusto (so anti self-hit): o bias 0.2 na origem deslocava o reflexo de contato
    // e inflava o hitT entregue ao NRD/temporal.
    RayDesc ray;
    ray.Origin    = OffsetRayGBuffer(worldPos, N, R, length(CameraPos.xyz - worldPos));
    ray.Direction = R;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;

    RayQuery<RAY_FLAG_NONE> q;
    // ALL: reflexao inclui translucido (ver ReflectionTrace.cs.hlsl).
    q.TraceRayInline(Scene, ReflectionCullFlags(), SMILE_RT_MASK_ALL, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
    P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColor.rgb;        P.ShadowRayBias = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
    P.AlbedoLOD      = ReflectParams.w;
    P.RealHitShading = ReflectParams.z > 0.5f;
    P.NumLights      = (int)CameraPos.w; // F5 (w da CameraPos era constante 1.0, livre)
    P.ShadowRayMask  = (uint)SunColor.w;
    P.ReGIRGridMin       = ReGIRGridMinSlots.xyz;
    P.ReGIRSlotsPerCell  = (uint)ReGIRGridMinSlots.w;
    P.ReGIRInvCellSize   = ReGIRInvCellEnabled.xyz;
    P.ReGIREnabled       = ReGIRInvCellEnabled.w > 0.5f;
    P.ReGIRGridCount     = (int3)ReGIRGridCountSamples.xyz;
    P.ReGIRSampleCount   = (int)ReGIRGridCountSamples.w;
    P.ReGIRSlotsSRV      = (uint)ReGIRResources.x;
    P.ReGIRAverageSRV    = (uint)ReGIRResources.y;
    P.FrameIndex         = (uint)TraceParams.x;
    P.ReGIRPad           = 0u;

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
