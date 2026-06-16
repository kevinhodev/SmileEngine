#ifndef SMILE_FOG_APPLY_HLSLI
#define SMILE_FOG_APPLY_HLSLI

#include "FogCommon.hlsli"

float4 FogApplyMain(float2 pixelXY) {
    int2  px       = int2(pixelXY);
    float depthNdc = FogSampleDepth(px);
    
    float2 uv  = pixelXY * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, depthNdc, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 c2r  = worldPos - CameraWorldPos.xyz;
    float  dist = length(c2r);

    if (dist >= DepthParams.y * 0.97f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float4 hf = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.z > 0.5f) hf = GetExponentialHeightFog(c2r);

    float4 ap = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.y > 0.5f) {
        float tDepthKm = dist * CameraWorldPos.w; 
        ap = SampleAerialPerspective(uv, tDepthKm, AerialParams.x);
    }

    float  T         = ap.a * hf.a;
    float3 inscatter = ap.rgb * hf.a + hf.rgb;
    return float4(inscatter, T);
}

#endif 
