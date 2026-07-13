#include "SunShaftBloomCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * ScreenParams.zw;
    float3 color = max(SceneColor.SampleLevel(LinearClamp, uv, 0.0f).rgb, 0.0f);
    float depth = SceneDepth.SampleLevel(PointClamp, uv, 0.0f);

    float distanceMask = 1.0f;
    if (!SmileIsSky(depth)) {
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 worldH = mul(float4(ndc, depth, 1.0f), InvViewProj);
        float3 worldPosition = worldH.xyz / max(abs(worldH.w), 1e-6f);
        float distanceToCamera = length(worldPosition - CameraWorldPos.xyz);
        float normalizedDistance = distanceToCamera * TintDepth.w;
        distanceMask = smoothstep(0.35f, 0.95f, normalizedDistance);
    }

    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    float luminance = max(dot(color, lumaWeights), 1e-5f);
    float threshold = BloomParams.x;
    float knee = BloomParams.y;
    float soft = clamp(luminance - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / max(4.0f * knee, 1e-5f);
    float contribution = min(max(soft, luminance - threshold), BloomParams.z);
    float3 bright = color * (contribution / luminance);

    float2 sunVector = (SunPosParams.xy - uv) * float2(1.0f, SunPosParams.w);
    float sourceMask = 1.0f - smoothstep(0.0f, BlurParams.y, length(sunVector));
    sourceMask *= sourceMask;

    float edgeDistance = min(min(uv.x, 1.0f - uv.x), min(uv.y, 1.0f - uv.y));
    float edgeFade = smoothstep(0.0f, 0.06f, edgeDistance);
    float3 result = bright * distanceMask * sourceMask * edgeFade * SunPosParams.z;
    return float4(result, 1.0f);
}
