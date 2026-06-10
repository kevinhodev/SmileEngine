// Cascaded Shadow Maps — amostragem no shading da cena (incluído pelo Triangle.ps).
// Base ortográfica estilo Unreal: WorldToShadow[i] projeta worldPos -> UV[0,1] + depth[0,1]
// da cascata i. Fase 2: PCF Poisson 16-taps rotacionado por pixel (soft shadows) + blend
// entre cascatas + normal-offset bias (anti peter-panning).
#ifndef SMILE_CSM_COMMON
#define SMILE_CSM_COMMON

#define SMILE_CSM_MAX_CASCADES 4

cbuffer CSMCB : register(b3) {
    row_major float4x4 WorldToShadow[SMILE_CSM_MAX_CASCADES]; // world -> shadow UV+depth por cascata
    float4 CSMTexelWorld; // x..w = tamanho de 1 texel em mundo, por cascata (p/ normal-offset)
    float4 CSMParams;     // x = numCascades, y = depthBias (NDC z), z = invShadowRes, w = enabled
    float4 CSMParams2;    // x = normal-offset (texels), y = penumbra (texels), z = blend band, w = -
};

Texture2DArray         SunShadowMap : register(t11);
SamplerComparisonState ShadowCmp    : register(s2);

// Disco de Poisson 16 pontos em [-1,1] (kernel irregular -> menos banding que grid regular).
static const float2 kPoisson16[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

// Interleaved Gradient Noise (Jimenez) — ângulo de rotação por pixel p/ dither do kernel.
float CSM_IGN(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// PCF Poisson rotacionado numa cascata. uvz já em shadow space [0,1]³.
float CSM_PCF(float3 uvz, int cascade, float2 screenPos) {
    float refZ     = uvz.z - CSMParams.y;
    float radiusUV = max(CSMParams2.y, 1.0f) * CSMParams.z; // penumbra (texels) -> UV
    float a = CSM_IGN(screenPos) * 6.2831853f;
    float s, c; sincos(a, s, c);
    float2x2 rot = float2x2(c, -s, s, c);
    float sum = 0.0f;
    [unroll] for (int k = 0; k < 16; ++k) {
        float2 o = mul(rot, kPoisson16[k]) * radiusUV;
        // ComparisonFunc LESS_EQUAL -> 1 quando refZ <= storedZ (iluminado).
        sum += SunShadowMap.SampleCmpLevelZero(ShadowCmp, float3(uvz.xy + o, (float)cascade), refZ);
    }
    return sum * (1.0f / 16.0f);
}

static bool CSM_InBounds(float3 uvz) {
    return uvz.x > 0.0f && uvz.x < 1.0f &&
           uvz.y > 0.0f && uvz.y < 1.0f &&
           uvz.z > 0.0f && uvz.z < 1.0f;
}

// Visibilidade do sol no ponto: 1 = iluminado, 0 = em sombra. worldNormal = normal geométrica
// (normal-offset bias). screenPos = pixel (SV_Position.xy) p/ rotacionar o kernel por pixel.
float SampleCSM(float3 worldPos, float3 worldNormal, float2 screenPos) {
    if (CSMParams.w < 0.5f) return 1.0f; // sombras desligadas

    int   numC = (int)CSMParams.x;
    float band = CSMParams2.z; // largura da zona de blend (em UV) na borda da cascata
    [loop] for (int i = 0; i < numC; ++i) {
        // Normal-offset escalado pelo texel-em-mundo desta cascata.
        float3 p   = worldPos + worldNormal * (CSMTexelWorld[i] * CSMParams2.x);
        float3 uvz = mul(float4(p, 1.0f), WorldToShadow[i]).xyz; // ortho -> w == 1
        if (!CSM_InBounds(uvz)) continue; // fora desta cascata: tenta a próxima

        float vis = CSM_PCF(uvz, i, screenPos);

        // Blend p/ a próxima cascata perto da borda (mata a costura/degrau).
        if (band > 0.0f && i + 1 < numC) {
            float2 dd   = min(uvz.xy, 1.0f - uvz.xy);
            float  edge = min(dd.x, dd.y); // distância à borda mais próxima (UV)
            if (edge < band) {
                float3 p2   = worldPos + worldNormal * (CSMTexelWorld[i + 1] * CSMParams2.x);
                float3 uvz2 = mul(float4(p2, 1.0f), WorldToShadow[i + 1]).xyz;
                if (CSM_InBounds(uvz2)) {
                    float vis2 = CSM_PCF(uvz2, i + 1, screenPos);
                    vis = lerp(vis2, vis, saturate(edge / band)); // borda -> próxima cascata
                }
            }
        }
        return vis;
    }
    return 1.0f; // além da última cascata: sem sombra
}

// --- Debug: visualização das cascatas (tint por cascata) ---
bool CSM_DebugEnabled() { return CSMParams2.w > 0.5f; }

int CSM_SelectCascade(float3 worldPos, float3 worldNormal) {
    int numC = (int)CSMParams.x;
    [loop] for (int i = 0; i < numC; ++i) {
        float3 p   = worldPos + worldNormal * (CSMTexelWorld[i] * CSMParams2.x);
        float3 uvz = mul(float4(p, 1.0f), WorldToShadow[i]).xyz;
        if (CSM_InBounds(uvz)) return i;
    }
    return -1;
}

float3 CSM_CascadeColor(int i) {
    if (i == 0) return float3(1.0f, 0.4f, 0.4f); // vermelho  — cascata mais próxima
    if (i == 1) return float3(0.4f, 1.0f, 0.4f); // verde
    if (i == 2) return float3(0.4f, 0.6f, 1.0f); // azul
    if (i == 3) return float3(1.0f, 1.0f, 0.4f); // amarelo   — cascata mais distante
    return float3(1.0f, 1.0f, 1.0f);             // fora de todas
}

#endif // SMILE_CSM_COMMON
