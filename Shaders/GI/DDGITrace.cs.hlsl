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
    float4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
    float4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
    float4 GIDistParams;    // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;    // x=escala do bias, y=teto em metros, z=fade de sondas,
                            // w=piso de roughness do hit (cache nao-direcional)
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;       // x = view height (km), y = raio do planeta (km) — ver ShadeSky
    // Campos preservam o layout completo de DDGICB, mesmo quando este passe nao os usa.
    float4 InvalidateMin;
    float4 InvalidateMaxHyst;
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 MiscParams3;     // z = 1 se o passe de classificacao de raios roda neste frame
    float4 GICascadeParams;          // x = nº de cascatas, y = sondas por cascata
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 GICascadeScrollDelta[4];
    float4 ProbeCompactionParams; // x = Trace/Update usam a lista compacta desta passada
};

RaytracingAccelerationStructure Scene       : register(t0);
Texture2D<float4>               SkyViewLUT  : register(t1);
StructuredBuffer<InstanceGeo>   Instances   : register(t2);
Texture2D<float4>               IrradAtlas  : register(t3);
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t6);
Buffer<uint>                    ProbeRayCount:register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8);
Buffer<uint>                    ActiveProbeIndices : register(t9);
Buffer<uint>                    ActiveProbeCount   : register(t10);

RWTexture2D<float4>             ProbesTrace : register(u0);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "HitShading.hlsli" 

#define DDGI_RAY_UNUSED -1e9f

[numthreads(DDGI_RAYS, 1, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int   numProbes = (int)AtlasParams.w;
    int   updateProbes = clamp((int)GICascadeParams.z, 1, numProbes);
    const bool compact = ProbeCompactionParams.x > 0.5f;
    const int workProbes = compact ? min((int)ActiveProbeCount[0], updateProbes) : updateProbes;
    // A compactacao muda apenas a ordem de trabalho, nao o indice global da sonda.
    int   workIdx   = DDGI_ProbeFromGroup(Gid.xy, max(workProbes, 1));
    int   rayIdx    = (int)GTid.x;
    if (workIdx >= workProbes) return;
    int   probeIdx  = compact ? (int)ActiveProbeIndices[workIdx] : workIdx;
    if (probeIdx >= updateProbes) return;

    int3  count   = (int3)GridCountRays.xyz;
    int   cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    int   localIdx = DDGI_LocalProbeIndex(probeIdx, count);
    float3 gridMin = GICascadeGridMinSpacing[cascade].xyz;
    float spacing  = GICascadeGridMinSpacing[cascade].w;
    int3  scroll = (int3)GICascadeScrollOffset[cascade].xyz;
    int3  pc     = DDGI_GeometricCoord(DDGI_ProbeCoord(localIdx, count), scroll, count);
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);

    // Classificacao usa todos os raios para nao realimentar a propria decimacao.
    const bool scrollOnly    = MiscParams3.w > 0.5f;
    const bool classifyFrame = MiscParams3.z > 0.5f && !scrollOnly;
    const bool justRelocated = GIProbeData[probeIdx].w >= 1.0f;
    int rayCount = (classifyFrame || newlyExposed || justRelocated)
                 ? DDGI_RAYS : (int)ProbeRayCount[probeIdx];
    int stride   = DDGI_RAYS / max(rayCount, 1);
    if ((rayIdx % max(stride, 1)) != 0) {
        ProbesTrace[DDGI_TraceTexel(probeIdx, rayIdx, numProbes, DDGI_RAYS)] =
            float4(0.0f, 0.0f, 0.0f, DDGI_RAY_UNUSED);
        return;
    }

    // O offset do slot recem-exposto pertence ao ponto antigo.
    float3 relocOffset = newlyExposed ? float3(0.0f, 0.0f, 0.0f) : GIProbeData[probeIdx].xyz;
    float3 probePos = DDGI_ProbeWorldPos(pc, gridMin, spacing) + relocOffset;

    float3 dir = DDGI_RayDirection(rayIdx, DDGI_RAYS, (uint)TraceParams.x, (uint)probeIdx);

    float3 sunDir = normalize(SunDirIntensity.xyz);
    float  maxT   = TraceParams.y;

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxT;

    RayQuery<RAY_FLAG_NONE> q;
    // RAY_FLAG_NONE preserva backfaces para detectar sondas enterradas.
    q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    // O peso externo do volume sempre usa a cascata grossa.
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
    P.Count          = count;               P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColorHyst.rgb;    P.ShadowRayBias = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = maxT;
    P.AlbedoLOD      = 4.0f;
    P.NumLights      = (int)MiscParams2.y;
    P.ShadowRayMask  = (uint)MiscParams2.z;
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
    P.SkyViewHeightKm    = SkyParams.x;
    P.SkyBottomRKm       = SkyParams.y;
    P.RoughnessMin       = GIBiasParams.w;
    P.CacheRayRoughness  = -1.0f;
    RC_UNPACK_PARAMS(P);

    float3 radiance;
    float  signedDist;

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        radiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                   q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                   ray.Origin, ray.Direction, q.CommittedRayT(), P, signedDist);
        // Distancia assinada curta torna o Chebyshev mais conservador perto de backfaces.
        if (signedDist < 0.0f) signedDist *= 0.2f;
    } else {
        radiance   = ShadeSky(dir, sunDir, P.SkyIntensity, P);
        signedDist = maxT;
    }

    ProbesTrace[DDGI_TraceTexel(probeIdx, rayIdx, numProbes, DDGI_RAYS)] =
        float4(radiance, signedDist);
}
