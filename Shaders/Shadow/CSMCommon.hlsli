#ifndef SMILE_CSM_COMMON
#define SMILE_CSM_COMMON

#define SMILE_CSM_MAX_CASCADES 4

cbuffer CSMCB : register(b3) {
    row_major float4x4 WorldToShadow[SMILE_CSM_MAX_CASCADES];
    float4 CSMTexelWorld;
    float4 CSMParams;
    float4 CSMParams2;
    float4 CSMParams3;    // x = frame do ruido (0 quando TAA/FSR2 off), y = tan(meio-angulo do sol; 0 = PCSS off), z = penumbra max em texels, w reservado
    float4 CSMBiasScale;  // multiplicador do depth bias por cascata
    float4 CSMDepthRangeWorld; // extensao em mundo do range de depth do ortho, por cascata (PCSS)
};

Texture2DArray         SunShadowMap : register(t11);
SamplerComparisonState ShadowCmp    : register(s2);

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

float CSM_IGN(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float CSM_PCF(float3 uvz, int cascade, float2 screenPos) {
    float refZ = uvz.z - CSMParams.y * CSMBiasScale[cascade];
    float a = CSM_IGN(screenPos + 5.588238f * CSMParams3.x) * 6.2831853f;
    float s, c; sincos(a, s, c);
    float2x2 rot = float2x2(c, -s, s, c);

    // Penumbra ~constante em mundo: o raio em texels da cascata i encolhe pela razao
    // texel0/texelI (piso 1 texel), senao a penumbra salta ~4x a cada troca de cascata
    // e a migracao com a camera fica visivel ("sombra respirando").
    float texels = max(CSMParams2.y * (CSMTexelWorld[0] / CSMTexelWorld[cascade]), 1.0f);

    // PCSS (contact hardening) nas cascatas 0-1: blocker search estima a distancia media
    // dos oclusores e a penumbra cresce com ela (penumbra = dist * tan(meio-angulo do
    // sol)); no contato o kernel colapsa pra 1 texel = sombra firme. Cascatas 2-3 seguem
    // no raio fixo (penumbra variavel nao e legivel a centenas de metros). O teto do
    // kernel e em texels da PROPRIA cascata (padrao UE) — na fronteira 0->1 sombras
    // muito difusas podem abrir um degrau sutil de suavidade, mascarado pelo blend band.
    if (cascade <= 1 && CSMParams3.y > 0.0f) {
        const int R = (int)(1.0f / CSMParams.z);
        float searchUV = CSMParams3.z * CSMParams.z; // raio de busca = penumbra maxima
        float sum = 0.0f, cnt = 0.0f;
        [unroll] for (int k = 0; k < 16; ++k) {
            float2 uv = uvz.xy + mul(rot, kPoisson16[k]) * searchUV;
            int2   t  = clamp(int2(uv * (float)R), int2(0, 0), int2(R - 1, R - 1));
            float  d  = SunShadowMap.Load(int4(t, cascade, 0)).r;
            if (d < refZ) { sum += d; cnt += 1.0f; }
        }
        if (cnt < 0.5f) return 1.0f; // nenhum oclusor no raio de busca = totalmente lit
        float avgBlocker = sum / cnt;
        float distWorld  = (refZ - avgBlocker) * CSMDepthRangeWorld[cascade];
        texels = clamp(distWorld * CSMParams3.y / CSMTexelWorld[cascade], 1.0f, CSMParams3.z);
    }

    float radiusUV = texels * CSMParams.z;
    float vis = 0.0f;
    [unroll] for (int k = 0; k < 16; ++k) {
        float2 o = mul(rot, kPoisson16[k]) * radiusUV;
        vis += SunShadowMap.SampleCmpLevelZero(ShadowCmp, float3(uvz.xy + o, (float)cascade), refZ);
    }
    return vis * (1.0f / 16.0f);
}

static bool CSM_InBounds(float3 uvz) {
    return uvz.x > 0.0f && uvz.x < 1.0f &&
           uvz.y > 0.0f && uvz.y < 1.0f &&
           uvz.z > 0.0f && uvz.z < 1.0f;
}

float SampleCSM(float3 worldPos, float3 worldNormal, float2 screenPos) {
    if (CSMParams.w < 0.5f) return 1.0f; 

    int   numC = (int)CSMParams.x;
    float band = CSMParams2.z; 
    [loop] for (int i = 0; i < numC; ++i) {
        float3 p   = worldPos + worldNormal * (CSMTexelWorld[i] * CSMParams2.x);
        float3 uvz = mul(float4(p, 1.0f), WorldToShadow[i]).xyz; 
        if (!CSM_InBounds(uvz)) continue; 

        float vis = CSM_PCF(uvz, i, screenPos);

        if (band > 0.0f) {
            float2 dd   = min(uvz.xy, 1.0f - uvz.xy);
            float  edge = min(dd.x, dd.y);
            if (edge < band) {
                if (i + 1 < numC) {
                    float3 p2   = worldPos + worldNormal * (CSMTexelWorld[i + 1] * CSMParams2.x);
                    float3 uvz2 = mul(float4(p2, 1.0f), WorldToShadow[i + 1]).xyz;
                    if (CSM_InBounds(uvz2)) {
                        float vis2 = CSM_PCF(uvz2, i + 1, screenPos);
                        vis = lerp(vis2, vis, saturate(edge / band));
                    }
                } else {
                    // Ultima cascata: fade pra iluminado na borda, em vez de corte seco
                    // da sombra em ShadowMaxDistance.
                    vis = lerp(1.0f, vis, saturate(edge / band));
                }
            }
        }
        return vis;
    }
    return 1.0f; 
}

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

// Paleta daltonico-safe (vermelho-verde): azul / ciano / laranja / magenta.
float3 CSM_CascadeColor(int i) {
    if (i == 0) return float3(0.30f, 0.50f, 1.00f);
    if (i == 1) return float3(0.30f, 1.00f, 1.00f);
    if (i == 2) return float3(1.00f, 0.60f, 0.20f);
    if (i == 3) return float3(1.00f, 0.30f, 1.00f);
    return float3(1.0f, 1.0f, 1.0f);
}

#endif 
