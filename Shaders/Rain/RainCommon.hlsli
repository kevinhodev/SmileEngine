#ifndef RAIN_COMMON_HLSLI
#define RAIN_COMMON_HLSLI

// Comum do sistema de chuva (wetness F1/F2 + cortina F3). O layout do cbuffer ESPELHA
// RainWetnessConstants (RainWetness.h) — mudou la, muda aqui.

cbuffer RainCB : register(b0) {
    row_major float4x4 InvViewProj; // inversa FULL da view-proj jitterada
    float4 CameraWorldPos;          // xyz = camera (mundo), w = tempo (s)
    float4 RainParams0;             // x = RainAmount, y = PuddleAmount, z = RippleStrength,
                                    // w = WetDarkening
    float4 RainParams1;             // x = 1/PuddleScale (1/m), y = 1/range vertical do ortho
                                    // (depth por metro), zw = -
    row_major float4x4 RainOccMatrix; // F2: world -> UVZ do mapa de oclusao (ortho top-down)
    float4 RainOccParams;           // x = enabled, y = bias (depth), z = 1/banda (depth),
                                    // w = resolucao do mapa
    float4 CurtainParams;           // F3: x = CurtainAmount, y = queda (m/s), zw = -
    float4 KeyLightDir;             // F3: xyz = dir PARA a key light (mundo)
    float4 KeyLightColor;           // F3: rgb = cor x intensidade da key light
    float4 SkyAmbientRain;          // F3: rgb = ambient fisico do ceu
};

Texture2D<float> SceneDepth : register(t2);
Texture2D<float> RainOccMap : register(t3); // F2: depth top-down cacheado (0 = teto do volume)

// ---- Ruido (procedural, sem textura) ----

float2 Hash22(float2 p) {
    // hash de Dave Hoskins (sem sin — estavel em qualquer GPU/distancia)
    float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float Hash21(float2 p) { return Hash22(p).x; }

// F2: "esse ponto ve o ceu?" — compara a altura do pixel com o depth top-down cacheado
// (estilo rain occlusion da Cry). 1 = exposto (molha), 0 = coberto (seco). Fora do volume
// do mapa = exposto: o mapa acompanha a camera, longe dela o erro nao e visivel.
float SkyVisibility(float3 worldPos) {
    if (RainOccParams.x < 0.5f) return 1.0f;

    float3 uvz = mul(float4(worldPos, 1.0f), RainOccMatrix).xyz; // ortho: w = 1
    if (any(uvz.xy != saturate(uvz.xy)) || uvz.z >= 1.0f) return 1.0f;

    // 2x2 taps (Load; borda suave vem da banda em profundidade, nao precisa de PCF grande)
    const float size = RainOccParams.w;
    int2 c = int2(clamp(uvz.xy * size - 0.5f, 0.0f, size - 2.0f));
    float occ = 0.0f;
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int i = 0; i < 2; ++i) {
        float mapZ = RainOccMap.Load(int3(c + int2(i, j), 0));
        // occluder acima do pixel (mapZ menor = mais perto do teto) alem do bias = coberto
        occ += saturate((uvz.z - RainOccParams.y - mapZ) * RainOccParams.z);
    }
    return 1.0f - occ * 0.25f;
}

#endif
