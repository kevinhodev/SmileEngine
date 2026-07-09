#include "../Common/DepthConfig.hlsli"

Texture2D<float4> CloudRT    : register(t0);
Texture2D<float>  SceneDepth : register(t1);
SamplerState      LinearClamp : register(s0);

cbuffer CompositeCB : register(b0) {
    float4 OutParams; // xy = 1/resolucao de saida, zw = dimensoes do RT (half-res)
};

struct PSInput {
    float4 pos : SV_POSITION;
};

// Upsample bilateral do RT half-res: pesos bilineares × concordancia ceu/geometria com o
// pixel full-res. O raymarch clampa a marcha no depth da cena, entao texels "de ceu" e
// "de geometria" carregam nuvem diferente — misturar os dois na silhueta criava halo.
float4 main(PSInput input) : SV_TARGET {
    int2  px = int2(input.pos.xy);
    float2 uv = input.pos.xy * OutParams.xy;

    float dCenter = SceneDepth.Load(int3(px, 0));
    bool  skyC    = SmileIsSky(dCenter);

    float2 rt   = OutParams.zw;
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

    float2 outSize = 1.0f / OutParams.xy;

    float4 sum = 0.0f, plain = 0.0f;
    float  wSum = 0.0f;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        int2 tx = clamp(int2(base) + offs[k], int2(0, 0), int2(rt) - int2(1, 1));
        float4 s = CloudRT.Load(int3(tx, 0));
        plain += s * wBil[k];

        // depth do pixel full-res que o raymarch amostrou p/ este texel half-res
        float2 fullUv = (float2(tx) + 0.5f) / rt;
        int2   fullPx = min(int2(fullUv * outSize), int2(outSize) - int2(1, 1));
        float  dTex   = SceneDepth.Load(int3(fullPx, 0));

        float w = wBil[k] * ((SmileIsSky(dTex) == skyC) ? 1.0f : 0.0f);
        sum  += s * w;
        wSum += w;
    }

    float4 cloud = (wSum > 1e-4f) ? sum / wSum : plain;
    return float4(cloud.rgb, cloud.a);
}
