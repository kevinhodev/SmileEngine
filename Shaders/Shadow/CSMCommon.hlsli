#ifndef SMILE_CSM_COMMON
#define SMILE_CSM_COMMON

#define SMILE_CSM_MAX_CASCADES 4

cbuffer CSMCB : register(b3) {
    row_major float4x4 WorldToShadow[SMILE_CSM_MAX_CASCADES];
    float4 CSMTexelWorld;
    float4 CSMParams;
    float4 CSMParams2;
    float4 CSMParams3;    // x = frame do ruido (0 quando TAA/FSR off), y = tan(meio-angulo do sol; 0 = PCSS off), z = penumbra max em texels, w reservado
    float4 CSMBiasScale;  // multiplicador do depth bias por cascata
    float4 CSMDepthRangeWorld; // extensao em mundo do range de depth do ortho, por cascata (PCSS)
    float4 CSMCascadeSplits;   // far view-depth de cada cascata
    float4 CSMCameraPosition;  // xyz = camera em mundo
    float4 CSMCameraForwardNear; // xyz = frente da camera, w = near plane
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

        // TAP CENTRAL, obrigatorio: o disco kPoisson16 nao tem amostra no centro — a mais
        // interna esta a 0,20 do raio, ou seja 1,6 texels com penumbra maxima 8. Um oclusor
        // mais fino que isso (poste, grade, galho, cabo) passa ENTRE os taps e a busca
        // devolve "sem oclusor". O tap central le o texel exato do receptor e fecha o buraco.
        {
            int2  t = clamp(int2(uvz.xy * (float)R), int2(0, 0), int2(R - 1, R - 1));
            float d = SunShadowMap.Load(int4(t, cascade, 0)).r;
            if (d < refZ) { sum += d; cnt += 1.0f; }
        }
        [unroll] for (int k = 0; k < 16; ++k) {
            float2 uv = uvz.xy + mul(rot, kPoisson16[k]) * searchUV;
            int2   t  = clamp(int2(uv * (float)R), int2(0, 0), int2(R - 1, R - 1));
            float  d  = SunShadowMap.Load(int4(t, cascade, 0)).r;
            if (d < refZ) { sum += d; cnt += 1.0f; }
        }

        // Miss do blocker search NAO significa "iluminado". A busca e esparsa (17 taps sobre
        // um disco de ~201 texels^2 = 1 tap por 12 texels^2), entao um miss so diz que ela
        // nao encontrou nada — e o `return 1.0f` que morava aqui transformava isso em luz
        // plena, apagando a sombra e fazendo-a piscar junto com a rotacao por pixel/frame.
        // O certo e cair no PCF de raio minimo, que ainda le o texel do proprio receptor.
        // Mesma politica da Cry (ShadowCommon.cfi): ela reescala o kernel no contact
        // hardening e sempre filtra, nunca devolve lit no miss.
        texels = 1.0f;
        if (cnt >= 0.5f) {
            float avgBlocker = sum / cnt;
            float distWorld  = (refZ - avgBlocker) * CSMDepthRangeWorld[cascade];
            texels = clamp(distWorld * CSMParams3.y / CSMTexelWorld[cascade], 1.0f, CSMParams3.z);
        }
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

float CSM_ViewDepth(float3 worldPos) {
    return dot(worldPos - CSMCameraPosition.xyz, CSMCameraForwardNear.xyz);
}

// Cascades describe intervals along the camera frustum. Selecting the first
// overlapping light-space box is incorrect: the fitting spheres overlap heavily
// and expose their rectangular boundary as a moving shell on terrain and fog.
int CSM_CascadeFromViewDepth(float viewDepth) {
    const int numC = (int)CSMParams.x;
    [loop] for (int i = 0; i < numC; ++i) {
        if (viewDepth <= CSMCascadeSplits[i]) return i;
    }
    return -1;
}

float CSM_CurrentCascadeWeight(int cascade, float viewDepth) {
    const float splitNear = cascade > 0
        ? CSMCascadeSplits[cascade - 1] : CSMCameraForwardNear.w;
    const float splitFar = CSMCascadeSplits[cascade];
    const float transitionLength = max(
        (splitFar - splitNear) * saturate(CSMParams2.z), 1.0e-4f);
    // 1 fora da faixa; 1 -> 0 nos ultimos N% antes do split.
    return saturate((splitFar - viewDepth) / transitionLength);
}

float SampleCSM(float3 worldPos, float3 worldNormal, float2 screenPos) {
    if (CSMParams.w < 0.5f) return 1.0f; 

    const int numC = (int)CSMParams.x;
    const float viewDepth = CSM_ViewDepth(worldPos);
    int cascade = CSM_CascadeFromViewDepth(viewDepth);
    if (cascade < 0) return 1.0f;

    float3 p = worldPos + worldNormal * (CSMTexelWorld[cascade] * CSMParams2.x);
    float3 uvz = mul(float4(p, 1.0f), WorldToShadow[cascade]).xyz;
    if (!CSM_InBounds(uvz)) {
        // Normal offset ou cache podem empurrar um receptor para fora por poucos
        // texels. Cascatas maiores ainda sao um fallback valido.
        [loop] for (int fallback = cascade + 1; fallback < numC; ++fallback) {
            p = worldPos + worldNormal * (CSMTexelWorld[fallback] * CSMParams2.x);
            uvz = mul(float4(p, 1.0f), WorldToShadow[fallback]).xyz;
            if (CSM_InBounds(uvz)) return CSM_PCF(uvz, fallback, screenPos);
        }
        return 1.0f;
    }

    float vis = CSM_PCF(uvz, cascade, screenPos);
    if (CSMParams2.z > 0.0f) {
        const float currentWeight = CSM_CurrentCascadeWeight(cascade, viewDepth);
        if (currentWeight < 1.0f) {
            if (cascade + 1 < numC) {
                const int nextCascade = cascade + 1;
                const float3 p2 = worldPos + worldNormal *
                    (CSMTexelWorld[nextCascade] * CSMParams2.x);
                const float3 uvz2 = mul(float4(p2, 1.0f),
                                        WorldToShadow[nextCascade]).xyz;
                if (CSM_InBounds(uvz2)) {
                    const float vis2 = CSM_PCF(uvz2, nextCascade, screenPos);
                    vis = lerp(vis2, vis, currentWeight);
                }
            } else {
                // Ultima cascata: fade por view-depth ate iluminado em MaxDistance.
                vis = lerp(1.0f, vis, currentWeight);
            }
        }
    }
    return vis;
}

// Single-tap CSM sampling for participating media. A wide PCF kernel is not
// readable at froxel/half resolution, but the cascade transition still MUST use
// both maps. Without this, the different resolution/bias/caster sets of cascades
// 1 and 2 become a horizontal bright/dark shell in fog and god rays.
// Uma amostra no meio participante esta NO AR: nao ha superficie, nao ha normal, nao ha
// auto-sombreamento a combater. A unica coisa que um depth bias faz aqui e DESLOCAR o feixe
// ao longo da direcao da luz. O bias antigo (2x o do opaco) valia 15 cm na cascata 0 e 2,6 m
// na cascata 3 — e exatamente o sintoma de "o shaft comeca tarde demais e desgruda da janela
// que o produziu". A Cry usa o MESMO fDepthTestBias do opaco no fog volumetrico, nunca o
// dobro; aqui fica so um epsilon contra precisao de float na comparacao.
//
// Se o A/B mostrar ruido de comparacao onde o froxel encosta na geometria, o remedio e
// deslocar a AMOSTRA ao longo do raio (jitter/conservative depth), nao reintroduzir bias.
static const float kCSMVolumeEps = 1.0e-5f;

float CSM_VolumeTap(float3 uvz, int cascade) {
    return SunShadowMap.SampleCmpLevelZero(
        ShadowCmp, float3(uvz.xy, (float)cascade), uvz.z - kCSMVolumeEps);
}

float SampleCSMVolumetric(float3 worldPos) {
    if (CSMParams.w < 0.5f) return 1.0f;

    const int numC = (int)CSMParams.x;
    const float viewDepth = CSM_ViewDepth(worldPos);
    int cascade = CSM_CascadeFromViewDepth(viewDepth);
    if (cascade < 0) return 1.0f;

    float3 uvz = mul(float4(worldPos, 1.0f), WorldToShadow[cascade]).xyz;
    if (!CSM_InBounds(uvz)) {
        [loop] for (int fallback = cascade + 1; fallback < numC; ++fallback) {
            uvz = mul(float4(worldPos, 1.0f), WorldToShadow[fallback]).xyz;
            if (CSM_InBounds(uvz)) return CSM_VolumeTap(uvz, fallback);
        }
        return 1.0f;
    }

    float vis = CSM_VolumeTap(uvz, cascade);
    if (CSMParams2.z > 0.0f) {
        const float currentWeight = CSM_CurrentCascadeWeight(cascade, viewDepth);
        if (currentWeight < 1.0f) {
            if (cascade + 1 < numC) {
                const float3 uvz2 = mul(float4(worldPos, 1.0f),
                                        WorldToShadow[cascade + 1]).xyz;
                if (CSM_InBounds(uvz2)) {
                    const float vis2 = CSM_VolumeTap(uvz2, cascade + 1);
                    vis = lerp(vis2, vis, currentWeight);
                }
            } else {
                vis = lerp(1.0f, vis, currentWeight);
            }
        }
    }
    return vis;
}

bool CSM_DebugEnabled() { return CSMParams2.w > 0.5f; }

int CSM_SelectCascade(float3 worldPos, float3 worldNormal) {
    return CSM_CascadeFromViewDepth(CSM_ViewDepth(worldPos));
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
