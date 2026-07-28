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
    float4 MiscParams2;     // y = nº de luzes puntuais no SceneLights (F5)
    // Perfil de epsilons: o DDGI so usa a familia (2) — os raios dele partem de PROBES, nao do
    // G-buffer, entao nao passa pelo OffsetRayGBuffer. Mas o HitShading.hlsli le RayEpsA.w e
    // RayEpsB.x nos shadow rays do 2o hit, que sao os mesmos p/ os tres passes.
    float4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
    float4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
    // Gather do 2o bounce (contrato do HitShading.hlsli): dist atlas + skipMode, bias.
    float4 GIDistParams;    // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;    // x=escala do bias, y=teto em metros, zw=-
};

RaytracingAccelerationStructure Scene       : register(t0);
Texture2D<float4>               SkyViewLUT  : register(t1);
StructuredBuffer<InstanceGeo>   Instances   : register(t2);
Texture2D<float4>               IrradAtlas  : register(t3);
// t4 = atlas de distancia (2o bounce com Chebyshev; ver ShadeSurfaceHit). t5 segue filler.
Texture2D<float4>               GIDistAtlas : register(t4);
// Offsets de relocacao: usados aqui na origem do raio E no gather do 2o bounce.
Buffer<float4>                  GIProbeData : register(t6);
Buffer<uint>                    ProbeRayCount:register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8); // F5: luzes puntuais no GI

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

    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, spacing) + GIProbeData[probeIdx].xyz;

    float3 dir = DDGI_RayDirection(rayIdx, DDGI_RAYS, (uint)TraceParams.x, (uint)probeIdx);

    float3 sunDir = normalize(SunDirIntensity.xyz);
    float  maxT   = TraceParams.y;

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxT;

    RayQuery<RAY_FLAG_NONE> q;
    // GATHER (sem translucido): o vidro nao pode barrar a luz que alimenta as probes, senao
    // ambiente envidracado fica escuro demais. RAY_FLAG_NONE segue de proposito — a deteccao de
    // probe enterrada precisa ENXERGAR backface (distancia assinada abaixo).
    q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = spacing;
    P.Count          = count;               P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColorHyst.rgb;    P.ShadowRayBias = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = maxT;
    P.AlbedoLOD      = 4.0f;
    P.RealHitShading = DistAtlasParams.w > 0.5f;
    P.NumLights      = (int)MiscParams2.y;
    P.ShadowRayMask  = (uint)MiscParams2.z;

    float3 radiance;
    float  signedDist;

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        radiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                   q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                   ray.Origin, ray.Direction, q.CommittedRayT(), P, signedDist);
        // Backface: encurta a distancia (RTXGI usa 0.2x) — o UpdateDist usa abs(), entao a media
        // de distancia perto/dentro de geometria cai e o Chebyshev escurece mais agressivo ali.
        // So aqui no DDGI: o HitShading e compartilhado e reflexoes/ReSTIR precisam do hitT real.
        if (signedDist < 0.0f) signedDist *= 0.2f;
    } else {
        radiance   = ShadeSky(dir, sunDir, P.SkyIntensity);
        signedDist = maxT;
    }

    ProbesTrace[int2(rayIdx, probeIdx)] = float4(radiance, signedDist);
}
