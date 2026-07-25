#ifndef SMILE_RAY_OFFSET_HLSLI
#define SMILE_RAY_OFFSET_HLSLI

// Os pisos do offset (kRayOriginFloorPerMeter / kRayOriginFloorMin) sao a FAMILIA (1) do perfil
// de epsilons — ver RayEpsilons.hlsli, onde estao definidos e documentados junto com as familias
// (2) intervalo do raio e (3) correspondencia temporal, que precisam ser calibradas em conjunto.
#include "RayEpsilons.hlsli"

// Origem robusta p/ raios que saem de uma superficie — Wächter & Binder, "A Fast and Robust
// Method for Avoiding Self-Intersection" (Ray Tracing Gems, cap. 6). Desloca a posicao em ULPs
// ao longo da normal GEOMETRICA operando na representacao inteira do float: o passo cresce com
// o expoente da coordenada (scale-invariant), sem constante de cena. Perto da origem (|p| <
// 1/32) o passo ULP degenera, entao cai num epsilon flutuante fixo.
float3 OffsetRayWB(float3 p, float3 n) {
    const float kOrigin     = 1.0f / 32.0f;
    const float kFloatScale = 1.0f / 65536.0f;
    const float kIntScale   = 256.0f;
    int3   ofI = int3(kIntScale * n);
    float3 pI  = asfloat(asint(p) + ((p < 0.0f) ? -ofI : ofI));
    return float3(abs(p.x) < kOrigin ? p.x + kFloatScale * n.x : pI.x,
                  abs(p.y) < kOrigin ? p.y + kFloatScale * n.y : pI.y,
                  abs(p.z) < kOrigin ? p.z + kFloatScale * n.z : pI.z);
}

// Variante p/ posicoes RECONSTRUIDAS do G-buffer (depth + normal octaedrica): Wächter/Binder
// assume o hit exato da propria geometria, mas a reconstrucao via InvViewProj + normal
// quantizada tem erro maior que ULP do hit. Soma o piso configuravel acima por cima do offset
// ULP. Substitui o normal-bias fixo de 0.2 que contaminava a medida do ReSTIR (pHat/Jacobiano/
// hitT do NRD nunca abaixo de ~0.2 em contato) e deslocava reflexos de contato em ate 20 cm.
float3 OffsetRayGBuffer(float3 p, float3 n, float camDist) {
    return OffsetRayWB(p, n) + n * max(kRayOriginFloorPerMeter * camDist, kRayOriginFloorMin);
}

#endif
