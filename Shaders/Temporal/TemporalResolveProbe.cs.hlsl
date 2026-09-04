// Sonda de teste do resolvedor de identidade temporal.
//
// Existe porque a cobertura anterior era um gate de STRINGS sobre o fonte do shader, e gate de
// string so pega o que muda o texto de forma prevista: trocar "||" por "&&" na comparacao de
// identidade mantem as duas substrings, passava verde, e o resolvedor passava a aceitar o ocupante
// novo de um slot reciclado. Medido.
//
// Aqui o resolvedor de PRODUCAO e executado de verdade. Roda em WARP (rasterizador de software da
// Microsoft), entao nao exige placa e cabe no CI.
//
// Este shader nao entra em nenhum passe -- ele so expoe TemporalResolvePreviousIndex para o teste.

#include "TemporalMotionCommon.hlsli"

cbuffer ProbeCB : register(b0) {
    uint4 Params; // x=slotCapacity, y=currentCount, z=quantidade de casos
};

StructuredBuffer<FGpuIdentityEntry> PreviousIdentity   : register(t0);
StructuredBuffer<uint>              SlotToCurrentIndex : register(t1);
StructuredBuffer<FGpuIdentityEntry> CurrentIdentity    : register(t2);
// x = id anterior, y = slotCapacity, z = currentCount DAQUELE caso. Por caso, e nao no cbuffer,
// para os limites serem exercitados na mesma dispatch em vez de exigir uma por cenario.
StructuredBuffer<uint4>             PreviousIds        : register(t3);

// x = resolveu (0/1), y = indice atual devolvido.
RWStructuredBuffer<uint2> Results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    if (i >= Params.z) return;

    uint index = 0xFFFFFFFFu;
    const uint4 caso = PreviousIds[i];
    const bool ok = TemporalResolvePreviousIndex(caso.x,
                                                 PreviousIdentity,
                                                 SlotToCurrentIndex,
                                                 CurrentIdentity,
                                                 caso.y,
                                                 caso.z,
                                                 index);
    Results[i] = uint2(ok ? 1u : 0u, index);
}
