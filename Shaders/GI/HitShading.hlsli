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

float3 ShadeSurfaceHit(uint instId, uint tri, float2 bary, float3x4 worldToObject,
                       float3 rayOrigin, float3 rayDir, float hitDist,
                       FHitShadeParams P, out float outSignedDist) {
    float3 hitPos = rayOrigin + rayDir * hitDist;

    InstanceGeo geo    = Instances[instId];
    float3      albedo = geo.BaseColor.rgb;

    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    uint   i0   = Idx[tri * 3 + 0];
    uint   i1   = Idx[tri * 3 + 1];
    uint   i2   = Idx[tri * 3 + 2];
    float3 nObj = Verts[i0].Normal * (1.0f - bary.x - bary.y)
                + Verts[i1].Normal * bary.x
                + Verts[i2].Normal * bary.y;
    float3 nWrld = mul(nObj, (float3x3)worldToObject);
    float  nLen  = length(nWrld);
    float3 geomN = (nLen > 1e-5f) ? (nWrld / nLen) : normalize(-rayDir);

    // FACING — duas perguntas DIFERENTES que antes compartilhavam a mesma variavel `backface`:
    //
    //  (1) "o raio veio pelo verso desta face?" — pergunta puramente geometrica, responde se a
    //      normal de SHADING precisa ser virada. Vale para QUALQUER material: uma folha ou uma
    //      cortina atingida por tras tem que iluminar pelo lado de tras. O raster ja faz isso sem
    //      gate nenhum (GBuffer.ps.hlsl: `if (!input.frontFace) GeoN = -GeoN;`) e o Lumen tambem
    //      (LumenHardwareRayTracingCommon.ush: vira a normal quando IsTwoSided && !IsFrontFace).
    //      Com o gate `TwoSided == 0` que existia aqui, folhagem/cortina atingida por tras ficava
    //      com a normal apontando p/ LONGE do raio: N.L errado, Lo errado, e o sample do DDGI
    //      lido do lado errado da superficie.
    //
    //  (2) "este hit significa que o raio esta DENTRO de geometria solida?" — pergunta de
    //      topologia, e so ela justifica o gate por TwoSided. Alimenta o outSignedDist, que o
    //      DDGITrace usa p/ encurtar a distancia (0.2x) e deixar o Chebyshev escurecer probes
    //      enterradas. Bater no verso de uma FOLHA nao quer dizer estar dentro de nada — por isso
    //      material two-sided continua reportando distancia POSITIVA. O gate fica aqui.
    //
    // O teste sai da normal de FACE (nao da interpolada): em malha suavizada a interpolada erra o
    // sinal perto da silhueta, e era essa impressao que o gate mascarava.
    float3 faceN;
    const bool faceOk       = HitFaceNormal(Verts, i0, i1, i2, worldToObject, faceN);
    const bool hitFromBehind = faceOk ? (dot(faceN,  rayDir) > 0.0f)
                                      : (dot(geomN,  rayDir) > 0.0f);
    outSignedDist = (geo.TwoSidedRT == 0 && hitFromBehind) ? -hitDist : hitDist;

    // Normal de FACE, so p/ a ORIGEM dos shadow rays. A interpolada (hitN, abaixo) continua
    // mandando na BRDF, no N.L e no sample do DDGI — o offset e um problema de escapar do PLANO
    // do triangulo, e quem descreve esse plano e a face; a normal de vertice/normal map descreve
    // a aparencia. Era a interpolada que deslocava a origem, entao em malha suavizada sobrava
    // componente tangencial e o bias precisava ser grande p/ compensar.
    //
    // Orientada CONTRA o raio incidente, igual ao HitGeomNormal (e nao "p/ o lado da saida"):
    // como o shadow ray so e tracado quando N.L > 0, a luz esta do mesmo lado de onde o raio veio.
    // Nos casos raros em que a interpolada e a face discordam (luz abaixo do horizonte geometrico
    // mas acima do de shading), virar a face p/ a luz empurraria a origem ATRAVES da superficie —
    // o remedio ali e bias modulado por angulo, que entra no sweep da rodada 3.
    //
    // Reusa o facing (1) ja calculado — a face e a orientacao sao as mesmas do teste acima.
    float3 offsetN = faceOk ? (hitFromBehind ? -faceN : faceN)
                            : (hitFromBehind ? -geomN : geomN); // degenerado: interpolada

    float2 uv = Verts[i0].TexCoord * (1.0f - bary.x - bary.y)
              + Verts[i1].TexCoord * bary.x
              + Verts[i2].TexCoord * bary.y;

    // === WORLD RADIANCE CACHE — consulta ====================================================
    // O ponto mais cedo em que da para sair: a normal geometrica ja esta resolvida (e a chave do
    // hash precisa dela) e nada caro aconteceu ainda. Sair aqui pula a amostragem de albedo/MR,
    // o loop de luzes com shadow ray, o gather do DDGI e o emissivo — que e todo o custo.
    //
    // A normal da CHAVE e a geometrica com facing aplicado: a chave tem de ser uma propriedade da
    // SUPERFICIE. Com a direcao do raio na chave, cada raio cairia numa celula diferente e o
    // cache nunca acertaria.
    const float3 cacheN = hitFromBehind ? -geomN : geomN;
    {
        float3 cachedRadiance;
        if (RC_Query(P.Cache, hitPos, cacheN, hitDist, P.CacheRayRoughness, cachedRadiance))
            return cachedRadiance; // outSignedDist ja foi escrito acima
    }

    float3 hitN = hitFromBehind ? -geomN : geomN; // facing (1): sem gate, igual ao raster
    if (geo.HasAlbedo != 0) {
        Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
        albedo *= albedoTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
    }

    // Metallic/roughness seguem o mesmo workflow do G-buffer. O MR e amostrado uma vez para os
    // dois parametros; mapas separados ocupam os slots +6/+7.
    float metallic  = geo.EmissiveFactor.w; // MetallicFactor cabe no .w do snapshot
    float roughness = geo.RoughnessFactor;
    if ((geo.Flags & INSTGEO_FLAG_MRMAP) != 0u) {
        Texture2D<float4> mrTex = ResourceDescriptorHeap[geo.MrMapIndex];
        const float4 mr = mrTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD);
        metallic  *= ((geo.Flags & INSTGEO_FLAG_SPECPACK) != 0u) ? mr.b : mr.r;
        roughness *= mr.g;
    }
    if ((geo.Flags & INSTGEO_FLAG_METALMAP) != 0u) {
        Texture2D<float4> metalTex = ResourceDescriptorHeap[geo.MetalMapIndex];
        metallic *= metalTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).r;
    }
    if ((geo.Flags & INSTGEO_FLAG_ROUGHMAP) != 0u) {
        Texture2D<float4> roughTex = ResourceDescriptorHeap[geo.RoughMapIndex];
        roughness *= roughTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).r;
    }
    metallic = saturate(metallic);
    // Piso de roughness do SECUNDARIO (RTXDI `minSecondaryRoughness`, default 0.5 no sample
    // app; Doc/RestirGI.md: "the BRDF of secondary surfaces should be limited [...] such as by
    // clamping the roughness to higher values").
    //
    // O motivo e de armazenamento, nao de aparencia: DDGI e ReSTIR GI guardam UM float3 de
    // radiancia por hit e o reusam de outras direcoes — a sonda integra o hit num cosseno e o
    // reservoir entrega o mesmo Lo a vizinhos que olham o ponto de outro angulo. Um lobo GGX
    // estreito no hit e, por definicao, radiancia que so existe naquela direcao: sem o piso ele
    // vira firefly no reuso, e ai quem "conserta" e o FireflyMax (4/8), que tira energia.
    // Com o piso o mesmo brilho vira um lobo largo — perde o highlight nitido do 2o bounce
    // (que nenhum dos dois caches sabe representar) e devolve a energia distribuida.
    //
    // As REFLEXOES passam 0 aqui de proposito: la o hit e sombreado p/ uma direcao de visada
    // conhecida e consumido uma vez so, sem reuso — clampar borraria espelho-no-espelho.
    // O 0.04 continua embaixo como piso numerico do GGX, valha o que valer o knob.
    roughness = max(roughness, max(P.RoughnessMin, 0.04f));

    const float3 diffuseColor  = albedo * (1.0f - metallic);
    const float3 specularColor = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float  alpha = roughness * roughness;
    const float  a2 = alpha * alpha;
    const float3 hitV = normalize(-rayDir);

    float ndl = saturate(dot(hitN, P.SunDir));
    float vis = 1.0f;
    if (ndl > 0.0f) {
        RayDesc sray;
        sray.Origin    = hitPos + offsetN * max(P.ShadowRayBias, RayEpsA.w);
        sray.Direction = P.SunDir; // direcional: a direcao nao depende da origem deslocada
        sray.TMin      = RayEpsB.x;
        sray.TMax      = P.MaxRayDist;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, sray);
        SMILE_RT_PROCEED(sq)
        vis = (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
    }
    float3 directLighting = BRDF_Direct(
        hitN, hitV, P.SunDir, P.SunColor * P.SunIntensity * vis,
        diffuseColor, specularColor, roughness, a2, 0.0f);

    // ReGIR substitui o loop O(N) por 8 propostas do pool da celula + UM shadow ray. O sol fica
    // dedicado acima. Fora da grade (ou com o toggle off), o loop historico permanece como
    // referencia exata e fallback funcional.
    uint regirLight;
    float3 regirEstimate;
    bool regirHandled = false;
    [branch] if (P.ReGIREnabled) {
        regirHandled = ReGIRSelectPunctual(
            hitPos, hitN, hitV, diffuseColor, specularColor, roughness, a2,
            P.ReGIRGridMin, P.ReGIRInvCellSize, P.ReGIRGridCount,
            P.ReGIRSlotsPerCell, (uint)P.ReGIRSampleCount, P.ReGIRSlotsSRV,
            P.ReGIRAverageSRV, P.FrameIndex, (uint)P.NumLights,
            regirLight, regirEstimate);
    }

    if (regirHandled && regirLight != REGIR_INVALID_LIGHT) {
        const float3 lorg = hitPos + offsetN * max(P.ShadowRayBias, RayEpsA.w);
        const float3 toL = SceneLights[regirLight].PosInvRadius.xyz - lorg;
        const float lenL = max(length(toL), 1e-4f);
        const float lTMax = lenL - kLightRayEndMargin;
        if (lTMax <= RayEpsB.x + kLightRayMinTMax) {
            directLighting += regirEstimate;
        } else {
            RayDesc lray;
            lray.Origin = lorg; lray.Direction = toL / lenL;
            lray.TMin = RayEpsB.x; lray.TMax = lTMax;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
            lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                              P.ShadowRayMask, lray);
            SMILE_RT_PROCEED(lq)
            if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                directLighting += regirEstimate;
        }
    } else if (!regirHandled) {
      [loop] for (int li = 0; li < P.NumLights; ++li) {
        float3 Ll; float distL;
        float3 contrib = HitPunctualBRDF(SceneLights[li], hitPos, hitN, hitV,
                                         diffuseColor, specularColor, roughness, a2,
                                         Ll, distL);
        if (dot(contrib, float3(0.2126f, 0.7152f, 0.0722f)) < 1e-3f) continue;

        // Segmento medido da origem EFETIVA (deslocada pelo ShadowRayBias): com origem em
        // hitPos+N*b mas direcao/TMax calculados de hitPos, origem/direcao/comprimento
        // descreviam segmentos diferentes — com luz proxima (b=0.2!) o erro angular e grande.
        // O shading (contrib) continua medido do hitPos real; so o raio usa o segmento efetivo.
        float3 lorg = hitPos + offsetN * max(P.ShadowRayBias, RayEpsA.w);
        float3 toL  = (hitPos + Ll * distL) - lorg;
        float  lenL = max(length(toL), 1e-4f);

        // TMax NUNCA passa da luz — o piso antigo (kLightRayMinTMax) podia empurra-lo p/ ALEM
        // dela e, pior, deixa-lo ABAIXO do TMin: com ShadowRayTMin virando knob (ate 50 mm) e uma
        // luz a poucos centimetros, saia TMin 50 mm > TMax 20 mm = intervalo invalido no DXR.
        // Agora o segmento e o real e, se nao sobrar corpo util entre TMin e TMax, a luz conta
        // como VISIVEL (o trecho nao testado e menor que os proprios epsilons).
        float lTMax = lenL - kLightRayEndMargin;
        if (lTMax <= RayEpsB.x + kLightRayMinTMax) {
            directLighting += contrib;
            continue;
        }

        RayDesc lray;
        lray.Origin    = lorg;
        lray.Direction = toL / lenL;
        lray.TMin      = RayEpsB.x;
        lray.TMax      = lTMax;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
        lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, lray);
        SMILE_RT_PROCEED(lq)
        if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) directLighting += contrib;
      }
    }

    // 2o bounce com o gather COMPLETO — Chebyshev, bias de superficie, offset de relocacao e
    // skip de sonda inativa —, o mesmo que o deferred usa. Antes aqui era a trilinear pura, e
    // isso era o pior lugar possivel para vazar: o resultado do hit volta para o atlas do DDGI,
    // que reamostra com hysteresis 0,99, entao o leak se REALIMENTA frame a frame. O Flax
    // tambem usa o sampler completo no bounce do surface atlas.
    //
    // O papel de "direcao da camera" no bias e do raio: quem observa este ponto e a origem do
    // raio, entao V = -rayDir. Sem isso o termo de view empurraria o ponto para uma direcao sem
    // relacao com a visada e o bias perderia o sentido geometrico.
    // O bias em si e calculado DENTRO do wrapper, uma vez por cascata — ele escala com o
    // espacamento e no blend as duas cascatas querem valores diferentes. Aqui sobra so a regra de
    // quem e o "V": a direcao da camera no bias e a do RAIO, porque quem observa este ponto e a
    // origem dele. Por isso o wrapper recebe -rayDir.
    float2 distInvSize = float2(1.0f / GIDistParams.y, 1.0f / GIDistParams.z);
    // Fora do volume o gather clampa e estende a ultima fileira de sondas ao infinito. Na tela
    // isso e substituido pelo ambiente hemisferico; aqui a radiancia do hit REALIMENTA o atlas,
    // entao extrapolar seria pior — a borda se reinjetaria. Sem o ambiente colorido do deferred
    // (nao existe no CB de um passe de RT), o hit desvanece o indireto para ZERO: escurece o
    // bounce distante em vez de inventar luz. O direto do hit (sol e puntuais) segue inteiro.
    // GIDistParams.w carrega DUAS coisas (ver FGIHitSampling::SkipModePacked): o skipMode de
    // sonda inativa nos dois bits baixos e, no bit 2, o gate de MEDICAO — "corte o DDGI como
    // terminador e me diga quanto muda". Com o gate ligado nao ha gather nenhum: o volW vai a
    // zero e o custo do tap tambem sai, o que mantem a medicao honesta tambem no profiler.
    const uint giHitFlags = (uint)GIDistParams.w;
    const bool termOff    = (giHitFlags & 4u) != 0u;
    float volW = termOff ? 0.0f
                         : DDGI_VolumeWeight(hitPos, P.GridMin, P.Spacing, P.Count, GIBiasParams.z);
    float3 indirect = float3(0.0f, 0.0f, 0.0f);
    if (volW > 0.0f) {
        // Tiles por linha (empacotamento 2D; ver DDGI_TileOrigin) derivado do par
        // (largura, tile) do atlas de distancia, que ja chega aqui pelo contrato de NOME do
        // GIDistParams — os cinco shaders que incluem este arquivo o declaram, entao nao ha
        // campo novo a plumbar nem estado duplicado a dessincronizar.
        const int tilesPerRow = DDGI_TilesPerRow(GIDistParams.y, (int)GIDistParams.x);
        // Wrapper com selecao e blend de cascata. O bias NAO entra pronto — ver a nota no
        // wrapper: ele escala com o espacamento e cada cascata quer o seu. O `hitBias` calculado
        // acima some por isso.
        //
        // O fallback deste caller e PRETO, e nao ambiente hemisferico: nao existe cor de ambiente
        // no cbuffer de um passe de RT. Por isso o volW continua aqui fora, e o wrapper devolve
        // so a irradiancia do DDGI.
        indirect = SampleDDGIIrradianceChebCascaded(
            IrradAtlas, GIDistAtlas, LinearClamp, hitPos, hitN, -rayDir,
            GICascadeGridMinSpacing, GICascadeScrollOffset, (int)GICascadeParams.x, P.Count,
            P.AtlasTile, P.AtlasInvSize, (int)GIDistParams.x, distInvSize,
            GIProbeData, giHitFlags & 3u, tilesPerRow,
            GIBiasParams.x, GIBiasParams.y);
        if (volW < 1.0f) indirect *= volW;
    }

    // Emissivo do hit (mesma formula do GBuffer.ps: factor*strength ja bakeado no InstanceGeo,
    // x mapa quando ha) — sem isto, superficies emissivas nao alimentam GI nem aparecem em
    // reflexoes/ReSTIR. O mapa e obrigatorio quando existe (factor costuma ser 1 e o mapa e
    // quase todo preto — so o factor estouraria a superficie inteira).
    float3 emissive = geo.EmissiveFactor.rgb;
    if ((geo.Flags & INSTGEO_FLAG_EMISSIVE) != 0u) {
        Texture2D<float4> emissiveTex = ResourceDescriptorHeap[geo.EmissiveMapIndex];
        emissive *= emissiveTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
    }

    // O atlas fornece irradiancia difusa, nao uma distribuicao direcional que permita integrar
    // GGX de verdade. O fallback split-sum usa Fresnel roughness-aware e reserva (1-F) para o
    // difuso: metal devolve energia tingida por F0 sem fingir que o atlas conhece a direcao de
    // espelho. O direto acima continua sendo a BRDF GGX direcional completa.
    const float NoV = saturate(dot(hitN, hitV));
    const float3 ambientF = F_SchlickRoughness(specularColor, NoV, roughness);
    const float3 indirectLighting = ((1.0f - ambientF) * diffuseColor + ambientF) * indirect;

    const float3 outRadiance = directLighting + indirectLighting + emissive;

    // === WORLD RADIANCE CACHE — atualizacao =================================================
    // Quem escreve e decidido no CPU, pelo bit de update do Flags: DDGI e ReSTIR GI recebem, as
    // reflexoes nao (radiancia direcional). Ver FRadianceCache::ShaderParams.
    RC_Update(P.Cache, hitPos, cacheN, outRadiance);

    return outRadiance;
}

#endif
