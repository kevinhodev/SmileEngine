// FogCommon.hlsli
// Deferred atmospheric fog: UE5-style exponential height fog (two layers +
// directional inscattering + start/cutoff distance) combined with the
// aerial-perspective froxel sampled from the atmosphere. All math composites in
// LINEAR HDR (the scene color buffer is linear; the final tonemap runs after).
//
// Height-fog math is a faithful port of HeightFogCommon.ush
// (CalculateLineIntegralShared / GetExponentialHeightFog). Up axis is +Y.

#ifndef SMILE_FOG_COMMON_HLSLI
#define SMILE_FOG_COMMON_HLSLI

// Matches Smile::FogConstants (Fog.h) field-for-field.
cbuffer FogCB : register(b0) {
    float4 ExponentialFogParameters;     // x=collapsed density1, y=falloff1, z=maxObserverHeight, w=startDistance
    float4 ExponentialFogParameters2;    // x=collapsed density2, y=falloff2, z=density2, w=fogHeight2
    float4 ExponentialFogParameters3;    // x=density1, y=fogHeight1, z=unused, w=cutoffDistance
    float4 FogInscatteringColor;         // rgb=base color, w=1-maxOpacity (transmittance floor)
    float4 DirectionalInscatteringColor; // rgb=color, w=exponent
    float4 InscatteringLightDirection;   // xyz=dir TO sun, w=start distance (>=0 enables, <0 disables)
    row_major float4x4 InvViewProj;      // FULL inverse view-proj (with translation)
    float4 CameraWorldPos;               // xyz=camera world pos (scene units), w=km per world unit
    float4 AerialParams;                 // x=AP volume depth (km), y=useAP, z=useHeightFog, w unused
    float4 ScreenParams;                 // x=w, y=h, z=1/w, w=1/h
    float4 DepthParams;                  // x=near, y=far, z unused, w unused
};

Texture3D<float4> AerialVolume     : register(t1);
SamplerState      LinearClampSampler : register(s0);

// --- Exponential height fog (HeightFogCommon.ush port) -----------------------
// Analytical line integral of exp2(-k*z) along the view ray, numerically stable
// near k=0 via a Taylor expansion. ln(2) = 0.6931471805599453.
float CalculateLineIntegralShared(float falloff, float rayDirZ, float rayOriginTerms) {
    float Falloff = max(-127.0f, falloff * rayDirZ);
    float LineIntegral       = (1.0f - exp2(-Falloff)) / Falloff;
    float LineIntegralTaylor = 0.6931472f - 0.5f * (0.6931472f * 0.6931472f) * Falloff;
    return rayOriginTerms * (abs(Falloff) > 0.01f ? LineIntegral : LineIntegralTaylor);
}

// Returns (premultiplied inscattering color, transmittance) for the ray from the
// camera to a world point (relative to the camera). Up axis = +Y.
float4 GetExponentialHeightFog(float3 worldPosRelCam) {
    const float MinFogOpacity = FogInscatteringColor.w; // 1 - FogMaxOpacity

    float3 c2r = worldPosRelCam;
    float  len = max(length(c2r), 1e-4f);
    float3 dir = c2r / len;

    float rayLength       = len;
    float rayDirZ         = c2r.y;                      // full Z (Y-up) extent
    float rayOriginTerms  = ExponentialFogParameters.x; // collapsed density1 @ camera height
    float rayOriginTerms2 = ExponentialFogParameters2.x;

    // Start distance: exclude [0, startDistance] of the ray.
    float startDist = ExponentialFogParameters.w;
    if (startDist > 0.0f && len > startDist) {
        float excludeT = startDist / len;
        float startY   = CameraWorldPos.y + excludeT * c2r.y;
        rayLength      = (1.0f - excludeT) * len;
        rayDirZ        = c2r.y - excludeT * c2r.y;
        float exp1     = max(-127.0f, ExponentialFogParameters.y  * (startY - ExponentialFogParameters3.y));
        float exp2t    = max(-127.0f, ExponentialFogParameters2.y * (startY - ExponentialFogParameters2.w));
        rayOriginTerms  = ExponentialFogParameters3.x * exp2(-exp1);  // density1 * ...
        rayOriginTerms2 = ExponentialFogParameters2.z * exp2(-exp2t); // density2 * ...
    }

    float shared1 = CalculateLineIntegralShared(ExponentialFogParameters.y,  rayDirZ, rayOriginTerms);
    float shared2 = CalculateLineIntegralShared(ExponentialFogParameters2.y, rayDirZ, rayOriginTerms2);
    float sharedIntegral = shared1 + shared2;

    float lineIntegral = sharedIntegral * rayLength;
    float expFogFactor = max(saturate(exp2(-lineIntegral)), MinFogOpacity); // transmittance

    // Directional inscattering: a cosine lobe toward the sun, fogged from a start distance.
    float3 dirInscatter = float3(0.0f, 0.0f, 0.0f);
    if (InscatteringLightDirection.w >= 0.0f) {
        float  d = saturate(dot(dir, InscatteringLightDirection.xyz));
        float3 dirColor = DirectionalInscatteringColor.rgb * pow(d, DirectionalInscatteringColor.w);
        float  dirStart = InscatteringLightDirection.w;
        float  dirIntegral = sharedIntegral * max(rayLength - dirStart, 0.0f);
        float  dirFogFactor = saturate(exp2(-dirIntegral));
        dirInscatter = dirColor * (1.0f - dirFogFactor);
    }

    // Cutoff distance: beyond it, render no fog (keeps far backgrounds clear).
    float cutoff = ExponentialFogParameters3.w;
    if (cutoff > 0.0f && len > cutoff) {
        expFogFactor = 1.0f;
        dirInscatter = float3(0.0f, 0.0f, 0.0f);
    }

    float3 fogColor = FogInscatteringColor.rgb * (1.0f - expFogFactor) + dirInscatter;
    return float4(fogColor, expFogFactor);
}

// --- Aerial-perspective froxel sampling (SkyAtmosphereCommon.ush port) -------
// tDepthKm = distance camera->point in km. Volume depth slices use a SQUARED
// distribution at bake time, so we invert with sqrt here. A short near-fade at
// the first half-slice avoids a hard edge right at the camera.
float4 SampleAerialPerspective(float2 screenUV, float tDepthKm, float apDepthKm) {
    float linW    = saturate(tDepthKm / max(apDepthKm, 1e-4f));
    float nonLinW = sqrt(linW);

    const float Slices = 16.0f;
    const float HalfSliceDepth = 0.70710678f; // sqrt(0.5)
    float nonLinSlice = nonLinW * Slices;
    float weight = 1.0f;
    if (nonLinSlice < HalfSliceDepth)
        weight = saturate(nonLinSlice * nonLinSlice * 2.0f);

    float4 ap = AerialVolume.SampleLevel(LinearClampSampler, float3(screenUV, nonLinW), 0.0f);
    ap.rgb *= weight;
    ap.a    = 1.0f - weight * (1.0f - ap.a);
    return ap; // (premultiplied inscatter, transmittance)
}

#endif // SMILE_FOG_COMMON_HLSLI
