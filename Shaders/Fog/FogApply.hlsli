#ifndef SMILE_FOG_APPLY_HLSLI
#define SMILE_FOG_APPLY_HLSLI

#include "FogCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// Upsample bilateral do RT meia-res dos sun shafts (mesma receita do CloudComposite):
// pesos bilineares x concordância céu/geometria x similaridade de distância com o pixel
// full-res. O raymarch clampa a marcha no depth — texel de folha (5m) e texel de chão
// (50m) carregam inscatter bem diferente; bilinear puro vazava meia-res na silhueta.
float3 SampleVolumetricShafts(float2 uv, float distCenter, bool skyCenter) {
    // floor reproduz a divisão inteira do CPU (RTWidth = W/2) mesmo com res ímpar
    float2 rt   = floor(ScreenParams.xy * 0.5f);
    float2 st   = uv * rt - 0.5f;
    float2 base = floor(st);
    float2 f    = st - base;

    float wBil[4] = {
        (1.0f - f.x) * (1.0f - f.y),
        f.x          * (1.0f - f.y),
        (1.0f - f.x) * f.y,
        f.x          * f.y
    };
    const int2 offs[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    float3 sum = 0.0f, plain = 0.0f;
    float  wSum = 0.0f;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        int2   tx = clamp(int2(base) + offs[k], int2(0, 0), int2(rt) - int2(1, 1));
        float3 s  = VolumetricShafts.Load(int3(tx, 0)).rgb;
        plain += s * wBil[k];

        // depth do pixel full-res que o raymarch amostrou p/ este texel meia-res
        float2 fullUv = (float2(tx) + 0.5f) / rt;
        int2   fullPx = min(int2(fullUv * ScreenParams.xy), int2(ScreenParams.xy) - int2(1, 1));
        float  dTex   = FogSampleDepth(fullPx);
        bool   skyTex = SmileIsSky(dTex);

        float w = wBil[k] * ((skyTex == skyCenter) ? 1.0f : 0.0f);
        if (!skyCenter && !skyTex && w > 0.0f) {
            float4 tH = mul(float4((fullUv.x * 2.0f - 1.0f), (1.0f - fullUv.y * 2.0f), dTex, 1.0f),
                            InvViewProj);
            float distTex = length(tH.xyz / tH.w - CameraWorldPos.xyz);
            w *= exp(-abs(distTex - distCenter) / max(0.25f * distCenter, 1.0f));
        }
        sum  += s * w;
        wSum += w;
    }
    return (wSum > 1e-4f) ? sum / wSum : plain;
}

float4 FogApplyMain(float2 pixelXY) {
    int2  px       = int2(pixelXY);
    float depthNdc = FogSampleDepth(px);

    float2 uv  = pixelXY * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, depthNdc, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 c2r  = worldPos - CameraWorldPos.xyz;
    float  dist = length(c2r);

    // Froxel volumetric fog: amostra o volume integrado em (uv, slice(viewZ)).
    // cosA converte distancia -> view-depth (o grid e fatiado em view-Z).
    const bool  volOn = VolFogParams2.y > 0.5f;
    float4 vol  = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float  cosA = 1.0f;
    if (volOn) {
        float3 dirV = c2r / max(dist, 1e-4f);
        cosA = max(dot(dirV, CamForwardVF.xyz), 0.05f);
        float viewZ = dist * cosA;
        float slice = log2(viewZ * VolFogParams.x + VolFogParams.y) * VolFogParams.z;
        float uvz   = saturate(slice / VolFogParams.w);
        vol = VolumetricFogTex.SampleLevel(LinearClampSampler, float3(uv, uvz), 0.0f);
    }

    // Ceu: sem height fog/aerial daqui (o sky shader ja tem a atmosfera), mas o
    // volume froxel na frente do horizonte existe fisicamente — aplica so ele.
    if (dist >= DepthParams.y * 0.97f) return float4(vol.rgb, vol.a);

    float4 hf = float4(0.0f, 0.0f, 0.0f, 1.0f);
    // Exclui o analitico no trecho coberto pelo volume (0..alcance/cosA), UE-style.
    float excludeDist = volOn ? (VolFogParams2.x / cosA) : 0.0f;
    if (AerialParams.z > 0.5f) hf = GetExponentialHeightFog(c2r, excludeDist);

    // Sun shafts volumétricos: somam no inscatter do height fog. Mesmo tratamento do
    // dirInscatter analítico: não é atenuado pelo aerial (consistência com o legado).
    if (AerialParams.w > 0.5f)
        hf.rgb += SampleVolumetricShafts(uv, dist, SmileIsSky(depthNdc));

    float4 ap = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.y > 0.5f) {
        float tDepthKm = dist * CameraWorldPos.w;
        ap = SampleAerialPerspective(uv, tDepthKm, AerialParams.x);
    }

    // Composicao de longe pra perto: aerial atras do height fog, ambos atras do
    // volume froxel (meio mais proximo da camera atenua o que vem de tras).
    float  T         = ap.a * hf.a * vol.a;
    float3 inscatter = (ap.rgb * hf.a + hf.rgb) * vol.a + vol.rgb;
    return float4(inscatter, T);
}

#endif 
