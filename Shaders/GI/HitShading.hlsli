#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

#include "DDGICommon.hlsli"
#include "../LightsCommon.hlsli"
#include "../RayEpsilons.hlsli"

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
    bool   RealHitShading;
    int    NumLights;     // luzes puntuais no SceneLights (F5)
    uint   ShadowRayMask; // instance mask dos shadow rays: GATHER = folhagem sombreia (alpha-test
                          // por candidato); OPAQUE = pula folhagem (rapido, traversal pura).
                          // Nenhum dos dois inclui TRANSLUCENT — vidro nao faz sombra dura.
};

static const float kSkyBottomR = 6360.0f;
static const float kSkyViewH   = 6360.5f;

float2 DDGI_SkyViewUv(float viewZenithCos, float lightViewCos) {
    float vHorizon = sqrt(max(0.0f, kSkyViewH * kSkyViewH - kSkyBottomR * kSkyBottomR));
    float cosBeta  = vHorizon / max(kSkyViewH, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = SMILE_PI - beta;

    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0f, 1.0f));
    float u, v;
    if (viewZenithAngle < zenithHorizonAngle) {
        float coord = 1.0f - viewZenithAngle / max(zenithHorizonAngle, 1e-4f);
        coord = 1.0f - sqrt(max(0.0f, coord));
        v = coord * 0.5f;
    } else {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-4f);
        v = sqrt(max(0.0f, coord)) * 0.5f + 0.5f;
    }
    u = sqrt(max(0.0f, -lightViewCos * 0.5f + 0.5f));
    return float2(u, v);
}

float3 ShadeSky(float3 dir, float3 sunDir, float skyIntensity) {
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float viewZenithCos = dot(dir, up);
    float3 viewHoriz = dir    - up * viewZenithCos;
    float3 sunHoriz  = sunDir - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));
    float2 uv = DDGI_SkyViewUv(viewZenithCos, lightViewCos);
    return SkyViewLUT.SampleLevel(LinearClamp, uv, 0.0f).rgb * skyIntensity;
}

// Alpha-test dos candidatos do RayQuery (AlphaTestPass + SMILE_RT_PROCEED) — extraido p/
// RTAlphaTest.hlsli p/ passes de visibilidade pura poderem incluir so ele (ReSTIRGISpatial).
#include "RTAlphaTest.hlsli"

// Normal de FACE (orientada contra o raio) no ponto de hit — usada pelo ReSTIR GI como n2 no
// Jacobiano de reconexao. Aditivo; nao altera ShadeSurfaceHit (caminho das reflexoes intacto).
//
// Era a normal INTERPOLADA de vertice, que e uma normal de SHADING suave: em malha suavizada o
// fator cosDst/cosSrc do Jacobiano saia sistematicamente errado, porque o Jacobiano e definido
// sobre a area real do triangulo (Ouyang 2021 Eq. 11 / GRIS Eq. 52 pedem a geometrica). Agora
// vem do cross das POSICOES; o custo extra e so ler Position dos 3 vertices ja buscados.
//
// Transformacao: cross em espaco de objeto e depois mul(n, WorldToObject) com vetor-linha — que
// e a inversa-transposta de ObjectToWorld, a mesma convencao ja usada pela normal de vertice.
//
// Orientacao: dot(faceN, rayDir) em vez de CommittedTriangleFrontFace(). Com a normal de FACE o
// teste do produto escalar e exato (com a interpolada nao era, e por isso havia o gate TwoSided),
// e nao depende da convencao de winding nem da flag TRIANGLE_FRONT_COUNTERCLOCKWISE da instancia
// — o cross ja herda o winding, entao parear os dois arriscaria inverter o sinal duas vezes. O
// gate `geo.TwoSided == 0` saiu: material two-sided agora tambem tem a normal virada no verso,
// como o raster ja faz em GBuffer.ps.hlsl. (Hoje isso e inerte para o unico consumidor: o
// ReconnectionJacobian usa abs() nos dois cossenos. Fica correto para os proximos.)
// Normal de FACE em espaco de mundo, SEM orientacao — o caller decide o lado. Retorna false em
// triangulo degenerado ou transform singular (o caller cai na normal de vertice).
//
// Teste de degenerado ADIMENSIONAL, feito em espaco de OBJETO e antes da transformacao:
// |cross|^2 / (|e1|^2 |e2|^2) = sin^2(angulo entre as arestas). Comparar o comprimento absoluto
// (ainda mais depois de multiplicar por worldToObject, que reescala por ~1/s) faria o limiar
// depender da escala da instancia e da unidade do asset — instancia muito ampliada cairia no
// fallback sem ser degenerada. sin <= 1e-6 = arestas colineares de verdade; aresta de comprimento
// zero da scale2 = 0 e tambem entra aqui.
bool HitFaceNormal(StructuredBuffer<DDGIVertex> Verts, uint i0, uint i1, uint i2,
                   float3x4 worldToObject, out float3 outFaceN) {
    float3 e1 = Verts[i1].Position - Verts[i0].Position;
    float3 e2 = Verts[i2].Position - Verts[i0].Position;
    float3 nObj = cross(e1, e2);

    float  nObjLen2 = dot(nObj, nObj);
    float  scale2   = dot(e1, e1) * dot(e2, e2);
    float3 nWrld    = mul(nObj, (float3x3)worldToObject);
    float  nLen     = length(nWrld);

    outFaceN = (nLen > 0.0f) ? (nWrld / nLen) : float3(0.0f, 0.0f, 1.0f);
    return (nObjLen2 > 1e-12f * scale2) && (nLen > 0.0f);
}

float3 HitGeomNormal(uint instId, uint tri, float2 bary, float3x4 worldToObject, float3 rayDir) {
    InstanceGeo geo = Instances[instId];
    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    uint i0 = Idx[tri * 3 + 0];
    uint i1 = Idx[tri * 3 + 1];
    uint i2 = Idx[tri * 3 + 2];

    float3 faceN;
    if (!HitFaceNormal(Verts, i0, i1, i2, worldToObject, faceN)) {
        // Degenerado: cai na normal de vertice interpolada.
        float3 vObj = Verts[i0].Normal * (1.0f - bary.x - bary.y)
                    + Verts[i1].Normal * bary.x
                    + Verts[i2].Normal * bary.y;
        float3 vWrld = mul(vObj, (float3x3)worldToObject);
        float  vLen  = length(vWrld);
        faceN = (vLen > 1e-5f) ? (vWrld / vLen) : normalize(-rayDir);
    }
    return (dot(faceN, rayDir) > 0.0f) ? -faceN : faceN;
}

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

    float3 hitN = normalize(-rayDir);
    if (P.RealHitShading) {
        hitN = hitFromBehind ? -geomN : geomN; // facing (1): sem gate, igual ao raster
        if (geo.HasAlbedo != 0) {
            Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
            albedo *= albedoTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
        }
    }

    // METALLIC: o difuso do hit e albedo*(1 - metallic), a mesma convencao do raster
    // (DiffuseColor = BaseColor*(1-Metallic), ver BRDF.hlsli). Sem isto um metal puro
    // fabricava difuso e injetava luz colorida nas probes, nas reflexoes e no ReSTIR.
    //
    // O fator sozinho NAO basta: o loader deixa MetallicFactor = 1 justamente para multiplicar
    // pelo mapa, entao um material texturizado (quase todo dieletrico) apareceria como metal
    // puro. Por isso, quando ha mapa mas nao estamos amostrando textura (RealHitShading off),
    // o mais seguro e nao aplicar nada — errar para o lado do comportamento antigo em vez de
    // apagar o difuso de uma superficie inteira.
    //
    // LIMITE CONHECIDO: o hit continua sem termo ESPECULAR, entao metal passa a contribuir
    // ~zero para o indireto em vez de contribuir errado. E o que o Flax faz no surface atlas
    // (GetDiffuseColor zera o metal). Metal visto dentro de reflexo/GI fica escuro; o conserto
    // e dar especular ao hit, que e outro trabalho.
    {
        const bool hasMetalMap =
            (geo.Flags & (INSTGEO_FLAG_MRMAP | INSTGEO_FLAG_METALMAP)) != 0u;
        float metallic = 0.0f;
        if (P.RealHitShading) {
            metallic = geo.EmissiveFactor.w; // MetallicFactor
            if ((geo.Flags & INSTGEO_FLAG_MRMAP) != 0u) {
                Texture2D<float4> mrTex = ResourceDescriptorHeap[geo.MrMapIndex];
                float4 mr = mrTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD);
                metallic *= ((geo.Flags & INSTGEO_FLAG_SPECPACK) != 0u) ? mr.b : mr.r;
            }
            if ((geo.Flags & INSTGEO_FLAG_METALMAP) != 0u) {
                Texture2D<float4> metalTex = ResourceDescriptorHeap[geo.MetalMapIndex];
                metallic *= metalTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).r;
            }
        } else if (!hasMetalMap) {
            metallic = geo.EmissiveFactor.w; // sem mapa, o fator descreve o material sozinho
        }
        albedo *= saturate(1.0f - metallic);
    }

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
    float3 Edirect = P.SunColor * P.SunIntensity * vis * ndl;

    // F5: luzes puntuais no hit — mesma atenuacao do deferred (LightsCommon) + shadow ray
    // inline SO pra luz que contribui de verdade (a janela de raio + cone ja zeram a
    // maioria por hit). E a visibilidade RT que impede o poste de vazar parede no GI.
    [loop] for (int li = 0; li < P.NumLights; ++li) {
        float3 Ll; float distL;
        float3 contrib = PunctualLightIncoming(SceneLights[li], hitPos, Ll, distL)
                       * saturate(dot(hitN, Ll));
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
        if (lTMax <= RayEpsB.x + kLightRayMinTMax) { Edirect += contrib; continue; }

        RayDesc lray;
        lray.Origin    = lorg;
        lray.Direction = toL / lenL;
        lray.TMin      = RayEpsB.x;
        lray.TMax      = lTMax;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
        lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, lray);
        SMILE_RT_PROCEED(lq)
        if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) Edirect += contrib;
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
    float2 distInvSize = float2(1.0f / GIDistParams.y, 1.0f / GIDistParams.z);
    float3 hitBias = DDGI_SurfaceBias(hitN, -rayDir, P.Spacing,
                                      GIBiasParams.x, GIBiasParams.y);
    float3 indirect = SampleDDGIIrradianceCheb(
        IrradAtlas, GIDistAtlas, LinearClamp, hitPos, hitN,
        P.GridMin, P.Spacing, P.Count, P.AtlasTile, P.AtlasInvSize,
        (int)GIDistParams.x, distInvSize, hitBias, GIProbeData, (uint)GIDistParams.w);

    // Emissivo do hit (mesma formula do GBuffer.ps: factor*strength ja bakeado no InstanceGeo,
    // x mapa quando ha) — sem isto, superficies emissivas nao alimentam GI nem aparecem em
    // reflexoes/ReSTIR. O mapa e obrigatorio quando existe (factor costuma ser 1 e o mapa e
    // quase todo preto — so o factor estouraria a superficie inteira). Sem gate no
    // RealHitShading: o branch e coerente por instancia e a maioria tem flag 0.
    float3 emissive = geo.EmissiveFactor.rgb;
    if ((geo.Flags & INSTGEO_FLAG_EMISSIVE) != 0u) {
        Texture2D<float4> emissiveTex = ResourceDescriptorHeap[geo.EmissiveMapIndex];
        emissive *= emissiveTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
    }

    return albedo * (Edirect / SMILE_PI + indirect) + emissive;
}

#endif
