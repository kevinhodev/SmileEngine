#ifndef SMILE_RT_ALPHATEST_HLSLI
#define SMILE_RT_ALPHATEST_HLSLI

// Alpha-test de candidatos do RayQuery (extraido do HitShading.hlsli p/ passes que so precisam
// de visibilidade, ex. ReSTIRGISpatial). Contrato de bindings (declarados pelo shader que
// inclui): Instances (StructuredBuffer<InstanceGeo>), Vertices (StructuredBuffer<DDGIVertex>),
// Indices (Buffer<uint>), LinearWrap e root signature heap-directly-indexed
// (ResourceDescriptorHeap).
//
// Instancias com AlphaTest sao marcadas FORCE_NON_OPAQUE na TLAS (RaytracingScene.cpp) e cada
// candidato nao-opaco amostra albedo.a vs cutoff. Sem isto, cards de folhagem seriam quads
// solidos (partes transparentes pretas + auto-sombra chapada).
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

#endif
