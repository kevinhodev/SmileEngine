#ifndef SMILE_PT_RNG_HLSLI
#define SMILE_PT_RNG_HLSLI

// ReSTIR PT — RNG deterministico STATELESS por caminho (pre-requisito do random replay, F2).
//
// O replay do hybrid shift exige reproduzir bit-exato as decisoes do caminho fonte em OUTRO
// pixel e OUTRO passe. Por isso o RNG nao carrega estado: cada numero e funcao pura de
// (pathSeed, bounce, dim). O seed (32 bits) vive no reservoir; o replay usa o seed ARMAZENADO
// do caminho fonte, nunca o recalcula do pixel.
//
// CONTRATO (nao violar): SampleBsdf/NEE consomem as dims em ordem fixa e SEMPRE as consomem,
// mesmo quando a amostra e rejeitada — o cursor de dims nunca pode divergir entre o sampling
// inicial e o replay. A dim de Russian roulette so e lida no sampling INICIAL (Enhanced §6.2.4);
// o replay nunca a consome.

#include "../Reflections/GGXSample.hlsli" // GGX_PCG

// Dimensoes por vertice do caminho.
#define PT_DIM_LOBE   0   // escolha difuso-vs-especular
#define PT_DIM_BSDF_U 1
#define PT_DIM_BSDF_V 2
#define PT_DIM_NEE_U  3   // amostra do cone do sol
#define PT_DIM_NEE_V  4
#define PT_DIM_RR     5   // Russian roulette — SO no sampling inicial

uint PTHash(uint seed, uint bounce, uint dim) {
    return GGX_PCG(seed ^ (bounce * 0x9E3779B9u) ^ (dim * 0x85EBCA6Bu));
}

float PTRand(uint seed, uint bounce, uint dim) {
    return (PTHash(seed, bounce, dim) >> 8) * 0x1p-24f; // 24 bits -> [0,1)
}

float2 PTRand2(uint seed, uint bounce, uint dim) {
    return float2(PTRand(seed, bounce, dim), PTRand(seed, bounce, dim + 1));
}

// Seed do caminho por pixel/frame (so no sampling inicial).
uint PTPathSeed(uint2 px, uint frame) {
    return GGX_PCG(px.x + GGX_PCG(px.y + GGX_PCG(frame)));
}

// Stream SEPARADO p/ decisoes de resampling (WRS, escolha de vizinho) — nao e replayado e nao
// contamina as dims do caminho. Salt por passe evita correlacao entre passes do mesmo frame.
uint PTResampleSeed(uint2 px, uint frame, uint passSalt) {
    return GGX_PCG(px.x + GGX_PCG(px.y + GGX_PCG(frame + passSalt * 0x1000193u)));
}
float PTResampleNext(inout uint s) {
    s = GGX_PCG(s);
    return (s & 0x00FFFFFFu) / 16777216.0f;
}

#endif
