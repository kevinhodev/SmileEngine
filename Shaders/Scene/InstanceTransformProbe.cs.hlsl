// Sonda de paridade da composicao de transform (Fase 4 do GPU-DRIVEN-PLAN).
//
// Existe pela mesma razao do GpuSceneTablesProbe: o `static_assert` do C++ prova o layout de um
// lado e nao diz nada sobre o outro. Aqui o que esta sob teste nao e so o layout — e a ORDEM DO
// PRODUTO, que e o erro classico desta migracao. `mul(Model, ViewProj)` e `mul(ViewProj, Model)`
// compilam, desenham, e a segunda poe a cena inteira no lugar errado.
//
// A composicao vem do MESMO include que o vertex shader de producao usa. Uma sonda que repetisse
// as quatro linhas compararia um espelho com outro espelho.
//
// Le as colunas por SRV direto, e nao por ResourceDescriptorHeap: o que se testa e a MATEMATICA, e
// amarrar a sonda a SM 6.6 mais root signature de heap indexado so acrescentaria motivo de falhar
// que nao e o do teste.
//
// Este shader nao entra em nenhum passe.

#include "GpuSceneTables.hlsli"
#include "InstanceTransform.hlsli"

cbuffer ProbeCB : register(b0) {
    uint4 Params; // x = quantas instancias ler
    GpuViewMatrices ViewMatrices;
};

StructuredBuffer<GpuInstanceTransform> InstanceCurrent  : register(t0);
StructuredBuffer<GpuInstanceTransform> InstancePrevious : register(t1);

// Dezesseis float4 por instancia: as quatro matrizes compostas, em ordem.
RWStructuredBuffer<float4> Results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.x) return;

    const GpuObjectTransforms T = ComposeObjectTransforms(
        ViewMatrices, InstanceCurrent[i].Value, InstancePrevious[i].Value);

    const uint b = i * 16u;
    for (uint r = 0; r < 4u; ++r) {
        Results[b + 0u  + r] = T.MVP[r];
        Results[b + 4u  + r] = T.ModelMatrix[r];
        Results[b + 8u  + r] = T.CurMVPNoJitter[r];
        Results[b + 12u + r] = T.PrevMVP[r];
    }
}
