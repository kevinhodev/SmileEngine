#include "SunShaftBloomCommon.hlsli"

Texture2D<float4> SourceTexture : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * ScreenParams.zw;
    float2 radialVector = (SunPosParams.xy - uv) * BlurParams.x;
    float3 accumulated = 0.0f;
    float accumulatedWeight = 0.0f;

    [unroll] for (uint sampleIndex = 0; sampleIndex < SUN_SHAFT_BLOOM_SAMPLES; ++sampleIndex) {
        float samplePosition = (float(sampleIndex) + 0.5f +
                                (BlurParams.z - 0.5f) * 0.75f) /
                               float(SUN_SHAFT_BLOOM_SAMPLES);
        float2 sampleUV = uv + radialVector * samplePosition;
        float inBounds = all(sampleUV >= 0.0f) && all(sampleUV <= 1.0f) ? 1.0f : 0.0f;
        float weight = lerp(1.0f, 0.35f, samplePosition) * inBounds;
        accumulated += SourceTexture.SampleLevel(LinearClamp, saturate(sampleUV), 0.0f).rgb * weight;
        accumulatedWeight += weight;
    }

    return float4(accumulated / max(accumulatedWeight, 1e-5f), 1.0f);
}
