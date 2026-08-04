#ifndef SMILE_CSM_COMMON
#define SMILE_CSM_COMMON

#define SMILE_CSM_MAX_CASCADES 4

cbuffer CSMCB : register(b3) {
    row_major float4x4 WorldToShadow[SMILE_CSM_MAX_CASCADES];
    float4 CSMTexelWorld;
    float4 CSMParams;
    float4 CSMParams2;
    float4 CSMParams3;    // x = frame do ruido (0 quando TAA/FSR off), y = tan(meio-angulo do sol; 0 = PCSS off), z = penumbra max em texels, w reservado
    // Depth bias JA EM NDC e JA por cascata (texels * texelWorld / rangeWorld * escala do
    // usuario), resolvido na CPU. Nao multiplicar por mais nada aqui.
    float4 CSMBiasNdc;
    float4 CSMDepthRangeWorld; // extensao em mundo do range de depth do ortho, por cascata (PCSS)
    float4 CSMCascadeSplits;   // far view-depth de cada cascata
    float4 CSMCameraPosition;  // xyz = camera em mundo
    float4 CSMCameraForwardNear; // xyz = frente da camera, w = near plane
    float4 CSMSunDirection;    // xyz = direcao PARA a key light
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

// Optimized PCF do The Witness (Castano), na forma do sample do MJP e do
// SampleShadowMapOptimizedPCF da Flax. Cada SampleCmpLevelZero ja e um PCF bilinear 2x2
// do TMU; escolhendo offsets e pesos em funcao da posicao FRACIONARIA do receptor dentro
// do texel (s, t), a soma ponderada de 9 taps reproduz EXATAMENTE um box 5x5 uniforme —
// nao e aproximacao. Custa ((N+1)/2)^2 em vez de N^2.
//
// Substitui os 16 taps Poisson rotados no caminho de raio fixo. Ali o raio pedido era
// sub-texel da cascata 1 em diante (0,63 / 0,18 / 0,06), colidia com o piso e os 16 taps
// caiam todos dentro de UM texel — 16 SampleCmp para produzir o que 1 produzia. Alem de
// caro, dependia do ruido animado, e portanto do TAA, para nao virar padrao fixo.
// Determinístico: sem ruido, sem boiling em disocclusion, sem depender do upscaler.
float CSM_OptimizedPCF(float3 uvz, int cascade, float refZ) {
    const float size    = 1.0f / CSMParams.z;   // resolucao do mapa
    const float invSize = CSMParams.z;

    float2 uv     = uvz.xy * size;
    float2 baseUV = floor(uv + 0.5f);
    float  s      = uv.x + 0.5f - baseUV.x;
    float  t      = uv.y + 0.5f - baseUV.y;
    baseUV        = (baseUV - 0.5f) * invSize;

    const float uw0 = 4.0f - 3.0f * s, uw1 = 7.0f, uw2 = 1.0f + 3.0f * s;
    const float u0  = (3.0f - 2.0f * s) / uw0 - 2.0f;
    const float u1  = (3.0f + s) / uw1;
    const float u2  = s / uw2 + 2.0f;

    const float vw0 = 4.0f - 3.0f * t, vw1 = 7.0f, vw2 = 1.0f + 3.0f * t;
    const float v0  = (3.0f - 2.0f * t) / vw0 - 2.0f;
    const float v1  = (3.0f + t) / vw1;
    const float v2  = t / vw2 + 2.0f;

    const float3 uu = float3(u0, u1, u2), vv = float3(v0, v1, v2);
    const float3 uw = float3(uw0, uw1, uw2), vw = float3(vw0, vw1, vw2);

    float sum = 0.0f;
    [unroll] for (int j = 0; j < 3; ++j) {
        [unroll] for (int i = 0; i < 3; ++i) {
            const float2 o = float2(uu[i], vv[j]) * invSize;
            sum += uw[i] * vw[j] * SunShadowMap.SampleCmpLevelZero(
                       ShadowCmp, float3(baseUV + o, (float)cascade), refZ);
        }
    }
    return sum * (1.0f / 144.0f);
}

float CSM_PCF(float3 uvz, int cascade, float2 screenPos) {
    float refZ = uvz.z - CSMBiasNdc[cascade];

    // Duas familias de filtro, separadas como a Unreal separa (r.Shadow.FilterMethod):
    // raio FIXO -> optimized PCF determinístico; penumbra VARIAVEL do PCSS -> disco de
    // Poisson rotado, que e o unico que aceita raio arbitrario por pixel. A tentativa de
    // manter penumbra constante em mundo escalando o raio por texel0/texelI saiu: ela era
    // anulada pelo piso de 1 texel a partir da cascata 1 (5,9 -> 9,3 -> 33 -> 101 cm) e
    // nenhuma das tres engines de referencia tenta isso.
    const bool pcss = (cascade <= 1) && (CSMParams3.y > 0.0f);
    if (!pcss) return CSM_OptimizedPCF(uvz, cascade, refZ);

    // Piso da penumbra, em texels. Casado com a meia-largura do box 5x5 acima para que a
    // troca entre as duas familias seja continua: no contato, onde o PCSS colapsa, os dois
    // filtros tem o mesmo alcance.
    const float minTexels = max(CSMParams2.y, 1.0f);

    float a = CSM_IGN(screenPos + 5.588238f * CSMParams3.x) * 6.2831853f;
    float s, c; sincos(a, s, c);
    float2x2 rot = float2x2(c, -s, s, c);
    float texels = minTexels;

    // PCSS (contact hardening) nas cascatas 0-1: blocker search estima a distancia media
    // dos oclusores e a penumbra cresce com ela (penumbra = dist * tan(meio-angulo do
    // sol)); no contato o kernel colapsa e a sombra fica firme. Cascatas 2-3 seguem no
    // raio fixo (penumbra variavel nao e legivel a centenas de metros). O teto do kernel
    // e em texels da PROPRIA cascata (padrao UE) — na fronteira 0->1 sombras muito difusas
    // podem abrir um degrau sutil de suavidade, mascarado pelo blend band.
    {
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
        if (cnt >= 0.5f) {
            float avgBlocker = sum / cnt;
            float distWorld  = (refZ - avgBlocker) * CSMDepthRangeWorld[cascade];
            texels = clamp(distWorld * CSMParams3.y / CSMTexelWorld[cascade],
                           minTexels, max(CSMParams3.z, minTexels));
        }
    }

    // Penumbra no piso = contato: o disco de 16 taps nao acrescenta nada sobre o box 5x5
    // determinístico do mesmo alcance, e custa 16 SampleCmp em vez de 9 mais o ruido. Com
    // o sol a 0,53 graus e piso de 2 texels, o oclusor precisa estar a mais de 10 m na
    // cascata 0 e a mais de 40 m na cascata 1 para sair do piso — ou seja, na cascata 1
    // (18-80 m) o caso COMUM cai aqui, e ela quase nunca pagava o disco por um resultado
    // que ja estava grampeado.
    if (texels <= minTexels + 1.0e-3f) return CSM_OptimizedPCF(uvz, cascade, refZ);

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

    // Normal offset proporcional a sin(alfa), com alfa o angulo entre a normal e a luz
    // (Castano / The Witness). O erro que este offset combate e a quantizacao do
    // rasterizador do shadow map: um texel inteiro guarda uma unica profundidade, e o
    // deslocamento LATERAL necessario para sair da celula errada vale texel * sin(alfa).
    // Em N.L = 1 esse erro e zero e um offset constante so produzia peter-panning de
    // graca, comendo o contato e o auto-sombreamento fino; em N.L -> 0 ele e maximo e o
    // valor constante ficava curto, deixando acne na faixa do terminador — a regiao mais
    // visivel da cena. A Flax usa saturate(1 - N.L), que subestima no angulo medio (em
    // N.L = 0,5 da 0,5 contra o 0,866 correto); aqui vai o seno exato, que custa o mesmo.
    //
    // Nao depende da cascata, entao serve aos tres caminhos abaixo — inclusive o do blend,
    // onde o offset PRECISA ser refeito com o texel da cascata seguinte.
    const float NoL = saturate(dot(worldNormal, CSMSunDirection.xyz));
    const float offsetScale = CSMParams2.x * sqrt(saturate(1.0f - NoL * NoL));

    float3 p = worldPos + worldNormal * (CSMTexelWorld[cascade] * offsetScale);
    float3 uvz = mul(float4(p, 1.0f), WorldToShadow[cascade]).xyz;
    if (!CSM_InBounds(uvz)) {
        // Normal offset ou cache podem empurrar um receptor para fora por poucos
        // texels. Cascatas maiores ainda sao um fallback valido.
        [loop] for (int fallback = cascade + 1; fallback < numC; ++fallback) {
            p = worldPos + worldNormal * (CSMTexelWorld[fallback] * offsetScale);
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
                    (CSMTexelWorld[nextCascade] * offsetScale);
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
