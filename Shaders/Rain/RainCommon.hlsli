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
    float4 CurtainParams;           // F3: x = CurtainAmount x rain, y = queda (m/s),
                                    // F5: z = particulas ativas (cortina poupa o near), w = -
    float4 KeyLightDir;             // F3: xyz = dir PARA a key light (mundo)
    float4 KeyLightColor;           // F3: rgb = cor x intensidade da key light
    float4 SkyAmbientRain;          // F3: rgb = ambient fisico do ceu
    row_major float4x4 RainViewProj; // F5: view-proj FULL jitterada (particulas projetam mundo)
    float4 RainParticleParams;      // F5: x = raio XZ do box (m), y = altura (m), z = -,
                                    //     w = num de particulas
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
// bandMul aperta a banda suave: 1 = 1.5 m (wetness, suaviza borda de parede na resolucao
// do mapa); a cortina usa ~6 (0.25 m — marquise baixa a 1.3 m do ponto corta por inteiro,
// senao 30-60% da chuva vaza por baixo).
float SkyVisibilityBand(float3 worldPos, float bandMul) {
    if (RainOccParams.x < 0.5f) return 1.0f;

    float3 uvz = mul(float4(worldPos, 1.0f), RainOccMatrix).xyz; // ortho: w = 1
    if (any(uvz.xy != saturate(uvz.xy)) || uvz.z >= 1.0f) return 1.0f;

    // PCF 2x2 com pesos bilineares (compara ANTES, filtra depois — filtrar o depth cru
    // inventaria alturas no meio do ar na borda da marquise). Media igual dava escada
    // diagonal dura na resolucao do mapa (~23 cm/texel) — os "triangulos" do A/B.
    const float size = RainOccParams.w;
    float2 st = uvz.xy * size - 0.5f;
    float2 fl = floor(st);
    float2 f  = st - fl;
    int2   c  = int2(clamp(fl, 0.0f, size - 2.0f));

    float o[4];
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int i = 0; i < 2; ++i) {
        float mapZ = RainOccMap.Load(int3(c + int2(i, j), 0));
        // occluder acima do pixel (mapZ menor = mais perto do teto) alem do bias = coberto
        o[j * 2 + i] = saturate((uvz.z - RainOccParams.y - mapZ) * RainOccParams.z * bandMul);
    }
    float occ = lerp(lerp(o[0], o[1], f.x), lerp(o[2], o[3], f.x), f.y);
    return 1.0f - occ;
}

float SkyVisibility(float3 worldPos) { return SkyVisibilityBand(worldPos, 1.0f); }

// ---- F5: particulas de chuva (compartilhado entre RainParticles.vs e RainSplash.vs) ----

// Eixo 1D com wrap preso na camera mas world-fixed entre wraps:
// d/dcam[fmod(h*span - cam, span) + cam] = 0.
float RainWrapAxis(float h, float cam, float span) {
    float m = fmod(h * span - cam, span);
    if (m < 0.0f) m += span;
    return cam + m - span * 0.5f;
}

// Posicao procedural da gota `iid` no instante corrente (hash + tempo, box com wrap na
// camera). A MESMA conta alimenta a gota (RainParticles.vs) e o splash dela
// (RainSplash.vs) — o splash toca na fracao "subterranea" do ciclo da queda.
float3 RainParticlePos(uint iid, out float speed, out float2 h1out) {
    const float2 h0 = Hash22(float2((float)iid * 0.7219f, 17.31f));
    const float2 h1 = Hash22(float2((float)iid * 0.3471f, 91.17f));
    h1out = h1;

    const float boxR = RainParticleParams.x;
    const float boxH = RainParticleParams.y;
    speed = CurtainParams.y * (0.85f + h1.x * 0.5f);

    float3 p;
    p.x = RainWrapAxis(h0.x, CameraWorldPos.x, boxR * 2.0f);
    p.z = RainWrapAxis(h0.y, CameraWorldPos.z, boxR * 2.0f);
    float m = fmod(h1.y * boxH - CameraWorldPos.y + CameraWorldPos.w * speed, boxH);
    if (m < 0.0f) m += boxH;
    p.y = CameraWorldPos.y + (boxH - m) - boxH * 0.35f;
    return p;
}

// Depth do mapa filtrado bilinear — p/ comparacoes de altura RELATIVAS (drenagem), onde
// interpolar o depth e inofensivo e tira o serrilhado dos taps pontuais.
float OccDepthBilinear(float2 uv) {
    const float size = RainOccParams.w;
    float2 st = uv * size - 0.5f;
    float2 fl = floor(st);
    float2 f  = st - fl;
    int2   c  = int2(clamp(fl, 0.0f, size - 2.0f));
    float d00 = RainOccMap.Load(int3(c, 0));
    float d10 = RainOccMap.Load(int3(c + int2(1, 0), 0));
    float d01 = RainOccMap.Load(int3(c + int2(0, 1), 0));
    float d11 = RainOccMap.Load(int3(c + int2(1, 1), 0));
    return lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
}

#endif
