#ifndef SMILE_FOG_COMMON_HLSLI
#define SMILE_FOG_COMMON_HLSLI

#include "../Common/DepthConfig.hlsli"

cbuffer FogCB : register(b0) {
    float4 ExponentialFogParameters;     
    float4 ExponentialFogParameters2;    
    float4 ExponentialFogParameters3;   
    float4 FogInscatteringColor;        
    float4 DirectionalInscatteringColor; 
    float4 InscatteringLightDirection;   
    row_major float4x4 InvViewProj;      
    float4 CameraWorldPos;          
    float4 AerialParams;               
    float4 ScreenParams;      
    float4 DepthParams;        
    float4 SunShaftParams;
};

Texture3D<float4> AerialVolume     : register(t1);
Texture3D<float4> SunShaftVolume   : register(t2);
SamplerState      LinearClampSampler : register(s0);

float FogLinearizeDepth(float d) {
    float n = DepthParams.x;
    float f = DepthParams.y;
#if SMILE_REVERSE_Z
    return n * f / (d * (f - n) + n);
#else
    return n * f / (f - d * (f - n));
#endif
}

float3 SampleSunShafts(float2 uv, float viewDepth) {
    float z = sqrt(saturate(viewDepth / max(SunShaftParams.x, 1e-4f)));
    return SunShaftVolume.SampleLevel(
        LinearClampSampler, float3(uv, z), 0.0f).rgb;
}

float CalculateLineIntegralShared(float falloff, float rayDirZ, float rayOriginTerms) {
    float Falloff = max(-127.0f, falloff * rayDirZ);
    float LineIntegral       = (1.0f - exp2(-Falloff)) / Falloff;
    float LineIntegralTaylor = 0.6931472f - 0.5f * (0.6931472f * 0.6931472f) * Falloff;
    return rayOriginTerms * (abs(Falloff) > 0.01f ? LineIntegral : LineIntegralTaylor);
}

float4 GetExponentialHeightFog(float3 worldPosRelCam) {
    const float MinFogOpacity = FogInscatteringColor.w; 

    float3 c2r = worldPosRelCam;
    float  len = max(length(c2r), 1e-4f);
    float3 dir = c2r / len;

    float rayLength       = len;
    float rayDirZ         = c2r.y;                      
    float rayOriginTerms  = ExponentialFogParameters.x; 
    float rayOriginTerms2 = ExponentialFogParameters2.x;

    float startDist = ExponentialFogParameters.w;
    if (startDist > 0.0f && len > startDist) {
        float excludeT = startDist / len;
        float startY   = CameraWorldPos.y + excludeT * c2r.y;
        rayLength      = (1.0f - excludeT) * len;
        rayDirZ        = c2r.y - excludeT * c2r.y;
        float exp1     = max(-127.0f, ExponentialFogParameters.y  * (startY - ExponentialFogParameters3.y));
        float exp2t    = max(-127.0f, ExponentialFogParameters2.y * (startY - ExponentialFogParameters2.w));
        rayOriginTerms  = ExponentialFogParameters3.x * exp2(-exp1);  
        rayOriginTerms2 = ExponentialFogParameters2.z * exp2(-exp2t); 
    }

    float shared1 = CalculateLineIntegralShared(ExponentialFogParameters.y,  rayDirZ, rayOriginTerms);
    float shared2 = CalculateLineIntegralShared(ExponentialFogParameters2.y, rayDirZ, rayOriginTerms2);
    float sharedIntegral = shared1 + shared2;

    float lineIntegral = sharedIntegral * rayLength;
    float expFogFactor = max(saturate(exp2(-lineIntegral)), MinFogOpacity);
    
    float3 dirInscatter = float3(0.0f, 0.0f, 0.0f);
    if (InscatteringLightDirection.w >= 0.0f) {
        float  d = saturate(dot(dir, InscatteringLightDirection.xyz));
        float3 dirColor = DirectionalInscatteringColor.rgb * pow(d, DirectionalInscatteringColor.w);
        float  dirStart = InscatteringLightDirection.w;
        float  dirIntegral = sharedIntegral * max(rayLength - dirStart, 0.0f);
        float  dirFogFactor = saturate(exp2(-dirIntegral));
        dirInscatter = dirColor * (1.0f - dirFogFactor);
    }

    float cutoff = ExponentialFogParameters3.w;
    if (cutoff > 0.0f && len > cutoff) {
        expFogFactor = 1.0f;
        dirInscatter = float3(0.0f, 0.0f, 0.0f);
    }

    float3 fogColor = FogInscatteringColor.rgb * (1.0f - expFogFactor) + dirInscatter;
    return float4(fogColor, expFogFactor);
}

float4 SampleAerialPerspective(float2 screenUV, float tDepthKm, float apDepthKm) {
    float linW    = saturate(tDepthKm / max(apDepthKm, 1e-4f));
    float nonLinW = sqrt(linW);

    const float Slices = 16.0f;
    const float HalfSliceDepth = 0.70710678f; 
    float nonLinSlice = nonLinW * Slices;
    float weight = 1.0f;
    if (nonLinSlice < HalfSliceDepth)
        weight = saturate(nonLinSlice * nonLinSlice * 2.0f);

    float4 ap = AerialVolume.SampleLevel(LinearClampSampler, float3(screenUV, nonLinW), 0.0f);
    ap.rgb *= weight;
    ap.a    = 1.0f - weight * (1.0f - ap.a);
    return ap; 
}

#endif 