#ifndef SMILE_SCENE_INSTANCETRANSFORM_HLSLI
#define SMILE_SCENE_INSTANCETRANSFORM_HLSLI

// ================================================================================================
// Espelho HLSL de Smile::FGpuViewMatrices / FGpuObjectTransforms
// (Engine/Include/Smile/Graphics/Scene/GpuInstanceTransform.h), Fase 4 do GPU-DRIVEN-PLAN.
//
// O vertex shader recebe hoje QUATRO matrizes por draw, num CBV de 256 B escrito por instancia por
// frame. Elas sao DERIVADAS de duas matrizes que ja moram na tabela canonica (`Transform` e
// `PrevTransform`) mais tres matrizes de view que valem para o passe inteiro. Este header e a
// composicao, e ela e a mesma dos dois lados: o C++ chama `ComposeObjectTransforms`, o VS chama a
// funcao abaixo, e o teste de paridade em WARP compara as duas saidas.
//
// O erro classico desta migracao e a ORDEM do produto. `A * B` do Mat44 e o produto linha-major
// padrao, identico a `mul(A, B)` sobre `row_major` — trocar a ordem compila, desenha, e poe a cena
// inteira no lugar errado de um jeito que so aparece em movimento.
//
// ATENCAO: este header e PURO. Nada de ResourceDescriptorHeap aqui, pela mesma regra escrita no
// GpuSceneTables.hlsli: a sonda de paridade le as colunas por SRV direto e compila em 6_0, e o DXC
// valida o corpo de toda funcao mesmo sem chamada. Quem carrega pelo heap e o proprio VS, sob a
// permutacao.
// ================================================================================================

struct GpuViewMatrices {
    row_major float4x4 ViewProj;               // jittered — SV_POSITION
    row_major float4x4 ViewProjUnjittered;     // motion vector do frame ATUAL
    row_major float4x4 PrevViewProjUnjittered; // motion vector do frame ANTERIOR
};

struct GpuObjectTransforms {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    row_major float4x4 CurMVPNoJitter;
    row_major float4x4 PrevMVP;
};

GpuObjectTransforms ComposeObjectTransforms(GpuViewMatrices View, float4x4 Model,
                                            float4x4 PrevModel) {
    GpuObjectTransforms Out;
    Out.MVP            = mul(Model, View.ViewProj);
    Out.ModelMatrix    = Model;
    Out.CurMVPNoJitter = mul(Model, View.ViewProjUnjittered);
    // PrevModel, e nao Model: sem historico daquele objeto os dois sao iguais e o motion vector do
    // OBJETO zera, que e o desfecho certo para quem acabou de nascer ou teve o slot reciclado.
    Out.PrevMVP        = mul(PrevModel, View.PrevViewProjUnjittered);
    return Out;
}

#endif // SMILE_SCENE_INSTANCETRANSFORM_HLSLI
