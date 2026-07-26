#ifndef SMILE_RT_ALPHATEST_HLSLI
#define SMILE_RT_ALPHATEST_HLSLI

// Alpha-test de candidatos do RayQuery (extraido do HitShading.hlsli p/ passes que so precisam
// de visibilidade, ex. ReSTIRGISpatial). Contrato de bindings (declarados pelo shader que
// inclui): Instances (StructuredBuffer<InstanceGeo>), LinearWrap e root signature
// heap-directly-indexed (ResourceDescriptorHeap). VB/IB vem bindless via InstanceGeo.
//
// Instancias com AlphaTest sao marcadas FORCE_NON_OPAQUE na TLAS (RaytracingScene.cpp) e cada
// candidato nao-opaco amostra albedo.a vs cutoff. Sem isto, cards de folhagem seriam quads
// solidos (partes transparentes pretas + auto-sombra chapada).
bool AlphaTestPass(uint instId, uint tri, float2 bary) {
    InstanceGeo geo = Instances[instId];
    if ((geo.Flags & INSTGEO_FLAG_ALPHATEST) == 0u || geo.HasAlbedo == 0u)
        return true; // sem alpha-test -> trata como opaco
    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    uint i0 = Idx[tri * 3 + 0];
    uint i1 = Idx[tri * 3 + 1];
    uint i2 = Idx[tri * 3 + 2];
    float2 uv = Verts[i0].TexCoord * (1.0f - bary.x - bary.y)
              + Verts[i1].TexCoord * bary.x
              + Verts[i2].TexCoord * bary.y;
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
    // O fator de opacidade do material entra no recorte, igual ao raster (GBuffer.ps.hlsl faz
    // clip(ClipAlpha * BaseColorFactor.a - AlphaCutoff)). Sem ele, um cutout com BaseColorFactor.a
    // < 1 (fade de LOD, dissolve) sumia da tela e continuava SOLIDO para os raios — sombra, oclusao
    // de GI e reflexo de uma geometria que o raster ja tinha descartado.
    return albedoTex.SampleLevel(LinearWrap, uv, 0.0f).a * geo.BaseColor.a >= geo.AlphaCutoff;
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
