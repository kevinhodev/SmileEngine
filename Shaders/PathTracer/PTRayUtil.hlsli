#ifndef SMILE_PT_RAYUTIL_HLSLI
#define SMILE_PT_RAYUTIL_HLSLI

// ReSTIR PT — busca de superficie no hit + raios utilitarios.
// Requer (via includes do .cs.hlsl host): DDGICommon.hlsli (InstanceGeo/DDGIVertex),
// HitShading.hlsli (SMILE_RT_PROCEED/AlphaTestPass), globals Scene/Instances/Vertices/Indices/
// LinearWrap e o cbuffer de PTCommon.hlsli.

// Superficie completa num vertice do caminho — tudo que o BSDF e o NEE precisam.
struct FPTSurface {
    float3 Pos;
    float3 N;         // normal geometrica orientada contra o raio de chegada
    float3 Albedo;
    float3 Emissive;
    float  Roughness;
    float  Metallic;
    bool   Foliage;   // two-sided + transmissao no NEE
};

FPTSurface PTFetchHitSurface(uint instId, uint tri, float2 bary, float3x4 worldToObject,
                             float3 rayOrigin, float3 rayDir, float hitDist, float albedoLOD) {
    InstanceGeo geo = Instances[instId];

    uint i0 = Indices[geo.IndexBase + tri * 3 + 0] + geo.VertexBase;
    uint i1 = Indices[geo.IndexBase + tri * 3 + 1] + geo.VertexBase;
    uint i2 = Indices[geo.IndexBase + tri * 3 + 2] + geo.VertexBase;
    float3 w    = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = Vertices[i0].Normal * w.x + Vertices[i1].Normal * w.y + Vertices[i2].Normal * w.z;
    float2 uv   = Vertices[i0].TexCoord * w.x + Vertices[i1].TexCoord * w.y + Vertices[i2].TexCoord * w.z;

    float3 nWrld = mul(nObj, (float3x3)worldToObject);
    float  nLen  = length(nWrld);
    float3 geomN = (nLen > 1e-5f) ? (nWrld / nLen) : normalize(-rayDir);
    if (dot(geomN, rayDir) > 0.0f) geomN = -geomN; // orienta contra o raio (two-sided implicito)

    FPTSurface s;
    s.Pos       = rayOrigin + rayDir * hitDist;
    s.N         = geomN;
    s.Albedo    = geo.BaseColor.rgb;
    s.Emissive  = geo.EmissiveFactor.rgb;
    s.Roughness = max(geo.RoughnessFactor, 0.04f);
    s.Metallic  = geo.EmissiveFactor.w;
    s.Foliage   = (geo.Flags & INSTGEO_FLAG_FOLIAGE) != 0u;

    if (geo.HasAlbedo != 0u) {
        Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
        s.Albedo *= albedoTex.SampleLevel(LinearWrap, uv, albedoLOD).rgb;
    }
    if ((geo.Flags & INSTGEO_FLAG_EMISSIVE) != 0u) {
        Texture2D<float4> emisTex = ResourceDescriptorHeap[geo.EmissiveMapIndex];
        s.Emissive *= emisTex.SampleLevel(LinearWrap, uv, albedoLOD).rgb;
    }
    if ((geo.Flags & INSTGEO_FLAG_MRMAP) != 0u) {
        // Convencao glTF (e SpecularPacking): G = roughness, B = metallic.
        Texture2D<float4> mrTex = ResourceDescriptorHeap[geo.MrMapIndex];
        float2 mr = mrTex.SampleLevel(LinearWrap, uv, albedoLOD).gb;
        s.Roughness = max(geo.RoughnessFactor * mr.x, 0.04f);
        s.Metallic  = saturate(geo.EmissiveFactor.w * mr.y);
    }
    return s;
}

// Raio de sombra (oclusao binaria) com alpha-test de folhagem na traversal.
float PTShadowRayVis(float3 origin, float3 dir, float tmax) {
    RayDesc sray;
    sray.Origin    = origin;
    sray.Direction = dir;
    sray.TMin      = 0.01f;
    sray.TMax      = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
    sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sray);
    SMILE_RT_PROCEED(sq)
    return (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

#endif
