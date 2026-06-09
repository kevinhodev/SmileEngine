// FogApply.hlsli
// Shared deferred-fog pixel-shader body. The including .ps.hlsl must, before
// including this file, declare the scene-depth texture and provide:
//     float FogSampleDepth(int2 px);   // raw NDC depth [0,1] at the pixel
// (Texture2D for non-MSAA, Texture2DMS sample-0 for MSAA.) The result is meant
// to be blended over the HDR scene color with One / SrcAlpha:
//     final = inscatter + scene * transmittance.

#ifndef SMILE_FOG_APPLY_HLSLI
#define SMILE_FOG_APPLY_HLSLI

#include "FogCommon.hlsli"

float4 FogApplyMain(float2 pixelXY) {
    int2  px       = int2(pixelXY);
    float depthNdc = FogSampleDepth(px); // [0,1], 1 = far plane

    // Reconstruct world position from depth (full inverse view-proj).
    float2 uv  = pixelXY * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, depthNdc, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 c2r  = worldPos - CameraWorldPos.xyz;
    float  dist = length(c2r);

    // Sky / far-plane pixels: the physical sky already contains the atmosphere, so
    // leave them untouched (transmittance = 1, no inscatter). A DISTANCE test is
    // used instead of a raw depth threshold because depth precision near the far
    // plane is far too poor to separate the distant ocean (depth ~0.9999997) from
    // the cleared sky depth (1.0). The sky reconstructs to ~far; opaque geometry
    // and the projected-grid ocean are always closer than far (maxDist < far).
    if (dist >= DepthParams.y * 0.97f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    // Height fog (near ground layer).
    float4 hf = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.z > 0.5f) hf = GetExponentialHeightFog(c2r);

    // Aerial perspective (atmospheric layer, the whole camera->point segment).
    float4 ap = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (AerialParams.y > 0.5f) {
        float tDepthKm = dist * CameraWorldPos.w; // world units -> km
        ap = SampleAerialPerspective(uv, tDepthKm, AerialParams.x);
    }

    // Layered composite over the scene: scene -> aerial perspective -> height fog.
    //   afterAP = scene*ap.a + ap.rgb
    //   afterHF = afterAP*hf.a + hf.rgb = scene*(ap.a*hf.a) + (ap.rgb*hf.a + hf.rgb)
    float  T         = ap.a * hf.a;
    float3 inscatter = ap.rgb * hf.a + hf.rgb;
    return float4(inscatter, T);
}

#endif // SMILE_FOG_APPLY_HLSLI
