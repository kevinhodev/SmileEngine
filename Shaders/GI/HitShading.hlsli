#ifndef SMILE_GI_HITSHADING_HLSLI
#define SMILE_GI_HITSHADING_HLSLI

#include "DDGICommon.hlsli" 

struct FHitShadeParams {
    float3 GridMin;       float Spacing;
    int3   Count;         int   AtlasTile;
    float2 AtlasInvSize;
    float3 SunDir;        float SunIntensity;
    float3 SunColor;      float NormalBias;     
    float  SkyIntensity;  float MaxRayDist;     
    float  AlbedoLOD;     
    bool   RealHitShading;
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

    float3 indirect = SampleDDGIIrradiance(IrradAtlas, LinearClamp, hitPos, hitN,
                                           P.GridMin, P.Spacing, P.Count,
                                           P.AtlasTile, P.AtlasInvSize);

    return albedo * (Edirect / SMILE_PI + indirect);
}

#endif I
