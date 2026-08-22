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
    // 6.2b-ii: scroll toroidal, em CELULAS, por cascata (xyz). Espelha o ScrollOffset do
    // FDDGICascadeConstants — o bloco e copiado campo-a-campo, entao a ORDEM e o contrato.
    float4 GICascadeScrollOffset[4];
    // Quanto cada cascata ROLOU desde o ultimo update que rodou, em celulas (xyz), w = rolou.
    // So os passes de update leem: e com ele que DDGI_NewlyExposed decide, em inteiro, se o slot
    // guarda outro ponto do mundo. Ver DDGIConstants::CascadeScrollDelta.
    float4 GICascadeScrollDelta[4];
    float4 ProbeCompactionParams; // x = Trace/Update usam a lista compacta desta passada
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
    // No caminho compacto, o mesmo mapeamento 2D lineariza ITEMS da lista. O indice global da
    // sonda vem do buffer e continua alimentando RNG, atlas e cascata — compactar nao muda a
    // identidade da amostra.
    int   workIdx   = DDGI_ProbeFromGroup(Gid.xy, max(workProbes, 1));
    int   rayIdx    = (int)GTid.x;
    if (workIdx >= workProbes) return;
    int   probeIdx  = compact ? (int)ActiveProbeIndices[workIdx] : workIdx;
    if (probeIdx >= updateProbes) return;

    int3  count   = (int3)GridCountRays.xyz;
    // Indice GLOBAL -> (cascata, indice local). A GEOMETRIA da sonda — coordenada no grid, origem
    // e espacamento — e toda da cascata dela; o GridMinSpacing do cbuffer e o da GROSSA e serviria
    // so por coincidencia enquanto existe uma cascata.
    int   cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    int   localIdx = DDGI_LocalProbeIndex(probeIdx, count);
    float3 gridMin = GICascadeGridMinSpacing[cascade].xyz;
    float spacing  = GICascadeGridMinSpacing[cascade].w;
    // ARMAZENAMENTO -> GEOMETRIA. `localIdx` enderreca um SLOT; com scrolling, o ponto do mundo que
    // ele representa neste frame sai desfazendo o scroll. Ler a geometria direto do slot daria uma
    // sonda deslocada de `scroll` celulas — parada isso e um erro fixo, andando e um rastro.
    int3  scroll = (int3)GICascadeScrollOffset[cascade].xyz;
    int3  pc     = DDGI_GeometricCoord(DDGI_ProbeCoord(localIdx, count), scroll, count);
    // Lamina recem-exposta: o slot guardava outro lugar do mundo, entao TUDO nele e lixo — inclusive
    // o offset de relocacao que este passe usaria como origem dos raios.
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);

    // Frame de CLASSIFICACAO: o passe de relocacao le ESTE trace para decidir quantos raios cada
    // sonda merece, e ele mede a proximidade pelo hit mais proximo. Uma sonda ja decimada daria
    // esse minimo sobre uma amostragem angular mais grossa — viesado PARA LONGE, o que a faz cair
    // mais um degrau, e outro. Com relocacao ligada a classificacao roda 180 frames SEGUIDOS no
    // comeco da cena: tempo de sobra para o grid inteiro escorregar ate o piso, e o A/B mediria
    // esse escorregamento em vez do que o knob faz. Nestes frames o trace ignora a contagem e
    // manda os 64, entao o classificador sempre decide sobre o conjunto cheio.
    //
    // O `.w` restringe isso ao caso do SCROLL: ali o relocate so reclassifica a lamina nova, entao
    // so ela precisa dos 64. Mandar os 64 no grid inteiro a cada celula andada neutralizaria os
    // raios adaptativos justamente em movimento; e reclassificar o grid inteiro a partir da
    // decimacao dele reabriria a catraca acima pela porta do scroll.
    const bool scrollOnly    = MiscParams3.w > 0.5f;
    const bool classifyFrame = MiscParams3.z > 0.5f && !scrollOnly;
    // A lamina nova pede os 64 SEMPRE, e nao so quando `scrollOnly`: durante os 180 frames iniciais
    // uma rolagem chega com z=1 e w=0, e a sonda nova continua precisando do conjunto cheio.
    //
    // A sonda marcada como recem-relocada (w >= 1) tambem: o relocate vai reclassifica-la NESTE
    // frame, a partir DESTE trace, e ela e a que acabou de trocar de posicao. Deixa-la decimada
    // aqui alimentaria o classificador com a propria decimacao — a catraca da fase 4, pela porta
    // do segundo tempo da lamina.
    const bool justRelocated = GIProbeData[probeIdx].w >= 1.0f;
    int rayCount = (classifyFrame || newlyExposed || justRelocated)
                 ? DDGI_RAYS : (int)ProbeRayCount[probeIdx];
    int stride   = DDGI_RAYS / max(rayCount, 1);
    if ((rayIdx % max(stride, 1)) != 0) {
        ProbesTrace[DDGI_TraceTexel(probeIdx, rayIdx, numProbes, DDGI_RAYS)] =
            float4(0.0f, 0.0f, 0.0f, DDGI_RAY_UNUSED);
        return;
    }

    // O offset de relocacao da lamina nova pertence ao lugar ANTIGO. Usa-lo aqui poe a origem dos
    // raios a ate 0,45 celula de onde a sonda esta, e como o update dessa sonda vai tomar a
    // estimativa INTEIRA (histerese 0, ela nao tem historico), o erro entra sem nada que o dilua —
    // pior do que nao ter limpado nada. Ate o relocate deste mesmo frame reescrever o buffer, a
    // sonda nova traca do vertice do grid.
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
