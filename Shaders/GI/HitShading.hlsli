#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

#include "DDGICommon.hlsli"
#include "../LightsCommon.hlsli"

// Contrato de bindings (declarados pelo shader que inclui): Scene, Instances, Vertices,
// Indices, SkyViewLUT, IrradAtlas, LinearClamp/Wrap e — F5 — SceneLights
// (StructuredBuffer<FPunctualLight> com TODAS as luzes ativas, sem frustum cull).
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

// Alpha-test de candidatos do RayQuery: instancias com AlphaTest sao marcadas FORCE_NON_OPAQUE na
// TLAS (RaytracingScene.cpp) e cada candidato nao-opaco amostra albedo.a vs cutoff. Sem isto,
// cards de folhagem seriam quads solidos (partes transparentes pretas + auto-sombra chapada).
// Requer heap-directly-indexed (ResourceDescriptorHeap) — todos os shaders de trace da cena tem.
bool AlphaTestPass(uint instId, uint tri, float2 bary) {
    InstanceGeo geo = Instances[instId];
    if ((geo.Flags & INSTGEO_FLAG_ALPHATEST) == 0u || geo.HasAlbedo == 0u)
        return true; // sem alpha-test -> trata como opaco
    uint i0 = Indices[geo.IndexBase + tri * 3 + 0] + geo.VertexBase;
    uint i1 = Indices[geo.IndexBase + tri * 3 + 1] + geo.VertexBase;
    uint i2 = Indices[geo.IndexBase + tri * 3 + 2] + geo.VertexBase;
    float2 uv = Vertices[i0].TexCoord * (1.0f - bary.x - bary.y)
              + Vertices[i1].TexCoord * bary.x
              + Vertices[i2].TexCoord * bary.y;
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
    return albedoTex.SampleLevel(LinearWrap, uv, 0.0f).a >= geo.AlphaCutoff;
}

// Drena a traversal honrando o alpha-test: opacos auto-comitam; candidatos nao-opacos so comitam
// se passarem no teste. Com ACCEPT_FIRST_HIT_AND_END_SEARCH o commit encerra a busca (shadow ray).
#define SMILE_RT_PROCEED(q)                                                                 \
    while (q.Proceed()) {                                                                   \
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&                           \
            AlphaTestPass(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),             \
                          q.CandidateTriangleBarycentrics()))                               \
            q.CommitNonOpaqueTriangleHit();                                                 \
    }

// Normal geometrica (orientada contra o raio) no ponto de hit — usada pelo ReSTIR GI p/ o Jacobiano
// de reconexao (n2). Aditivo; nao altera ShadeSurfaceHit (caminho das reflexoes intacto).
float3 HitGeomNormal(uint instId, uint tri, float2 bary, float3x4 worldToObject, float3 rayDir) {
    InstanceGeo geo = Instances[instId];
    uint i0 = Indices[geo.IndexBase + tri * 3 + 0] + geo.VertexBase;
    uint i1 = Indices[geo.IndexBase + tri * 3 + 1] + geo.VertexBase;
    uint i2 = Indices[geo.IndexBase + tri * 3 + 2] + geo.VertexBase;
    float3 nObj = Vertices[i0].Normal * (1.0f - bary.x - bary.y)
                + Vertices[i1].Normal * bary.x
                + Vertices[i2].Normal * bary.y;
    float3 nWrld = mul(nObj, (float3x3)worldToObject);
    float  nLen  = length(nWrld);
    float3 geomN = (nLen > 1e-5f) ? (nWrld / nLen) : normalize(-rayDir);
    bool   backface = (geo.TwoSided == 0) && (dot(geomN, rayDir) > 0.0f);
    return backface ? -geomN : geomN;
}

float3 ShadeSurfaceHit(uint instId, uint tri, float2 bary, float3x4 worldToObject,
                       float3 rayOrigin, float3 rayDir, float hitDist,
                       FHitShadeParams P, out float outSignedDist) {
    float3 hitPos = rayOrigin + rayDir * hitDist;

    InstanceGeo geo    = Instances[instId];
    float3      albedo = geo.BaseColor.rgb;

    uint   i0   = Indices[geo.IndexBase + tri * 3 + 0] + geo.VertexBase;
    uint   i1   = Indices[geo.IndexBase + tri * 3 + 1] + geo.VertexBase;
    uint   i2   = Indices[geo.IndexBase + tri * 3 + 2] + geo.VertexBase;
    float3 nObj = Vertices[i0].Normal * (1.0f - bary.x - bary.y)
                + Vertices[i1].Normal * bary.x
                + Vertices[i2].Normal * bary.y;
    float3 nWrld = mul(nObj, (float3x3)worldToObject);
    float  nLen  = length(nWrld);
    float3 geomN = (nLen > 1e-5f) ? (nWrld / nLen) : normalize(-rayDir);
    bool   backface = (geo.TwoSided == 0) && (dot(geomN, rayDir) > 0.0f);
    outSignedDist   = backface ? -hitDist : hitDist;

    float2 uv = Vertices[i0].TexCoord * (1.0f - bary.x - bary.y)
              + Vertices[i1].TexCoord * bary.x
              + Vertices[i2].TexCoord * bary.y;

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
        sray.Origin    = hitPos + hitN * max(P.ShadowRayBias, 1e-3f);
        sray.Direction = P.SunDir; // direcional: a direcao nao depende da origem deslocada
        sray.TMin      = 0.01f;
        sray.TMax      = P.MaxRayDist;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray);
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
        float3 lorg = hitPos + hitN * max(P.ShadowRayBias, 1e-3f);
        float3 toL  = (hitPos + Ll * distL) - lorg;
        float  lenL = max(length(toL), 1e-4f);
        RayDesc lray;
        lray.Origin    = lorg;
        lray.Direction = toL / lenL;
        lray.TMin      = 0.01f;
        lray.TMax      = max(lenL - 0.05f, 0.02f);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
        lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, lray);
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
