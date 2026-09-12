// Observador da sonda de comando indireto (Fase 5 do GPU-DRIVEN-PLAN).
//
// Roda DENTRO de um ExecuteIndirect e relata o que o command processor entregou. E a unica forma de
// provar o contrato: o layout do argumento nao e escolhido pela struct, e ditado pela ORDEM das
// entradas em `pArgumentDescs`, e nada do lado do shader valida isso. Com as duas entradas de
// constante trocadas, o codigo compila, o comando executa, e a linha do material chega no slot da
// instancia.
//
// CINCO observaveis, e cada um prova um campo:
//
//   SV_VertexID    — vem do INDEX BUFFER. Prova `StartIndexLocation`, e pelo conjunto de invocacoes,
//                    `IndexCountPerInstance`. NAO carrega `BaseVertexLocation`: medido em WARP.
//   SV_InstanceID  — 0-based DENTRO do comando. Prova `InstanceCount` pelo conjunto. NAO carrega
//                    `StartInstanceLocation`, pela mesma razao do de cima.
//   VertexValue    — vem do vertex buffer PER_VERTEX, onde `VB[k] == k`. O fetch e
//                    `VB[IndexBuffer[i] + BaseVertexLocation]`, entao a diferenca para o SV_VertexID
//                    E o campo.
//   InstanceValue  — vem do vertex buffer PER_INSTANCE, onde `IB[k] == k`. O fetch e
//                    `IB[SV_InstanceID + StartInstanceLocation]`, entao a diferenca para o
//                    SV_InstanceID E o campo.
//   as constantes  — provam as duas entradas de CONSTANT e a sobrevivencia das outras tres, que o
//                    passe amarra uma vez.
//
// Os dois buffers de atributo existem porque sem eles `BaseVertexLocation` e `StartInstanceLocation`
// nao tem efeito OBSERVAVEL nenhum, e o teste estaria declarando uma cobertura que nao tem.
//
// Este shader nao entra em nenhum passe.

#include "GpuIndirectDraw.hlsli"

cbuffer DrawConstantsCB : register(b4) {
    SMILE_DRAW_CONSTANTS_BLOCK
};

RWStructuredBuffer<uint4> Observed : register(u0);

struct FVertexIn {
    uint VertexValue   : TEXCOORD0; // per-vertex
    uint InstanceValue : TEXCOORD1; // per-instance
};

float4 main(FVertexIn In, uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
    : SV_Position {
    // Endereco por (vertice, instancia): o conjunto de posicoes escritas E o conjunto de invocacoes,
    // entao `IndexCountPerInstance` e `InstanceCount` errados nao produzem valor errado — produzem
    // escrita em outro lugar, ou a falta dela.
    const uint Base = (VertexId * SMILE_PROBE_MAX_INSTANCES + InstanceId) * 2u;
    Observed[Base + 0u] = uint4(MaterialRow, InstanceSlot, In.VertexValue, 1u);
    Observed[Base + 1u] = uint4(MaterialTableSrv, TransformSrv, PrevTransformSrv, In.InstanceValue);
    // Fora do frustum: a sonda so existe pelo efeito colateral do UAV, e rasterizar nao acrescenta
    // nada ao que ela mede.
    return float4(2.0f, 2.0f, 0.5f, 1.0f);
}
