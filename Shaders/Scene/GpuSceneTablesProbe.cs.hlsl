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
StructuredBuffer<GpuInstanceMeta>   InstanceMeta      : register(t3);
StructuredBuffer<GpuInstanceBounds> InstanceBounds    : register(t4);
StructuredBuffer<GpuInstanceTransform> InstanceCurrent  : register(t5);
StructuredBuffer<GpuInstanceTransform> InstancePrevious : register(t6);

// Trinta uint4 por instancia: todos os campos das sete structs mais os derivados que provam
// que as CONSTANTES tambem casam (deslocamentos de textura e bits de flag).
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
    const GpuInstanceMeta   im = InstanceMeta[i];
    const GpuInstanceBounds ib = InstanceBounds[i];
    const GpuInstanceTransform ic = InstanceCurrent[i];
    const GpuInstanceTransform ip = InstancePrevious[i];

    const uint b = i * 30u;
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
    Results[b + 9u] = uint4(im.MeshId, im.MaterialId, im.CellId, im.Flags);
    Results[b + 10u] = uint4(im.Generation,
                             (im.Flags & SMILE_INSTANCE_FLAG_VISIBLE) != 0u ? 1u : 0u,
                             (im.Flags & SMILE_INSTANCE_FLAG_RT_ONLY) != 0u ? 1u : 0u,
                             (im.Flags & SMILE_INSTANCE_FLAG_DYNAMIC) != 0u ? 1u : 0u);
    Results[b + 11u] = asuint(ib.Center);
    Results[b + 12u] = asuint(ib.Extent);
    Results[b + 13u] = asuint(ic.Value[0]);
    Results[b + 14u] = asuint(ic.Value[1]);
    Results[b + 15u] = asuint(ic.Value[2]);
    Results[b + 16u] = asuint(ic.Value[3]);
    Results[b + 17u] = asuint(ip.Value[0]);
    Results[b + 18u] = asuint(ip.Value[1]);
    Results[b + 19u] = asuint(ip.Value[2]);
    Results[b + 20u] = asuint(ip.Value[3]);

    // Bloco de RASTER da linha de material (Fase 3). Sao os campos que o pixel shader passa a ler
    // daqui em vez do CBV por draw: um offset errado neste bloco nao apareceria como material
    // trocado, e sim como parallax ou intensidade de normal de OUTRO material.
    Results[b + 21u] = asuint(m.RasterEmissive);
    Results[b + 22u] = uint4(asuint(m.NormalStrength), asuint(m.HeightScale),
                             asuint(m.ParallaxMinSteps), asuint(m.ParallaxMaxSteps));
    Results[b + 23u] = uint4(asuint(m.ParallaxFadeStart), asuint(m.ParallaxFadeRange),
                             m.ParallaxRefineSteps, m.ShadingModel);
    // E as seis constantes de bit novas, pela mesma razao das duas antigas: renumerar uma delas
    // mantem o layout certo e troca o significado do bit.
    Results[b + 24u] = uint4((m.Flags & SMILE_MAT_FLAG_NORMALMAP)    != 0u ? 1u : 0u,
                             (m.Flags & SMILE_MAT_FLAG_AOMAP)        != 0u ? 1u : 0u,
                             (m.Flags & SMILE_MAT_FLAG_HEIGHTMAP)    != 0u ? 1u : 0u,
                             (m.Flags & SMILE_MAT_FLAG_NORMALFLIPY)  != 0u ? 1u : 0u);
    Results[b + 25u] = uint4((m.Flags & SMILE_MAT_FLAG_NORMALRECONZ)   != 0u ? 1u : 0u,
                             (m.Flags & SMILE_MAT_FLAG_PARALLAXREFINE) != 0u ? 1u : 0u,
                             MaterialTextureIndex(m, SMILE_MAT_TEX_NORMAL),
                             MaterialTextureIndex(m, SMILE_MAT_TEX_HEIGHT));

    // O bloco de ENDERECO NO POOL da linha de malha (Fase 5). Chegou a vez do `Flags`, que era
    // reservado: ele agora carrega o bit de offset inalcancavel, e os tres campos seguintes sao o
    // que o argumento indireto consome. `VertexOffset` viaja como asuint porque e INT — e ler um
    // negativo como uint aqui e exatamente o que o teste tem de conseguir ver.
    Results[b + 26u] = uint4(h.Flags, asuint(h.VertexOffset), h.IndexOffset, h.GeometryPoolId);
    Results[b + 27u] = uint4(
        (h.Flags & SMILE_MESH_FLAG_OFFSETS_UNREPRESENTABLE) != 0u ? 1u : 0u,
        h.VirtualGeometryId, 0u, 0u);

    // A CLASSE DE PIPELINE (Fase 5) e as duas portas do caminho indireto.
    //
    // A linha 28 e o valor lido da tabela mais as duas respostas CALCULADAS pelo espelho HLSL: sem
    // elas, um mirror que esquecesse o termo da flag da malha passaria verde, porque o campo em si
    // continuaria no offset certo. A linha 29 sao as quatro constantes cruas, pela mesma razao dos
    // bits de flag: renumerar uma delas mantem todo o layout correto e troca o lote em que o
    // material cai.
    Results[b + 28u] = uint4(m.PipelineClass,
                             GpuPipelineClassIndirectEligible(m.PipelineClass) ? 1u : 0u,
                             GpuIndirectEligible(m.PipelineClass, h.Flags) ? 1u : 0u,
                             SMILE_PIPELINE_CLASS_COUNT);
    Results[b + 29u] = uint4(SMILE_PIPELINE_CLASS_OPAQUE_ONESIDED,
                             SMILE_PIPELINE_CLASS_OPAQUE_TWOSIDED,
                             SMILE_PIPELINE_CLASS_MASKED,
                             SMILE_PIPELINE_CLASS_BLEND);
}
