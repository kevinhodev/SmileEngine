#ifndef SMILE_RT_ALPHATEST_HLSLI
#define SMILE_RT_ALPHATEST_HLSLI

// Alpha-test de candidatos do RayQuery (extraido do HitShading.hlsli p/ passes que so precisam
// de visibilidade, ex. ReSTIRGISpatial). Contrato de bindings (declarados pelo shader que
// inclui): Instances (StructuredBuffer<InstanceGeo>), LinearWrap e root signature
// heap-directly-indexed (ResourceDescriptorHeap). O payload por triangulo vem bindless via
// InstanceGeo.TriangleSrv.
//
// Instancias com AlphaTest sao marcadas FORCE_NON_OPAQUE na TLAS (RaytracingScene.cpp) e cada
// candidato nao-opaco amostra albedo.a vs cutoff. Sem isto, cards de folhagem seriam quads
// solidos (partes transparentes pretas + auto-sombra chapada).
//
// ⚠️ ESTE E O CAMINHO MAIS QUENTE dos que migraram, e por um motivo que nao e obvio: ele roda por
// CANDIDATO de travessia (ver SMILE_RT_PROCEED), nao por hit comitado. Em folhagem um raio gera
// varios candidatos nao-opacos antes de comitar, entao a contagem de chamadas aqui supera a do
// PT_LoadHitSurface. Antes ele pagava a cadeia IB -> 3 vertices INTEIRA para extrair 3 UVs — 3
// leituras dependentes com stride de 32 B para usar 24 B. Agora sao os 12 B de UV do registro.
bool AlphaTestPass(uint instId, uint tri, float2 bary) {
    InstanceGeo geo = Instances[instId];
    if ((geo.Flags & INSTGEO_FLAG_ALPHATEST) == 0u || geo.HasAlbedo == 0u)
        return true; // sem alpha-test -> trata como opaco
    float2 uv = RT_InterpolatedUV(RT_LoadTriangle(geo, tri), bary);
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
