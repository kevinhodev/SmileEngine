#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

#include "DDGICommon.hlsli"
#include "../LightsCommon.hlsli"
#include "../RayEpsilons.hlsli"

// Contrato de bindings (declarados pelo shader que inclui): Scene, Instances, SkyViewLUT,
// IrradAtlas, LinearClamp/Wrap e — F5 — SceneLights (StructuredBuffer<FPunctualLight> com
// TODAS as luzes ativas, sem frustum cull). VB/IB vem bindless via InstanceGeo
// (ResourceDescriptorHeap), nao ha mais Vertices/Indices globais.
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
    uint   ShadowRayMask; // instance mask dos shadow rays: ALL = folhagem sombreia (alpha-test
                          // por candidato); OPAQUE = pula folhagem (rapido, traversal pura)
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
    bool   backface = (geo.TwoSided == 0) && (dot(geomN, rayDir) > 0.0f);
    outSignedDist   = backface ? -hitDist : hitDist;

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
    // MUDANCA DE COMPORTAMENTO em material TWO-SIDED: o `backface` acima e gateado por
    // TwoSided == 0, entao folha/cortina atingida por tras tinha hitN apontando p/ longe do raio e
    // a origem do shadow ray era empurrada p/ DENTRO da superficie. Aqui a orientacao nao tem
    // gate. Em geometria one-sided o resultado e identico ao de antes.
    float3 faceN;
    float3 offsetN = HitFaceNormal(Verts, i0, i1, i2, worldToObject, faceN)
                   ? ((dot(faceN, rayDir) > 0.0f) ? -faceN : faceN)
                   : ((dot(geomN, rayDir) > 0.0f) ? -geomN : geomN); // degenerado: interpolada

    float2 uv = Verts[i0].TexCoord * (1.0f - bary.x - bary.y)
              + Verts[i1].TexCoord * bary.x
              + Verts[i2].TexCoord * bary.y;

    float3 hitN = normalize(-rayDir);
    if (P.RealHitShading) {
        hitN = backface ? -geomN : geomN;
        if (geo.HasAlbedo != 0) {
            Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
            albedo *= albedoTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
        }
    }

    float ndl = saturate(dot(hitN, P.SunDir));
    float vis = 1.0f;
    if (ndl > 0.0f) {
        RayDesc sray;
        sray.Origin    = hitPos + offsetN * max(P.ShadowRayBias, kShadowRayBiasMin);
        sray.Direction = P.SunDir; // direcional: a direcao nao depende da origem deslocada
        sray.TMin      = kShadowRayTMin;
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
        float3 lorg = hitPos + offsetN * max(P.ShadowRayBias, kShadowRayBiasMin);
        float3 toL  = (hitPos + Ll * distL) - lorg;
        float  lenL = max(length(toL), 1e-4f);
        RayDesc lray;
        lray.Origin    = lorg;
        lray.Direction = toL / lenL;
        lray.TMin      = kShadowRayTMin;
        lray.TMax      = max(lenL - kLightRayEndMargin, kLightRayMinTMax);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
        lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, lray);
        SMILE_RT_PROCEED(lq)
        if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) Edirect += contrib;
    }

    float3 indirect = SampleDDGIIrradiance(IrradAtlas, LinearClamp, hitPos, hitN,
                                           P.GridMin, P.Spacing, P.Count,
                                           P.AtlasTile, P.AtlasInvSize);

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
