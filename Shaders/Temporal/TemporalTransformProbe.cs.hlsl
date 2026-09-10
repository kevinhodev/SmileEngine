// Sonda de paridade do par temporal derivado (Fase 4 do GPU-DRIVEN-PLAN).
//
// O que ela existe para impedir: a aposentadoria do `TransformBuffer` do TemporalMotion tira da CPU
// uma tabela de 128 B por instancia -- reescrita inteira por frame, com DUAS inversas 4x4 por
// entrada -- e passa a derivar o mesmo par na leitura, a partir das colunas canonicas que ja
// existem. O risco nao e de layout: e a INVERSA e a ORDEM DO PRODUTO.
//
// Uma inversa com um sinal de cofator trocado nao da erro em lugar nenhum. Ela reprojeta para o
// lugar errado, e so em movimento -- exatamente onde o suspeito natural seria o denoiser, que nao
// tem nada a ver com isso.
//
// Os dois lados sao os de PRODUCAO: `Smile::ComposeTemporalInstanceTransform`, que o UpdatePerFrame
// chama, e o `TemporalComposeInstanceTransform` deste include, que os consumidores vao chamar.
// Nenhum dos dois e reescrito aqui.
//
// Le as colunas por SRV direto, e nao por ResourceDescriptorHeap: o que esta sob teste e a
// MATEMATICA, e amarrar a sonda a SM 6.6 so acrescentaria motivo de falhar que nao e o do teste.
//
// Este shader nao entra em nenhum passe.

#include "TemporalMotionCommon.hlsli"
#include "../Scene/GpuSceneTables.hlsli"

cbuffer ProbeCB : register(b0) {
    uint4 Params; // x = quantas instancias ler
};

StructuredBuffer<GpuInstanceTransform> InstanceCurrent  : register(t0);
StructuredBuffer<GpuInstanceTransform> InstancePrevious : register(t1);

// Dezesseis float4 por instancia: o PAR (duas matrizes) seguido dos dois sentidos calculados
// SEPARADAMENTE. As duas metades tem de coincidir -- e o que impede os helpers de um sentido so, que
// e o que os consumidores vao chamar, de divergirem do par que a paridade com o C++ cobre.
RWStructuredBuffer<float4> Results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.x) return;

    const float4x4 current  = InstanceCurrent[i].Value;
    const float4x4 previous = InstancePrevious[i].Value;

    const FTemporalInstanceTransform T = TemporalComposeInstanceTransform(current, previous);
    const float4x4 SoIda   = TemporalCurrentToPrevious(current, previous);
    const float4x4 SoVolta = TemporalPreviousToCurrent(current, previous);

    const uint b = i * 16u;
    for (uint r = 0; r < 4u; ++r) {
        Results[b +  0u + r] = T.CurrentToPrevious[r];
        Results[b +  4u + r] = T.PreviousToCurrent[r];
        Results[b +  8u + r] = SoIda[r];
        Results[b + 12u + r] = SoVolta[r];
    }
}
