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
    float4 GIBiasParams;    // x=escala do bias, y=teto em metros, z=fade de sondas,
                            // w=piso de roughness do hit (cache nao-direcional)
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;       // x = view height (km), y = raio do planeta (km) — ver ShadeSky
    // Invalidacao espacial: consumida pelo DDGIUpdate, nao por este passe. Declarada aqui como
    // preenchimento p/ alcancar o offset do cache (mesma convencao da cauda da agua nos shaders
    // de reflexao).
    float4 InvalidateMin;
    float4 InvalidateMaxHyst;
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 MiscParams3;     // z = 1 se o passe de classificacao de raios roda neste frame
    // Cascatas. Os nomes sao os do contrato do HitShading (GICascade*), e este passe usa o MESMO
    // bloco para a origem do raio — um so array, nao dois: dois nomes para o mesmo dado seria
    // exatamente o estado duplicado que o resto desta fase evitou.
    float4 GICascadeParams;          // x = nº de cascatas, y = sondas por cascata
    float4 GICascadeGridMinSpacing[4];
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
    int   numProbes = (int)AtlasParams.w;
    // Grade 2D de grupos (ver DDGI_ProbeFromGroup): o dispatch 1D parava em 65535 sondas.
    int   probeIdx  = DDGI_ProbeFromGroup(Gid.xy, numProbes);
    int   rayIdx    = (int)GTid.x;
    if (probeIdx >= numProbes) return;

    int3  count   = (int3)GridCountRays.xyz;
    // Indice GLOBAL -> (cascata, indice local). A GEOMETRIA da sonda — coordenada no grid, origem
    // e espacamento — e toda da cascata dela; o GridMinSpacing do cbuffer e o da GROSSA e serviria
    // so por coincidencia enquanto existe uma cascata.
    int   cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    int   localIdx = DDGI_LocalProbeIndex(probeIdx, count);
    float3 gridMin = GICascadeGridMinSpacing[cascade].xyz;
    float spacing  = GICascadeGridMinSpacing[cascade].w;
    int3  pc       = DDGI_ProbeCoord(localIdx, count);

    // Frame de CLASSIFICACAO: o passe de relocacao le ESTE trace para decidir quantos raios cada
    // sonda merece, e ele mede a proximidade pelo hit mais proximo. Uma sonda ja decimada daria
    // esse minimo sobre uma amostragem angular mais grossa — viesado PARA LONGE, o que a faz cair
    // mais um degrau, e outro. Com relocacao ligada a classificacao roda 180 frames SEGUIDOS no
    // comeco da cena: tempo de sobra para o grid inteiro escorregar ate o piso, e o A/B mediria
    // esse escorregamento em vez do que o knob faz. Nestes frames o trace ignora a contagem e
    // manda os 64, entao o classificador sempre decide sobre o conjunto cheio.
    const bool classifyFrame = MiscParams3.z > 0.5f;
    int rayCount = classifyFrame ? DDGI_RAYS : (int)ProbeRayCount[probeIdx];
    int stride   = DDGI_RAYS / max(rayCount, 1);
    if ((rayIdx % max(stride, 1)) != 0) {
        ProbesTrace[DDGI_TraceTexel(probeIdx, rayIdx, numProbes, DDGI_RAYS)] =
            float4(0.0f, 0.0f, 0.0f, DDGI_RAY_UNUSED);
        return;
    }

    float3 probePos = DDGI_ProbeWorldPos(pc, gridMin, spacing) + GIProbeData[probeIdx].xyz;

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
    // A GROSSA, e nao a cascata desta sonda: o P.GridMin/P.Spacing alimenta o DDGI_VolumeWeight
    // sobre o ponto de HIT, que pode cair em qualquer lugar da cena — quem escolhe a cascata la
    // e o gather. Usar o espacamento da sonda que disparou o raio mediria a borda do volume com
    // a regua errada.
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
    // A sonda integra o hit num cosseno e serve a hemisferio inteiro: cache nao-direcional pelo
    // mesmo motivo do reservoir do ReSTIR — ver o bloco no ShadeSurfaceHit.
    P.RoughnessMin       = GIBiasParams.w;
    P.CacheRayRoughness  = -1.0f;
    RC_UNPACK_PARAMS(P);

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
        radiance   = ShadeSky(dir, sunDir, P.SkyIntensity, P);
        signedDist = maxT;
    }

    ProbesTrace[DDGI_TraceTexel(probeIdx, rayIdx, numProbes, DDGI_RAYS)] =
        float4(radiance, signedDist);
}
