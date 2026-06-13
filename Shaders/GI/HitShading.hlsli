#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

// Shading do hit de um raio de cena, COMPARTILHADO pelo DDGI (difuso) e pelo Specular GI
// (reflexoes RT). Fatorado de DDGITrace.cs.hlsl (Fase 1a/1b) p/ que as duas pipelines usem
// EXATAMENTE o mesmo modelo: no hit, sol direto (shadow ray) + multibounce do atlas DDGI do
// frame anterior; no miss, ceu da Sky-View LUT. Espelha o "TraceSurfaceCacheRay" do Lumen,
// so que sombreado pelo DDGI em vez do Surface Cache.
//
// O includer DEVE declarar estes globais (mesmos nomes/registros usados aqui):
//   RaytracingAccelerationStructure Scene   // TLAS da cena (shadow ray)
//   Texture2D<float4>               SkyViewLUT
//   StructuredBuffer<InstanceGeo>   Instances
//   Texture2D<float4>               IrradAtlas   // atlas octaedrico de irradiancia (frame anterior)
//   StructuredBuffer<DDGIVertex>    Vertices     // global mesclado
//   Buffer<uint>                    Indices      // global mesclado (R32_UINT)
//   SamplerState                    LinearClamp, LinearWrap
// E compilar como cs_6_6 com HEAP_DIRECTLY_INDEXED (albedo bindless via ResourceDescriptorHeap).

#include "DDGICommon.hlsli" // SampleDDGIIrradiance, SMILE_PI, InstanceGeo, DDGIVertex

// Parametros escalares do shading (vem do CB de cada pipeline).
struct FHitShadeParams {
    float3 GridMin;       float Spacing;
    int3   Count;         int   AtlasTile;
    float2 AtlasInvSize;
    float3 SunDir;        float SunIntensity;
    float3 SunColor;      float NormalBias;     // offset do shadow ray (world units)
    float  SkyIntensity;  float MaxRayDist;     // TMax do shadow/sky ray
    float  AlbedoLOD;     // LOD da textura albedo no hit (difuso usa 4=media; reflexao usa menor)
    bool   RealHitShading;// normal geometrica real no hit (vs aproximacao -rayDir)
};

// --- Sky-View LUT por direcao (replica AtmosphereCommon, consts fixas p/ evitar clash de b0) ---
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

// Radiancia do ceu na direcao 'dir' (miss do raio). Escala por skyIntensity.
float3 ShadeSky(float3 dir, float3 sunDir, float skyIntensity) {
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float viewZenithCos = dot(dir, up);
    float3 viewHoriz = dir    - up * viewZenithCos;
    float3 sunHoriz  = sunDir - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));
    float2 uv = DDGI_SkyViewUv(viewZenithCos, lightViewCos);
    return SkyViewLUT.SampleLevel(LinearClamp, uv, 0.0f).rgb * skyIntensity;
}

// Sombreia a superficie atingida por um RayQuery commitado. Retorna a radiancia que sai do hit
// na direcao do observador; 'outSignedDist' = |dist| ao hit, NEGATIVO se backface (probe/origem
// atras da superficie) — usado pela relocacao do DDGI. Folhagem/two-sided nao emite backface.
float3 ShadeSurfaceHit(uint instId, uint tri, float2 bary, float3x4 worldToObject,
                       float3 rayOrigin, float3 rayDir, float hitDist,
                       FHitShadeParams P, out float outSignedDist) {
    float3 hitPos = rayOrigin + rayDir * hitDist;

    InstanceGeo geo    = Instances[instId];
    float3      albedo = geo.BaseColor.rgb;

    // Normal geometrica real do triangulo (interpola as 3 normais por barycentricas + leva p/
    // mundo via WorldToObject == inverse-transpose). Sempre computada: serve p/ o backface-test.
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

    // Normal de shading: real (encarando o raio) com o toggle; senao a aproximacao -rayDir.
    float3 hitN = normalize(-rayDir);
    if (P.RealHitShading) {
        hitN = backface ? -geomN : geomN;
        if (geo.HasAlbedo != 0) {
            float2 uv = Vertices[i0].TexCoord * (1.0f - bary.x - bary.y)
                      + Vertices[i1].TexCoord * bary.x
                      + Vertices[i2].TexCoord * bary.y;
            Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
            albedo *= albedoTex.SampleLevel(LinearWrap, uv, P.AlbedoLOD).rgb;
        }
    }

    // Sol direto, com shadow ray (visibilidade).
    float ndl = saturate(dot(hitN, P.SunDir));
    float vis = 1.0f;
    if (ndl > 0.0f) {
        RayDesc sray;
        sray.Origin    = hitPos + hitN * max(P.NormalBias, 1e-3f);
        sray.Direction = P.SunDir;
        sray.TMin      = 0.01f;
        sray.TMax      = P.MaxRayDist;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray);
        sq.Proceed();
        vis = (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
    }
    float3 Edirect = P.SunColor * P.SunIntensity * vis * ndl;

    // Multibounce: irradiancia do atlas DDGI do frame ANTERIOR no ponto de hit.
    float3 indirect = SampleDDGIIrradiance(IrradAtlas, LinearClamp, hitPos, hitN,
                                           P.GridMin, P.Spacing, P.Count,
                                           P.AtlasTile, P.AtlasInvSize);

    return albedo * (Edirect / SMILE_PI + indirect);
}

#endif // SMILE_GI_HITSHADING_HLSLI
