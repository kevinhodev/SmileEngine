#include "VolumetricFogCommon.hlsli"
#include "../Shadow/CSMCommon.hlsli"
#include "../GI/DDGICommon.hlsli"

// LightScattering (UE): luz espalhada em direcao a camera por froxel.
// Sol (tap unico no CSM, receita dos sun shafts — penumbra nao e legivel em volume)
// com fase HG + ambiente via DDGI (fallback: SkyAmbient da atmosfera).
// F2: jitter Halton por frame + reprojecao temporal (historia ping-pong, blend 0.9)
// + supersampling quando a historia falha (fora do frustum/primeiro frame) — receita
// exata do LightScatteringCS da UE. F3 adiciona as luzes puntuais.

Texture3D<float4> VBufferA            : register(t0);
Texture2D<float4> DDGIIrradianceAtlas : register(t1);
Texture3D<float4> ScatteringHistory   : register(t2);
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

// Luz espalhada em direcao a camera num ponto do ar (posicao ja jitterada).
float3 VolFog_Lighting(float3 wp) {
    float3 dir = normalize(wp - CameraWorldPos.xyz); // camera -> ponto

    // Sol: radiancia da key light x visibilidade CSM x fase HG (lobo contra a luz).
    float vis = VolFog_SunVis(wp);
    float ph  = VolFog_PhaseHG(SunDirPhase.w, dot(SunDirPhase.xyz, -dir));
    float3 lighting = SunColorInt.rgb * (vis * ph);

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

    float3 lighting = float3(0.0f, 0.0f, 0.0f);
    [loop] for (int s = 0; s < numSamples; ++s) {
        float viewZ;
        float3 wp = VolFog_WorldPos(id, FrameJitterOffsets[s].xyz, viewZ);
        lighting += VolFog_Lighting(wp);
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
