// Atmosphere sky pass: samples the sky-view LUT by world view direction and adds
// an analytic sun disk attenuated by the transmittance LUT at the camera. Reinhard
// + gamma to match the scene tonemap chain.

#include "AtmosphereCommon.hlsli"

Texture2D<float4> SkyViewLUT       : register(t0);
Texture2D<float4> TransmittanceLUT : register(t1);

struct PSInput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    // World view direction (translation removed → sky at infinity).
    float4 worldFar = mul(float4(input.clipXY, 1.0f, 1.0f), InvViewProjNoTrans);
    float3 viewDir  = normalize(worldFar.xyz);

    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 sunDir   = normalize(SunDir.xyz);

    float viewZenithCos = dot(viewDir, up);

    float3 viewHoriz = viewDir - up * viewZenithCos;
    float3 sunHoriz  = sunDir  - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));

    float2 uv = SkyViewParamsToUv(viewZenithCos, lightViewCos, kViewHeight);
    float3 L  = SkyViewLUT.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;

    // Sun disk + glare. The physical disk is tiny (~0.5°); games read "bigger"
    // mostly from bloom/glare, so we add a soft wide halo (glare) around the
    // bright core. Both are attenuated by the atmospheric transmittance toward
    // the sun (so the sun reddens near the horizon).
    float  cosToSun = dot(viewDir, sunDir);
    float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, dot(up, sunDir));

    // Horizon occlusion check to prevent sun rendering under/through the ground.
    //float  cosHorizon = -sqrt(max(0.0f, kViewHeight * kViewHeight - kBottomR * kBottomR)) / max(kViewHeight, 1e-4f);
    //float  sunVisible = saturate((dot(up, sunDir) - cosHorizon) * 100.0f) * saturate((viewZenithCos - cosHorizon) * 100.0f);

    // Bright near-physical core disk.
    float core = smoothstep(kSunDiskCos, lerp(kSunDiskCos, 1.0f, 0.5f), cosToSun);
    // Wide soft glare: pow falloff that fades over a few degrees around the sun.
    float glare = pow(saturate(cosToSun), 350.0f);

    L += sunTrans * (kSunDiskInt * core + kSunGlareInt * glare);

    return float4(L, 1.0f);
}
