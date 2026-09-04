// Sonda de ABI das tabelas canonicas da cena.
//
// Existe pela mesma razao do TemporalResolveProbe: static_assert prova o layout do lado C++, e nao
// prova NADA sobre o lado HLSL. Trocar dois campos de lugar no espelho, ou renumerar um
// SMILE_MAT_FLAG_*, compila dos dois lados e so aparece como material trocado em todo hit de RT.
//
// Aqui a struct de PRODUCAO e lida pelo MESMO DXC dos shaders do jogo, sobre bytes montados pelo
// C++, e cada campo volta para o teste conferir. Roda em WARP, entao nao exige placa.
//
// O acesso e por SRV direto e nao por ResourceDescriptorHeap de proposito: o que esta sob teste e
// o LAYOUT (stride e offsets), e amarrar a sonda a SM 6.6 + root signature heap-directly-indexed
// so acrescentaria motivo de falhar que nao e o do teste.
//
// Este shader nao entra em nenhum passe.

#include "GpuSceneTables.hlsli"

cbuffer ProbeCB : register(b0) {
    uint4 Params; // x = quantidade de instancias a ler
};

StructuredBuffer<GpuMaterialRecord> Materials : register(t0);
StructuredBuffer<InstanceGeo>       Instances : register(t1);
StructuredBuffer<GpuMeshRecord>     Meshes    : register(t2);

// Nove uint4 por instancia: todos os campos das tres structs mais os tres derivados que provam
// que as CONSTANTES tambem casam (deslocamento de textura e dois bits de flag).
RWStructuredBuffer<uint4> Results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.x) return;

    // Indexar Instances[i] prova o STRIDE do snapshot; usar o MaterialId e o MeshId que vieram
    // dele para indexar as tabelas prova o stride de cada uma e a ligacao entre as tres.
    const InstanceGeo       g = Instances[i];
    const GpuMaterialRecord m = Materials[g.MaterialId];
    const GpuMeshRecord     h = Meshes[g.MeshId];

    const uint b = i * 9u;
    Results[b + 0u] = asuint(m.BaseColor);
    Results[b + 1u] = asuint(m.EmissiveFactor);
    Results[b + 2u] = asuint(m.SubsurfaceColor);
    Results[b + 3u] = uint4(asuint(m.RoughnessFactor), asuint(m.AlphaCutoff),
                            m.Flags, m.TextureBase);
    Results[b + 4u] = uint4(g.TriangleSrv, g.MeshSrv, g.MeshId, g.MaterialSrv);
    Results[b + 5u] = uint4(g.MaterialId,
                            MaterialTextureIndex(m, SMILE_MAT_TEX_EMISSIVE),
                            (m.Flags & SMILE_MAT_FLAG_TWOSIDED)  != 0u ? 1u : 0u,
                            (m.Flags & SMILE_MAT_FLAG_ALBEDOMAP) != 0u ? 1u : 0u);
    Results[b + 6u] = uint4(asuint(h.LocalAABBMin), asuint(h.LocalAABBMax.x));
    Results[b + 7u] = uint4(asuint(h.LocalAABBMax.yz), h.VertexSrv, h.IndexSrv);
    Results[b + 8u] = uint4(h.TriangleSrv, h.VertexCount, h.IndexCount, h.TriangleCount);
    // Flags e VirtualGeometryId sao reservados e valem 0; conferi-los no C++ exigiria dois uint4
    // so para dois zeros. O static_assert do offset ja fixa a posicao deles, e o dia em que
    // ganharem valor este teste ganha a leitura junto.
}
