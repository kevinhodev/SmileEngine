#ifndef SMILE_RT_ALPHATEST_HLSLI
#define SMILE_RT_ALPHATEST_HLSLI

// RT_LoadTriangle / RT_LoadMaterial. Separados do DDGICommon porque usam ResourceDescriptorHeap —
// ver a nota la.
#include "RTTriangleAccess.hlsli"
#include "../Scene/GpuSceneTablesAccess.hlsli"

// Alpha-test de candidatos do RayQuery (extraido do HitShading.hlsli p/ passes que so precisam
// de visibilidade, ex. ReSTIRGISpatial). Contrato de bindings (declarados pelo shader que
// inclui): Instances (StructuredBuffer<InstanceGeo>), LinearWrap e root signature
// heap-directly-indexed (ResourceDescriptorHeap). O payload por triangulo vem bindless via
// InstanceGeo.TriangleSrv.
//
// Geometria alpha-tested entra na BLAS com Opacity Micro-Map (OMM 4-state), e nenhuma instancia
// tem flag de opacidade na TLAS (RaytracingScene.cpp, CollectInstances). Quem decide e o
// micro-map: microtriangulo opaco ou transparente o hardware resolve sozinho, e so o UNKNOWN chega
// aqui como candidato nao-opaco, que amostra albedo.a vs cutoff. Sem isto, cards de folhagem
// seriam quads solidos (partes transparentes pretas + auto-sombra chapada). (Este comentario
// dizia que as instancias eram marcadas FORCE_NON_OPAQUE; nao sao desde o OMM.)
//
// ⚠️ ESTE E O CAMINHO MAIS QUENTE dos que migraram, e por um motivo que nao e obvio: ele roda por
// CANDIDATO de travessia (ver SMILE_RT_PROCEED), nao por hit comitado. Em folhagem um raio gera
// varios candidatos nao-opacos antes de comitar, entao a contagem de chamadas aqui supera a do
// PT_LoadHitSurface. Antes ele pagava a cadeia IB -> 3 vertices INTEIRA para extrair 3 UVs — 3
// leituras dependentes com stride de 32 B para usar 24 B. Agora sao os 12 B de UV do registro.
bool AlphaTestPass(uint instId, uint tri, float2 bary) {
    InstanceGeo geo = Instances[instId];
    // A linha do material vem antes do payload do triangulo de proposito: a saida antecipada
    // (material sem alpha-test) depende so dela, e nesse caso o registro de 32 B nem e lido.
    GpuMaterialRecord mat = RT_LoadMaterial(geo);
    const uint kNeeded = SMILE_MAT_FLAG_ALPHATEST | SMILE_MAT_FLAG_ALBEDOMAP;
    if ((mat.Flags & kNeeded) != kNeeded)
        return true; // sem alpha-test, ou sem albedo p/ testar -> trata como opaco
    float2 uv = RT_InterpolatedUV(RT_LoadTriangle(geo, tri), bary);
    Texture2D<float4> albedoTex =
        ResourceDescriptorHeap[MaterialTextureIndex(mat, SMILE_MAT_TEX_ALBEDO)];
    // O fator de opacidade do material entra no recorte, igual ao raster (GBuffer.ps.hlsl faz
    // clip(ClipAlpha * BaseColorFactor.a - AlphaCutoff)). Sem ele, um cutout com BaseColorFactor.a
    // < 1 (fade de LOD, dissolve) sumia da tela e continuava SOLIDO para os raios — sombra, oclusao
    // de GI e reflexo de uma geometria que o raster ja tinha descartado.
    return albedoTex.SampleLevel(LinearWrap, uv, 0.0f).a * mat.BaseColor.a >= mat.AlphaCutoff;
}

// A POLITICA DE OMM de um passe, como chega do CB (float). Devolve 0 (4-state: o UNKNOWN passa
// pelo alpha-test acima) ou RAY_FLAG_FORCE_OMM_2_STATE (o hardware resolve o UNKNOWN pelo bit
// O/T e nenhum candidato chega ao SMILE_RT_PROCEED). MASCARADO: so este bit atravessa o CB, entao
// um valor qualquer ali nunca injeta outra ray flag.
//
// Vai no argumento de RUNTIME do TraceRayInline, nao no template do RayQuery: as flags efetivas
// sao o OR das duas, e o template so precisa de RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS (proposta
// HLSL 0024) — que todos ja tem. Assim o tipo do RayQuery nao muda e a politica e um knob.
//
// Quem pode ligar: so os passes de GI (DDGI, ReSTIR GI, radiance cache), e so com a cena toda
// coberta por micro-map — o 2-state transforma FULLY_UNKNOWN_OPAQUE em opaco solido. Reflexos e
// ReSTIR DI ficam em 4-state. Ver Docs/OMM-AUDIT.md, P0.
uint SMILE_OmmRayFlags(float packed) {
    return ((uint)packed) & RAY_FLAG_FORCE_OMM_2_STATE;
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
