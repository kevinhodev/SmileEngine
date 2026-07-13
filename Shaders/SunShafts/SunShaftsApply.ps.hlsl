#include "SunShaftsCommon.hlsli"

// Composicao aditiva dos shafts no HDR (blend ONE/ONE no PSO), tingida pela key light
// e escalada pela intensidade. Roda em full-res lendo o resultado meia-res (bilinear).

Texture2D<float4> Shafts : register(t0);

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    float2 uv = svpos.xy * ScreenParams.zw;
    float3 s  = Shafts.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    return float4(s * ShaftTint.rgb * ShaftParams.w, 1.0f);
}
