// Observador da sonda de comando indireto (Fase 5 do GPU-DRIVEN-PLAN).
//
// Roda DENTRO de um ExecuteIndirect e relata o que o command processor entregou. E a unica forma de
// provar o contrato: o layout do argumento nao e escolhido pela struct, e ditado pela ORDEM das
// entradas em `pArgumentDescs`, e nada do lado do shader valida isso. Com as duas entradas de
// constante trocadas, o codigo compila, o comando executa, e a linha do material chega no slot da
// instancia.
//
// TRES observaveis, e cada um prova um grupo de campos:
//
//   SV_VertexID      — vem do INDEX BUFFER. Prova `StartIndexLocation` e, pelo numero de invocacoes,
//                      `IndexCountPerInstance`. NAO carrega `BaseVertexLocation`: medido em WARP, o
//                      valor chega sem ele.
//   VertexValue      — vem do VERTEX BUFFER, onde `VB[k] == k`. Como o fetch e
//                      `VB[IndexBuffer[i] + BaseVertexLocation]`, a diferenca para o SV_VertexID E
//                      o `BaseVertexLocation`. Sem vertex buffer o campo seria inobservavel, e o
//                      teste estaria declarando uma cobertura que nao tem.
//   as constantes    — provam as duas entradas de CONSTANT e a sobrevivencia das outras tres, que o
//                      passe amarra uma vez.
//
// Este shader nao entra em nenhum passe.

#include "GpuIndirectDraw.hlsli"

cbuffer DrawConstantsCB : register(b4) {
    SMILE_DRAW_CONSTANTS_BLOCK
};

RWStructuredBuffer<uint4> Observed : register(u0);

struct FVertexIn {
    uint Value : TEXCOORD0;
};

float4 main(FVertexIn In, uint VertexId : SV_VertexID) : SV_Position {
    Observed[VertexId * 2u + 0u] = uint4(MaterialRow, InstanceSlot, In.Value, 1u);
    Observed[VertexId * 2u + 1u] = uint4(MaterialTableSrv, TransformSrv, PrevTransformSrv, VertexId);
    // Fora do frustum: a sonda so existe pelo efeito colateral do UAV, e rasterizar nao acrescenta
    // nada ao que ela mede.
    return float4(2.0f, 2.0f, 0.5f, 1.0f);
}
