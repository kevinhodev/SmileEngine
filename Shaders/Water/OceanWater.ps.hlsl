#include "WaterCommon.hlsli"

struct PSInput {
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float  crest    : TEXCOORD3;
};

float3 AnalyticSkyReflection(float3 R) {
    float horizon = saturate(R.y * 0.5f + 0.5f);
    float sunFacing = pow(saturate(dot(normalize(R), normalize(SunDirection.xyz))), 256.0f);
    float3 horizonColor = ShallowColor.rgb * 0.9f + SunColor.rgb * 0.04f;
    float3 zenithColor = float3(0.12f, 0.27f, 0.50f) * (0.35f + 0.65f * saturate(SunDirection.y));
    return lerp(horizonColor, zenithColor, horizon) + SunColor.rgb * SunDirection.w * sunFacing * 0.35f;
}

float4 main(PSInput input) : SV_TARGET {
    float3 N = ComputeOceanNormal(input.worldPos.xz);
    float3 V = normalize(CameraPosition.xyz - input.worldPos);
    float distToCamera = length(input.worldPos.xz - CameraPosition.xz);
    float normalFade = 1.0f - smoothstep(WaterParams1.x * 0.25f, WaterParams1.x * 0.55f, distToCamera);
    N = normalize(lerp(float3(0.0f, 1.0f, 0.0f), N, lerp(0.35f, 1.0f, normalFade)));

    float3 L = normalize(SunDirection.xyz);
    float3 H = normalize(L + V);

    float NoV = saturate(dot(N, V));
    float NoL = saturate(dot(N, L));
    float NoH = saturate(dot(N, H));

    float fresnel = SchlickFresnel(NoV);

    float waveHeight = input.worldPos.y - WaterParams0.x;
    float shallowMix = saturate(0.45f + waveHeight * WaterParams2.y + N.y * 0.25f);
    float3 refractedWater = lerp(DeepColor.rgb, ShallowColor.rgb, shallowMix);

    float3 R = reflect(-V, N);
    float3 analyticReflection = AnalyticSkyReflection(R);
    float3 iblReflection = analyticReflection;
    if (IBLParams.w > 0.5f) {
        float3 rotR = RotateY(R, IBLParams.y);
        float reflectionMip = min(IBLParams.z, lerp(4.5f, 2.0f, saturate(fresnel * 1.5f)));
        iblReflection = PrefilteredMap.SampleLevel(WaterClampSampler, rotR, reflectionMip).rgb * IBLParams.x;
    }
    float3 reflection = lerp(analyticReflection, iblReflection, IBLParams.w);

    float sunSpec = pow(NoH, 192.0f) * NoL;
    float3 specular = SunColor.rgb * SunDirection.w * sunSpec * 0.45f;

    float subsurface = pow(saturate(dot(-L, V)), 2.0f) * (1.0f - NoV);
    float3 sss = ShallowColor.rgb * SunColor.rgb * subsurface * 0.18f;

    float reflectWeight = saturate(fresnel * WaterParams1.z);
    float3 color = lerp(refractedWater + sss, reflection + specular, reflectWeight);

    float foamMask = saturate((input.crest - 0.78f) * 6.0f) * WaterParams2.x;
    float foamLight = 0.35f + 0.65f * NoL;
    float3 foamColor = lerp(float3(0.75f, 0.88f, 0.92f), SunColor.rgb, 0.15f) * foamLight;
    color = lerp(color, color + foamColor, foamMask * 0.45f);

    return float4(color, 1.0f);
}
