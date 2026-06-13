// DDGI — passe de trace (DXR 1.1 inline ray tracing, SM 6.6). Por (probe, raio): atira 1 raio
// contra a TLAS. No hit/miss, sombreia via HitShading.hlsli (sol+sombra+multibounce / ceu),
// COMPARTILHADO com o Specular GI (reflexoes RT). Resultado (radiancia + distancia assinada)
// vai p/ ProbesTrace, que o DDGIUpdate integra no atlas octaedrico de irradiancia.
//
// Fase 1a: normal geometrica REAL do triangulo (barycentricas + WorldToObject). Fase 1b: albedo
// TEXTURIZADO bindless (ResourceDescriptorHeap, SM 6.6). Ambas gated por realHitShading
// (DistAtlasParams.w). A logica de hit/miss agora vive em HitShading.hlsli (ShadeSurfaceHit/ShadeSky).

#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 // teto (compile-time = numthreads). LOD adaptativo traca um subconjunto.

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;  // xyz = grid origin (world), w = probe spacing
    float4 GridCountRays;   // xyz = probe counts, w = rays per probe
    float4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
    float4 SunDirIntensity; // xyz = dir TO sun, w = sun intensity
    float4 SunColorHyst;    // rgb = sun color, w = hysteresis (update)
    float4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = normalBias
    float4 DistAtlasParams; // x = distTile, y/z = dist atlas, w = realHitShading (0=approx,1=normal real)
    float4 MiscParams;      // x = relocationEnabled, y = deactivThreshold, z = maxRays, w = minRays
};

// Globais lidos por HitShading.hlsli (mesmos nomes/registros que a fatoracao espera).
RaytracingAccelerationStructure Scene       : register(t0);
Texture2D<float4>               SkyViewLUT  : register(t1);
StructuredBuffer<InstanceGeo>   Instances   : register(t2);
Texture2D<float4>               IrradAtlas  : register(t3); // frame anterior (multibounce)
StructuredBuffer<DDGIVertex>    Vertices    : register(t4); // global mesclado
Buffer<uint>                    Indices     : register(t5); // global mesclado (R32_UINT)
Buffer<float4>                  ProbeData   : register(t6); // xyz = offset de relocacao, w = state
Buffer<uint>                    ProbeRayCount:register(t7); // raios deste probe (LOD adaptativo), escrito pelo relocate

RWTexture2D<float4>             ProbesTrace : register(u0);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "HitShading.hlsli" // ShadeSurfaceHit, ShadeSky, FHitShadeParams (usa os globais acima)

// Sentinela de "raio nao tracado" (LOD adaptativo): .a bem mais negativo que qualquer backface
// (backface = -|dist| >= -maxRayDist). Os passes de update/relocate ignoram (tr.a < -1e8).
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

    // --- LOD adaptativo (estilo GetProbeRaysCount do Flax, sem SDF): o nº de raios deste probe vem
    // do relocate (proximidade real = closestFront). Reamostra o set FIXO de DDGI_RAYS direcoes
    // (stride uniforme), entao update/relocate so precisam pular os sentinelas. ---
    int rayCount = (int)ProbeRayCount[probeIdx];
    int stride   = DDGI_RAYS / max(rayCount, 1);
    if ((rayIdx % max(stride, 1)) != 0) {
        ProbesTrace[int2(rayIdx, probeIdx)] = float4(0.0f, 0.0f, 0.0f, DDGI_RAY_UNUSED);
        return;
    }
    // Posicao do probe + offset de relocacao (Fase 2): traca do ponto realocado p/ fora de parede.
    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, spacing) + ProbeData[probeIdx].xyz;

    // Direcao do raio (rotacionada por frame; identica a usada no DDGIUpdate).
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
    while (q.Proceed()) {}

    // Parametros de shading (compartilhados com o Specular GI).
    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = spacing;
    P.Count          = count;               P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColorHyst.rgb;    P.NormalBias   = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = maxT;
    P.AlbedoLOD      = 4.0f; // difuso quer a cor media (sem detalhe/aliasing)
    P.RealHitShading = DistAtlasParams.w > 0.5f;

    float3 radiance;
    float  signedDist; // |dist| ao hit; NEGATIVO se backface (relocacao). Miss = +maxT.

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
