#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

#include "DDGICommon.hlsli"
#include "../LightsCommon.hlsli"
#include "../BRDF.hlsli"
#include "../RayEpsilons.hlsli"
#include "RadianceCache.hlsli"

// Contrato de bindings (declarados pelo shader que inclui): Scene, Instances, SkyViewLUT,
// IrradAtlas, GIDistAtlas, GIProbeData, LinearClamp/Wrap e — F5 — SceneLights
// (StructuredBuffer<FPunctualLight> com TODAS as luzes ativas, sem frustum cull). VB/IB vem
// bindless via InstanceGeo (ResourceDescriptorHeap), nao ha mais Vertices/Indices globais.
// Contrato de CBUFFER: RayEpsA/RayEpsB declarados no b0 (ver RayOffset.hlsli) — daqui saem o
// piso do ShadowRayBias (RayEpsA.w) e o TMin dos shadow rays (RayEpsB.x) — mais GIDistParams
// (x=tile, y=W, z=H do atlas de distancia, w=skipMode) e GIBiasParams (x=escala do bias,
// y=teto em metros), que alimentam o gather completo do 2o bounce (ver ShadeSurfaceHit).
struct FHitShadeParams {
    float3 GridMin;       float Spacing;
    int3   Count;         int   AtlasTile;
    float2 AtlasInvSize;
    float3 SunDir;        float SunIntensity;
    float3 SunColor;      float ShadowRayBias; // origem dos shadow rays no hit (anti-acne;
                                               // 0.2 historico — calibrar vs offset robusto)
    float  SkyIntensity;  float MaxRayDist;
    float  AlbedoLOD;
    int    NumLights;     // luzes puntuais no SceneLights (F5)
    uint   ShadowRayMask; // instance mask dos shadow rays: GATHER = folhagem sombreia (alpha-test
                          // por candidato); OPAQUE = pula folhagem (rapido, traversal pura).
                          // Nenhum dos dois inclui TRANSLUCENT — vidro nao faz sombra dura.
    float3 ReGIRGridMin;       uint ReGIRSlotsPerCell;
    float3 ReGIRInvCellSize;   bool ReGIREnabled;
    int3   ReGIRGridCount;     int  ReGIRSampleCount;
    uint   ReGIRSlotsSRV;      uint ReGIRAverageSRV;
    uint   FrameIndex;         uint ReGIRPad;
    // Parameterizacao do sky-view LUT (ver ShadeSky). Vem do SkyParams do CB de cada dono, que
    // por sua vez vem do FAtmosphere::ViewHeightKm()/BottomRadiusKm() — a MESMA fonte que o
    // bake usa. Eram dois literais aqui, e o de view height estava errado.
    float  SkyViewHeightKm;    float SkyBottomRKm;
    // Piso de roughness do hit — SO para quem guarda a radiancia num cache NAO-DIRECIONAL
    // (DDGI e ReSTIR GI). 0 = desligado, que e o que as reflexoes usam. Ver o uso abaixo.
    float  RoughnessMin;       float CacheRayRoughness;
    // World radiance cache. CacheRayRoughness e a roughness do lobo que GEROU este raio, nao a
    // do material atingido. Negativo = transporte difuso; reflexoes usam o gate de cone SHaRC.
    FRadianceCacheParams Cache;
};

// Requer SceneLights e o heap bindless declarados pelo shader hospedeiro.
#include "ReGIRSampling.hlsli"

// A parameterizacao do sky-view LUT era reescrita a mao aqui, com o raio do planeta e o view
// height como literais — o unico jeito, porque este header nao pode incluir o
// AtmosphereCommon.hlsli (o `cbuffer AtmosphereCB : register(b0)` de la colide com o b0 proprio
// de cada shader de trace). A copia divergiu: 6360.5 contra os 6360.001 do bake, 0,49 grau de
// erro no dip do horizonte = ~3,8 texels dos 104 do eixo V, justo na banda mais brilhante e de
// maior gradiente do LUT. GI e reflexoes liam o ceu errado no horizonte.
//
// Agora a conta vem do AtmosphereMath.hlsli (matematica pura, sem cbuffer) e os dois numeros
// chegam por FHitShadeParams. Passar floats soltos sem o header comum consertaria o sintoma de
// hoje e deixaria a formula divergir de novo na proxima edicao da parameterizacao.
#include "../Atmosphere/AtmosphereMath.hlsli"

float3 ShadeSky(float3 dir, float3 sunDir, float skyIntensity, FHitShadeParams P) {
    float2 uv = AtmoWorldDirToSkyViewUv(dir, sunDir, P.SkyViewHeightKm, P.SkyBottomRKm);
    return SkyViewLUT.SampleLevel(LinearClamp, uv, 0.0f).rgb * skyIntensity;
}

// Alpha-test dos candidatos do RayQuery (AlphaTestPass + SMILE_RT_PROCEED) — extraido p/
// RTAlphaTest.hlsli p/ passes de visibilidade pura poderem incluir so ele (ReSTIRGISpatial).
#include "RTAlphaTest.hlsli"

// Normal de FACE / de shading no ponto de hit (HitFaceNormal, HitGeomNormal). Vivia aqui;
// mudou-se p/ o RTGeometry.hlsli quando o BvhDebug passou a precisar da geometria do hit sem a
// iluminacao. Move puro — mesmas funcoes, mesmo contrato de bindings (Instances + heap
// diretamente indexado), ja satisfeito acima.
#include "RTGeometry.hlsli"

// Os BLOCOS que o ShadeSurfaceHit compoe — geometria, material, direta, fallback indireto. Vem
// DEPOIS de tudo acima porque depende do FHitShadeParams (declarado neste arquivo) e do
// ReGIRSampling/RTGeometry/BRDF que os includes anteriores trouxeram.
//
// O que ficou aqui e a POLITICA: em que ordem os blocos entram, onde o cache corta o caminho e o
// que o resultado alimenta. E exatamente essa separacao que a Fase 3 precisa — o path tracer de
// update reusa os blocos com outra politica, em vez de copiar 290 linhas que divergiriam no
// primeiro fix de material.
#include "PathTracingCommon.hlsli"

// CLASSIFICACAO do hit, sem sombrear — a politica de auto-interseccao precisa decidir se
// re-traca ANTES de pagar o shading, e o caller e quem escolhe o que fazer (o ReSTIR mata o
// caminho, o DDGI usa o outSignedDist do ShadeSurfaceHit, as reflexoes tem politica propria).
// Por isso isto NAO vive dentro do ShadeSurfaceHit, que e compartilhado pelos tres.
//
// `outTwoSided` sai junto porque a politica trata os dois casos com distancias diferentes.
// Custo: os 3 indices + as 3 posicoes do triangulo — o mesmo que o HitGeomNormal ja faz.
bool HitIsBackface(uint instId, uint tri, float3x4 worldToObject, float3 rayDir,
                   out bool outTwoSided) {
    InstanceGeo geo = Instances[instId];
    outTwoSided = (geo.TwoSidedRT != 0);

    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    float3 faceN;
    if (!HitFaceNormal(Verts, Idx[tri * 3 + 0], Idx[tri * 3 + 1], Idx[tri * 3 + 2],
                       worldToObject, faceN))
        return false; // degenerado: nao da p/ afirmar que e verso
    return dot(faceN, rayDir) > 0.0f;
}

// A POLITICA do hit de render: em que ordem os blocos entram, onde o cache corta o caminho e o que
// o resultado alimenta. Os blocos moram no PathTracingCommon.hlsli.
//
// A assinatura e a sequencia sao as mesmas de quando isto era uma funcao de ~290 linhas — os seis
// call sites (DDGITrace, ReSTIRGITrace x2, ReflectionTrace, ReflectionTraceMirror,
// WaterReflectionTrace) nao mudaram uma linha. O que mudou e que a POLITICA agora e legivel de uma
// vez, e o path tracer da Fase 3 pode escrever a DELE reusando os mesmos blocos.
float3 ShadeSurfaceHit(uint instId, uint tri, float2 bary, float3x4 worldToObject,
                       float3 rayOrigin, float3 rayDir, float hitDist,
                       FHitShadeParams P, out float outSignedDist) {
    InstanceGeo geo = Instances[instId];

    const FHitSurface S = PT_LoadHitSurface(geo, tri, bary, worldToObject,
                                            rayOrigin, rayDir, hitDist);
    outSignedDist = S.SignedDist;

    // === WORLD RADIANCE CACHE — consulta ====================================================
    // O ponto mais cedo em que da para sair: a normal geometrica ja esta resolvida (e a chave do
    // hash precisa dela) e nada caro aconteceu ainda. Sair aqui pula a amostragem de albedo/MR,
    // o loop de luzes com shadow ray, o gather do DDGI e o emissivo — que e todo o custo.
    //
    // Esta ordem e CONTRATO, nao arrumacao: adiantar o material para "organizar melhor" jogaria
    // fora justamente a economia que o cache existe para dar.
    const FPathState Path = PT_MakePathState(rayOrigin, rayDir, hitDist, P.CacheRayRoughness);
    {
        const FRCQueryResult Q = RC_QueryEx(P.Cache, S.Pos, S.CacheN,
                                            Path.SegmentLength, Path.RayRoughness);
        if (RC_QueryHit(Q)) return Q.Radiance; // outSignedDist ja foi escrito acima
    }

    // DEPOIS do miss, e nao antes: daqui para baixo e o caminho caro. O `V` entra aqui pelo mesmo
    // motivo — um normalize no fast path e pequeno, mas o ponto do retorno antecipado e nao pagar
    // NADA, e com hit rate de ~78% "pequeno" acontece em quatro de cada cinco consultas.
    const float3 V = PT_ViewDir(Path);
    const FHitMaterial M = PT_LoadHitMaterial(geo, S.UV, P.AlbedoLOD, P.RoughnessMin);

    float3 directLighting = PT_ShadeDirectSun(S, M, V, P);
    PT_AddDirectLocal(S, M, V, P, directLighting);

    // Fallback indireto. Na Fase 5 e ESTA linha que troca de fonte — o cache passa a responder
    // primeiro e o DDGI fica so no ramo de miss. Hoje ela e o unico terminador.
    const float3 indirect = PT_SampleIndirectFallback(S, rayDir, P);

    const float3 emissive = PT_LoadHitEmissive(geo, S.UV, P.AlbedoLOD);

    const float3 indirectLighting = PT_ComposeIndirect(S, M, V, indirect);
    const float3 outRadiance = directLighting + indirectLighting + emissive;

    // === WORLD RADIANCE CACHE — atualizacao =================================================
    // Quem escreve e decidido no CPU, pelo bit de update do Flags: DDGI e ReSTIR GI recebem, as
    // reflexoes nao (radiancia direcional). Ver FRadianceCache::ShaderParams.
    //
    // E ESTE o "produtor legado" que o plano manda manter ate a Fase 3 existir: o cache aprende
    // dos hits do render, e nao de um passe proprio. O bit ja e a flag que o desliga — quando o
    // updater dedicado entrar, o CPU para de arma-lo aqui e esta linha sai sem cirurgia.
    RC_Update(P.Cache, S.Pos, S.CacheN, outRadiance);

    return outRadiance;
}

#endif
