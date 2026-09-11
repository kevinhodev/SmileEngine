// Produtor da sonda de comando indireto (Fase 5 do GPU-DRIVEN-PLAN).
//
// Monta `GpuIndirectDrawArgs` CAMPO A CAMPO a partir de uma entrada que nao compartilha layout
// nenhum com a saida — dois uint4 por comando. Copiar de uma `StructuredBuffer<GpuIndirectDrawArgs>`
// seria mais curto e nao provaria nada: o compilador viraria um memcpy, e um espelho HLSL com os
// campos trocados continuaria produzindo o mesmo bloco de bytes.
//
// Quem le o que sai daqui e o COMMAND PROCESSOR, nao um shader. E por isso que a sonda nao releia o
// buffer: releitura provaria o produtor. Quem prova o contrato e o ExecuteIndirect do teste.
//
// Este shader nao entra em nenhum passe.

#include "GpuIndirectDraw.hlsli"

cbuffer ProbeCB : register(b0) {
    uint4 Params; // x = quantos comandos escrever
};

// [2i+0] = (MaterialRow, InstanceSlot, IndexCountPerInstance, InstanceCount)
// [2i+1] = (StartIndexLocation, BaseVertexLocation como bits, StartInstanceLocation, 0)
StructuredBuffer<uint4> Specs : register(t0);

RWStructuredBuffer<GpuIndirectDrawArgs> Args : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.x) return;

    const uint4 A = Specs[i * 2u + 0u];
    const uint4 B = Specs[i * 2u + 1u];

    GpuIndirectDrawArgs Out;
    Out.MaterialRow           = A.x;
    Out.InstanceSlot          = A.y;
    Out.IndexCountPerInstance = A.z;
    Out.InstanceCount         = A.w;
    Out.StartIndexLocation    = B.x;
    Out.BaseVertexLocation    = asint(B.y);
    Out.StartInstanceLocation = B.z;
    Args[i] = Out;
}
