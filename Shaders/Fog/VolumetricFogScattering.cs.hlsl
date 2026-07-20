#include "VolumetricFogCommon.hlsli"
#include "../Shadow/CSMCommon.hlsli"
#include "../GI/DDGICommon.hlsli"

// LightScattering (UE): luz espalhada em direcao a camera por froxel.
// Sol (tap unico no CSM, receita dos sun shafts — penumbra nao e legivel em volume)
// com fase HG + ambiente via DDGI (fallback: SkyAmbient da atmosfera).
// F2: jitter Halton por frame + reprojecao temporal (historia ping-pong, blend 0.9)
// + supersampling quando a historia falha (fora do frustum/primeiro frame) — receita
// exata do LightScatteringCS da UE.
// F3: luzes puntuais — MESMA lista FGPULight do deferred (root SRV t3) com atenuacao
// identica (janela UE + bulbo Cry + cone quadratico), sombra por tap unico no atlas
// spot / cube array (caminho linear de refZ do deferred, sem normal-offset) e bias de
// falloff pelo raio do froxel (InverseSquaredLightDistanceBias da UE — sem ele a luz
// dentro de um voxel vira firefly).

// Espelha o FGPULight do Renderer.h (versao do deferred, com ShadowMatrix).
struct FGPULight {
    float4 PosInvRadius;      // xyz = posicao, w = 1/raio de atenuacao
    float4 ColorSourceRadius; // rgb = cor*intensidade, w = bulbo (distancia minima)
    float4 DirCosOuter;       // xyz = eixo do spot, w = cos(outer); -2 = point
    float4 SpotParams;        // x = 1/(cosInner-cosOuter), y = slice de sombra (-1 = sem)
    row_major float4x4 ShadowMatrix; // world -> UVZ do slice (dividir por w)
};

Texture3D<float4> VBufferA            : register(t0);
Texture2D<float4> DDGIIrradianceAtlas : register(t1);
Texture3D<float4> ScatteringHistory   : register(t2);
StructuredBuffer<FGPULight> LocalLights : register(t3);
Texture2DArray    LocalShadowMap      : register(t18); // atlas D32 dos spots (F3a)
TextureCubeArray  LocalCubeShadow     : register(t19); // cube array dos points (F3b)
Texture2D<float>  CloudShadowMap      : register(t4);  // transmitancia das nuvens (F4)
SamplerState      LinearClamp         : register(s0);

RWTexture3D<float4> LightScattering : register(u0);

// Visibilidade do sol p/ ponto no ar: mesma do SunShaftsVolumetric (sem normal-offset,
// bias fixo — acne nao existe em volume).
float VolFog_SunVis(float3 worldPos) {
    if (CSMParams.w < 0.5f) return 1.0f;
    int numC = (int)CSMParams.x;
    [loop] for (int i = 0; i < numC; ++i) {
        float3 uvz = mul(float4(worldPos, 1.0f), WorldToShadow[i]).xyz;
        if (!CSM_InBounds(uvz)) continue;
        float refZ = uvz.z - CSMParams.y * CSMBiasScale[i] * 2.0f;
        return SunShadowMap.SampleCmpLevelZero(ShadowCmp, float3(uvz.xy, (float)i), refZ);
    }
    return 1.0f; // fora do range do CSM = iluminado (igual as superficies)
}

// refZ NDC (LH 0..1) a partir da distancia LINEAR no eixo da face/cone — copia do
// LocalNdcDepth do deferred (bias em espaco linear, senao peter-panning na borda).
float VolFog_LocalNdcDepth(float viewZ, float farP) {
    float nearP = LightParamsVF2.x;
    float fRng  = farP / max(farP - nearP, 1e-4f);
    return fRng - (fRng * nearP) / max(viewZ, nearP);
}

float VolFog_LocalBias(float viewZ) { return LightParamsVF.z + viewZ * 0.008f; }

// Luz espalhada em direcao a camera num ponto do ar (posicao ja jitterada).
// cellRadius = raio do froxel em mundo (bias do falloff perto do centro da luz).
float3 VolFog_Lighting(float3 wp, float cellRadius) {
    float3 dir = normalize(wp - CameraWorldPos.xyz); // camera -> ponto

    // Sol: radiancia da key light x visibilidade CSM x fase HG (lobo contra a luz).
    float vis = VolFog_SunVis(wp);

    // F4: sombra das nuvens POR FROXEL (mesma projecao do deferred/shafts) — nuvem
    // corta o feixe no meio do ar; dia nublado apaga o sol do fog sozinho.
    if (CloudShadowParams.w > 0.0f && vis > 0.0f) {
        float  kKm   = CloudShadowParams2.x;
        float2 hitKm = wp.xz * kKm + CloudShadowParams2.zw *
                       max(CloudShadowParams2.y - wp.y * kKm, 0.0f);
        float2 suv   = (hitKm - CloudShadowParams.xy) * CloudShadowParams.z + 0.5f;
        if (all(suv > 0.0f) && all(suv < 1.0f)) {
            float Tc = CloudShadowMap.SampleLevel(LinearClamp, suv, 0.0f);
            vis *= lerp(1.0f, Tc, CloudShadowParams.w);
        }
    }

    float ph  = VolFog_PhaseHG(SunDirPhase.w, dot(SunDirPhase.xyz, -dir));
    float3 lighting = SunColorInt.rgb * (vis * ph);

    // Luzes puntuais: atenuacao identica ao deferred + fase HG + sombra por tap unico
    // (PCF nao e legivel em volume; o temporal integra o resto).
    uint numLights = (uint)LightParamsVF.x;
    float biasSqr  = max(cellRadius, 1.0f);
    biasSqr *= biasSqr;
    [loop] for (uint li = 0; li < numLights; ++li) {
        FGPULight Lp = LocalLights[li];

        float3 toL     = Lp.PosInvRadius.xyz - wp;
        float  distSqr = dot(toL, toL);
        float  invR    = Lp.PosInvRadius.w;

        float win = distSqr * invR * invR;      // (d/r)^2
        win = saturate(1.0f - win * win);       // 1-(d/r)^4
        win *= win;
        if (win <= 0.0f) continue;

        float  dist = sqrt(distSqr);
        float3 L    = toL / max(dist, 1e-4f);
        float  dc   = max(dist, Lp.ColorSourceRadius.w);
        // Falloff com bias do raio do froxel (UE): remove o pico 1/d^2 quando a luz
        // cai dentro do proprio voxel.
        float atten = win / (dc * dc + biasSqr);

        if (Lp.DirCosOuter.w > -1.5f) {
            float sm = saturate((dot(-L, Lp.DirCosOuter.xyz) - Lp.DirCosOuter.w)
                                * Lp.SpotParams.x);
            atten *= sm * sm;
        }
        if (atten <= 0.0f) continue;

        // Sombra local (mesmo caminho linear de refZ do deferred, sem normal-offset).
        if (Lp.SpotParams.y >= 0.0f) {
            float farP = max(1.0f / Lp.PosInvRadius.w, LightParamsVF2.x * 2.0f);
            if (Lp.DirCosOuter.w > -1.5f) {
                float4 sp = mul(float4(wp, 1.0f), Lp.ShadowMatrix);
                if (sp.w > 0.0f) {
                    float3 uvz = sp.xyz / sp.w;
                    if (all(uvz.xy > 0.0f) && all(uvz.xy < 1.0f) &&
                        uvz.z > 0.0f && uvz.z < 1.0f) {
                        float viewZ = dot(wp - Lp.PosInvRadius.xyz, Lp.DirCosOuter.xyz);
                        float refZ  = VolFog_LocalNdcDepth(viewZ - VolFog_LocalBias(viewZ), farP);
                        atten *= LocalShadowMap.SampleCmpLevelZero(
                                     ShadowCmp, float3(uvz.xy, Lp.SpotParams.y), refZ);
                    }
                }
            } else {
                float3 l2p   = wp - Lp.PosInvRadius.xyz;
                float3 ad    = abs(l2p);
                float  viewZ = max(ad.x, max(ad.y, ad.z));
                float  refZ  = VolFog_LocalNdcDepth(viewZ - VolFog_LocalBias(viewZ) * 1.5f, farP);
                atten *= LocalCubeShadow.SampleCmpLevelZero(
                             ShadowCmp, float4(normalize(l2p), Lp.SpotParams.y), refZ);
            }
        }
        if (atten <= 0.0f) continue;

        float phL = VolFog_PhaseHG(SunDirPhase.w, dot(L, -dir));
        lighting += Lp.ColorSourceRadius.rgb * (atten * phL * LightParamsVF.w);
    }

    // Ambiente: DDGI amostrado "olhando" pra camera (direcao dominante do inscatter
    // forward) / pi — mesma escala do difuso em superficie. Fora do DDGI, SkyAmbient.
    float3 amb;
    if (DDGIGridCount.w > 0.5f) {
        float2 invSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
        amb = SampleDDGIIrradiance(DDGIIrradianceAtlas, LinearClamp, wp, -dir,
                  DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                  (int)DDGIParams.y, invSize) * (DDGIParams.x / SMILE_PI);
    } else {
        amb = AmbientFallback.rgb;
    }
    return lighting + amb * AlbedoAmb.w;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)kVolFogW || id.y >= (uint)kVolFogH || id.z >= (uint)kVolFogZ)
        return;

    // Reprojecao: CENTRO do voxel (sem jitter) pro clip do frame anterior; slice da
    // historia pela MESMA distribuicao log (params mudaram -> CPU zera o peso).
    float viewZc;
    float3 wpCenter = VolFog_WorldPos(id, float3(0.5f, 0.5f, 0.5f), viewZc);

    float histAlpha = TemporalParams.x;
    float3 histUV   = float3(-1.0f, -1.0f, -1.0f);
    if (histAlpha > 0.0f) {
        float4 prevH = mul(float4(wpCenter, 1.0f), PrevViewProj);
        if (prevH.w > 1e-4f) {
            float2 prevNdc = prevH.xy / prevH.w;
            float  prevZ   = dot(wpCenter - PrevCamPos.xyz, PrevCamForward.xyz);
            histUV = float3(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f,
                            VolFog_SliceFromDepth(max(prevZ, 1e-3f)) / GridZParams.w);
        }
        if (any(histUV < 0.0f) || any(histUV > 1.0f)) histAlpha = 0.0f;
    }

    // Historia invalida = supersampling com os jitters dos ultimos frames (UE:
    // HistoryMissSupersampleCount) — mata o ruido em pan/corte de camera.
    int numSamples = (histAlpha < 0.001f) ? max((int)TemporalParams.y, 1) : 1;

    // Raio do froxel em mundo (diagonal ate o vizinho +1,+1,+1) — bias do falloff
    // das luzes puntuais (UE calcula identico por CellRadius).
    float unusedZ;
    float cellRadius = length(VolFog_WorldPos(id + uint3(1, 1, 1),
                                              float3(0.5f, 0.5f, 0.5f), unusedZ) - wpCenter);

    float3 lighting = float3(0.0f, 0.0f, 0.0f);
    [loop] for (int s = 0; s < numSamples; ++s) {
        float viewZ;
        float3 wp = VolFog_WorldPos(id, FrameJitterOffsets[s].xyz, viewZ);
        lighting += VolFog_Lighting(wp, cellRadius);
    }
    lighting /= (float)numSamples;

    float4 matScatExt = VBufferA[id];
    float4 result = float4(lighting * matScatExt.rgb, matScatExt.w);

    if (histAlpha > 0.0f) {
        float4 hist = ScatteringHistory.SampleLevel(LinearClamp, histUV, 0.0f);
        result = lerp(result, hist, histAlpha);
    }

    LightScattering[id] = result;
}
