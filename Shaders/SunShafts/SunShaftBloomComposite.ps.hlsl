#include "SunShaftBloomCommon.hlsli"

Texture2D<float4> ShaftTexture : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * ScreenParams.zw;
    float3 shafts = ShaftTexture.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    float3 gentleTint = lerp(1.0f, TintDepth.rgb, 0.35f);
    return float4(shafts * gentleTint * BloomParams.w, 0.0f);
}
