#ifndef SMILE_RAY_OFFSET_HLSLI
#define SMILE_RAY_OFFSET_HLSLI

#include "RayEpsilons.hlsli"

// CONTRATO DE CBUFFER: quem inclui este header precisa ter declarado RayEpsA/RayEpsB no proprio
// cbuffer b0, e o include tem que vir DEPOIS da declaracao. Mesmo padrao do HitShading.hlsli, que
// exige Scene/Instances/SkyViewLUT declarados. Os campos espelham o FRayEpsilonProfile do C++:
//   RayEpsA = x originFloorMin, y originFloorPerMeter, z angularMax, w shadowRayBiasMin
//   RayEpsB = x shadowRayTMin,  y visRayTMin,          z visRayEndMargin, w angularMinRatio

// Normalizacao segura p/ direcoes que podem degenerar. Os call sites do offset calculam a direcao
// ANTES de validar o comprimento do segmento (a direcao exata depende da origem, que depende do
// offset), entao conexao degenerada daria NaN/Inf — e ele entraria justo no dot() do termo
// angular, contaminando a origem. Fallback = a propria normal (bias maximo, o lado seguro).
float3 SafeRayDir(float3 d, float3 fallback) {
    float l2 = dot(d, d);
    return (l2 > 1e-12f) ? (d * rsqrt(l2)) : fallback;
}

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
// assume o hit exato da propria geometria, mas a reconstrucao via InvViewProj + normal quantizada
// tem erro maior que ULP. Soma, por cima do offset ULP, os termos calibraveis do perfil.
//
// rayDir = direcao de SAIDA do raio (normalizada). So e usada pelo termo angular; quando ele esta
// desligado (angularMax == 0, o default) o valor nao importa.
float3 OffsetRayGBuffer(float3 p, float3 n, float3 rayDir, float camDist) {
    const float floorMin   = RayEpsA.x;
    const float perMeter   = RayEpsA.y;
    const float angularMax = RayEpsA.z;
    const float minRatio   = RayEpsB.w;

    // Termo constante + termo por distancia da camera (o que existia antes da exposicao).
    float bias = max(perMeter * camDist, floorMin);

    // Termo ANGULAR, estilo Lumen (ApplyPositionBias em RayTracingCommon.ush): o raio rasante e o
    // que auto-intersecta, entao leva o maximo; o perpendicular leva minRatio * maximo. O Lumen
    // usa 0,5 mm / 0,1 mm com a shading normal — mas ele opera em coordenadas camera-relative e
    // nao tem termo por distancia, entao os numeros dele nao transplantam direto.
    //
    // ADITIVO aos termos acima de proposito: o sweep precisa isolar um eixo de cada vez, e zerar
    // um nao pode desligar o outro.
    if (angularMax > 0.0f)
        bias += lerp(angularMax, angularMax * minRatio, saturate(dot(n, rayDir)));

    return OffsetRayWB(p, n) + n * bias;
}

#endif
