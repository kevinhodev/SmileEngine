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
        // Integrated texel z is the result at the END boundary z+1. Convert
        // that boundary coordinate to the normalized center of texel z.
        float uvz   = saturate((slice - 0.5f) / VolFogParams.w);
        vol = VolumetricFogTex.SampleLevel(LinearClampSampler, float3(uv, uvz), 0.0f);
    }

    // Ceu: sem height fog/aerial daqui (o sky shader ja tem a atmosfera), mas o que
    // esta FISICAMENTE entre a camera e o horizonte existe — volume froxel E sun shafts.
    // O RT de shafts ja contem a integral completa camera->fim da marcha, incluindo
    // a propria transmitancia. Portanto ele entra direto; multiplicar novamente por
    // vol.a aplicaria duas vezes a extincao do mesmo meio. Antes o early-return era antes do
    // += SampleVolumetricShafts, o que (a) cortava o feixe exatamente na silhueta —
    // e feixe contra o ceu e a foto classica de god ray —, (b) pagava o raymarch do
    // ceu e jogava fora (SunShaftsVolumetric marcha o alcance todo no ceu de proposito)
    // e (c) deixava o skyCenter do upsample bilateral como codigo morto.
    // Ceu e definido pelo depth clear, nao pela distancia reconstruida. A heuristica antiga
    // (dist >= 97% do far plane) classificava o ultimo anel do oceano como ceu: ele deixava de
    // receber aerial perspective justamente no horizonte e virava uma faixa especular clara,
    // especialmente visivel a noite. Geometria valida deve continuar pelo ramo de fog ate o far.
    if (SmileIsSky(depthNdc)) {
        float3 skyInscatter = vol.rgb;
        if (AerialParams.w > 0.5f)
            skyInscatter += SampleVolumetricShafts(uv, dist, true);
        return float4(skyInscatter, vol.a);
    }

    float4 hf = float4(0.0f, 0.0f, 0.0f, 1.0f);
    // Exclui o analitico no trecho coberto pelo volume (0..alcance/cosA), UE-style.
    float excludeDist = volOn ? (VolFogParams2.x / cosA) : 0.0f;
    if (AerialParams.z > 0.5f) hf = GetExponentialHeightFog(c2r, excludeDist);

    // Sun shafts volumetricos: integral solar de alta frequencia no proprio meio.
    // Mantemos separado porque ja inclui a transmitancia camera->amostra; ele substitui
    // a parcela solar do froxel no mesmo intervalo, em vez de ficar atras do volume.
    float3 shaftInscatter = float3(0.0f, 0.0f, 0.0f);
    if (AerialParams.w > 0.5f)
        shaftInscatter = SampleVolumetricShafts(uv, dist, false);

    float4 ap = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.y > 0.5f) {
        float tDepthKm = dist * CameraWorldPos.w;
        ap = SampleAerialPerspective(uv, tDepthKm, AerialParams.x);
    }

    // Composicao de longe pra perto: aerial atras do height fog, ambos atras do
    // volume froxel. Shafts ja sao a integral do meio proximo e entram uma unica vez.
    float  T         = ap.a * hf.a * vol.a;
    float3 inscatter = (ap.rgb * hf.a + hf.rgb) * vol.a + vol.rgb + shaftInscatter;
    return float4(inscatter, T);
}

#endif 
