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

// Seis uint4 por instancia: os treze campos das duas structs mais os tres derivados que provam
// que as CONSTANTES tambem casam (deslocamento de textura e dois bits de flag).
RWStructuredBuffer<uint4> Results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.x) return;

    // Indexar Instances[i] prova o STRIDE do snapshot; usar o MaterialId que veio dele para
    // indexar Materials prova o stride da tabela e a ligacao entre as duas.
    const InstanceGeo       g = Instances[i];
    const GpuMaterialRecord m = Materials[g.MaterialId];

    const uint b = i * 6u;
    Results[b + 0u] = asuint(m.BaseColor);
    Results[b + 1u] = asuint(m.EmissiveFactor);
    Results[b + 2u] = asuint(m.SubsurfaceColor);
    Results[b + 3u] = uint4(asuint(m.RoughnessFactor), asuint(m.AlphaCutoff),
                            m.Flags, m.TextureBase);
    Results[b + 4u] = uint4(g.VertexSrv, g.IndexSrv, g.TriangleSrv, g.MaterialSrv);
    Results[b + 5u] = uint4(g.MaterialId,
                            MaterialTextureIndex(m, SMILE_MAT_TEX_EMISSIVE),
                            (m.Flags & SMILE_MAT_FLAG_TWOSIDED)  != 0u ? 1u : 0u,
                            (m.Flags & SMILE_MAT_FLAG_ALBEDOMAP) != 0u ? 1u : 0u);
}
