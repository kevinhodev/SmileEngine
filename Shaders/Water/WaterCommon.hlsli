#ifndef SMILE_WATER_COMMON_HLSLI
#define SMILE_WATER_COMMON_HLSLI

#include "../Common/DepthConfig.hlsli"

cbuffer WaterCB : register(b0) {
    row_major float4x4 ViewProj;
    row_major float4x4 InvViewProj;
    float4 CameraPos;        
    float4 SunDirection;     
    float4 SunColor;         
    float4 OceanParams0;    
    float4 OceanParams1;     
    float4 DeepColorDensity; 
    float4 Misc;            
    float4 WaterFXParams;    
    float4 ShadeParams;      
    float4 OceanFFT;         
    float4 OceanFade;    
    float4 BumpParams;     
    float4 BumpParams2;    
    float4 ScreenParams;   
    float4 DepthParams;     
    float4 RefractionParams; 
    float4 DebugParams;    
    float4 InScatterColor;   
    float4 AbsorptionColor; 
    float4 FoamParams;
    float4 FoamColor;
    float4 QuadTreeParams;
    row_major float4x4 ViewProjNoJitter; // velocity: projecao atual SEM jitter
    row_major float4x4 PrevViewProj;     // velocity: projecao anterior SEM jitter
    float4 CascadeParams; // x,y,z = 1/tileWorld das cascatas 0..2, w = nº de cascatas
    float4 WaterAmbient;  // rgb = ambiente físico do céu (ilumina o in-scatter), w = livre
};

Texture2D<float4> FFTDisplacement  : register(t1); // cascata 0 (detalhe)
Texture2D<float4> FFTDisplacement1 : register(t6); // cascata 1
Texture2D<float4> FFTDisplacement2 : register(t7); // cascata 2 (swell)
Texture2D<float4> FFTPreviousDisplacement0 : register(t12);
Texture2D<float4> FFTPreviousDisplacement1 : register(t13);
Texture2D<float4> FFTPreviousDisplacement2 : register(t14);
SamplerState      LinearWrap       : register(s0);

float2 WaterFFTSampleUV(float2 worldXZ) {
    return worldXZ * CascadeParams.x; // cascata 0
}

float2 WaterCascadeUV(uint c, float2 worldXZ) {
    return worldXZ * CascadeParams[c];
}

float4 WaterSampleFFTUv(float2 baseUV) {
    return FFTDisplacement.SampleLevel(LinearWrap, baseUV, 0.0);
}

float4 WaterSampleFFTCascadeUv(uint c, float2 baseUV) {
    if (c == 1) return FFTDisplacement1.SampleLevel(LinearWrap, baseUV, 0.0);
    if (c == 2) return FFTDisplacement2.SampleLevel(LinearWrap, baseUV, 0.0);
    return FFTDisplacement.SampleLevel(LinearWrap, baseUV, 0.0);
}

float4 WaterSampleFFTCascadeUvLod(uint c, float2 baseUV, float lod) {
    if (c == 1) return FFTDisplacement1.SampleLevel(LinearWrap, baseUV, lod);
    if (c == 2) return FFTDisplacement2.SampleLevel(LinearWrap, baseUV, lod);
    return FFTDisplacement.SampleLevel(LinearWrap, baseUV, lod);
}

float4 WaterSampleFFTCascadeUvGrad(uint c, float2 baseUV, float2 dx, float2 dy) {
    if (c == 1) return FFTDisplacement1.SampleGrad(LinearWrap, baseUV, dx, dy);
    if (c == 2) return FFTDisplacement2.SampleGrad(LinearWrap, baseUV, dx, dy);
    return FFTDisplacement.SampleGrad(LinearWrap, baseUV, dx, dy);
}

float4 WaterSamplePreviousFFTCascadeUv(uint c, float2 baseUV) {
    if (c == 1) return FFTPreviousDisplacement1.SampleLevel(LinearWrap, baseUV, 0.0);
    if (c == 2) return FFTPreviousDisplacement2.SampleLevel(LinearWrap, baseUV, 0.0);
    return FFTPreviousDisplacement0.SampleLevel(LinearWrap, baseUV, 0.0);
}

float4 WaterSamplePreviousFFTCascadeUvLod(uint c, float2 baseUV, float lod) {
    if (c == 1) return FFTPreviousDisplacement1.SampleLevel(LinearWrap, baseUV, lod);
    if (c == 2) return FFTPreviousDisplacement2.SampleLevel(LinearWrap, baseUV, lod);
    return FFTPreviousDisplacement0.SampleLevel(LinearWrap, baseUV, lod);
}

float4 WaterSampleFFTCascade(uint c, float2 worldXZ) {
    return WaterSampleFFTCascadeUv(c, WaterCascadeUV(c, worldXZ));
}

float4 WaterSampleFFT(float2 worldXZ) {
    return WaterSampleFFTUv(WaterFFTSampleUV(worldXZ));
}

float3 WaterFFTNormalFromDisplacement(float2 worldXZ, float worldStep, float normalUp) {
    float2 e = float2(max(worldStep, 0.05), 0.0);
    float hL = WaterSampleFFT(worldXZ - e.xy).z;
    float hR = WaterSampleFFT(worldXZ + e.xy).z;
    float hD = WaterSampleFFT(worldXZ - e.yx).z;
    float hU = WaterSampleFFT(worldXZ + e.yx).z;
    return normalize(float3(hL - hR, max(normalUp * 0.65, 1.0), hD - hU));
}

Texture2D<float4> WaterNormalTex  : register(t4); // cascata 0 (Toksvig mips)
Texture2D<float4> WaterNormalTex1 : register(t8);
Texture2D<float4> WaterNormalTex2 : register(t9);
SamplerState      AnisoWrap      : register(s3); // s2 e o sampler de comparacao do CSM

Texture2D<float4> SceneColor : register(t2);
Texture2D<float>  SceneDepth : register(t3);

float LinearizeDepth(float ndcZ) {
    float n = DepthParams.x, f = DepthParams.y;
#if SMILE_REVERSE_Z
    return n * f / (ndcZ * (f - n) + n); 
#else
    return n * f / (f - ndcZ * (f - n));
#endif
}

float SampleSceneDepthLin(float2 uv) {
    int2 px = clamp(int2(uv * ScreenParams.xy), int2(0, 0), int2(ScreenParams.xy) - 1);
    return LinearizeDepth(SceneDepth.Load(int3(px, 0)));
}

struct VSOutput {
    float4 pos        : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 vView      : TEXCOORD2; 
    float4 screenProj : TEXCOORD3; 
    float4 baseTC     : TEXCOORD4; 
    float4 debugData  : TEXCOORD5;
    float2 tileUV     : TEXCOORD6;
    float4 curClip    : TEXCOORD7; // clip atual sem jitter (velocity)
    float4 prevClip   : TEXCOORD8; // clip anterior sem jitter (velocity)
    float2 parametricXZ : TEXCOORD9; // pre-displacement material coordinate
};

float FresnelSchlick(float F0, float cosTheta) {
    float f = pow(saturate(1.0 - cosTheta), 5.0);
    return saturate(F0 + (1.0 - F0) * f);
}

float3 AnalyticSky(float3 dir) {
    float up = saturate(dir.y * 0.5 + 0.5);
    float3 horizon = float3(0.52, 0.62, 0.72);
    float3 zenith  = float3(0.09, 0.24, 0.52);
    float3 sky = lerp(horizon, zenith, up);
    float sun = saturate(dot(normalize(dir), normalize(SunDirection.xyz)));
    sky += SunColor.rgb * pow(sun, 64.0) * 0.5;
    return sky;
}

float4 SampleWaterNormalGrad(float2 uv, float2 dx, float2 dy) {
    float4 n = WaterNormalTex.SampleGrad(AnisoWrap, uv, dx, dy);
    n.xyz = normalize(n.xyz);
    n.w = saturate(n.w);
    return n;
}

float4 SampleWaterNormalGradCascade(uint c, float2 uv, float2 dx, float2 dy) {
    float4 n;
    if      (c == 1) n = WaterNormalTex1.SampleGrad(AnisoWrap, uv, dx, dy);
    else if (c == 2) n = WaterNormalTex2.SampleGrad(AnisoWrap, uv, dx, dy);
    else             n = WaterNormalTex.SampleGrad(AnisoWrap, uv, dx, dy);
    n.xyz = normalize(n.xyz);
    n.w = saturate(n.w);
    return n;
}

float SmoothWaterStep(float x) {
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}

float WaterDistanceFade(float dist, float start, float range) {
    return SmoothWaterStep(saturate(1.0 - (dist - start) / max(range, 1.0)));
}

float WaterNormalFade(float dist) {
    float start = max(OceanFade.x * 2.5, 1000.0);
    float range = max(OceanFade.y * 3.0, 4500.0);
    return WaterDistanceFade(dist, start, range);
}

float WaterHash21(float2 p) {
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float WaterValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = WaterHash21(i);
    float b = WaterHash21(i + float2(1.0, 0.0));
    float c = WaterHash21(i + float2(0.0, 1.0));
    float d = WaterHash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float2 BumpParallaxOffset(float4 baseTC, float3 V, float heightScale) {
    if (heightScale <= 0.0) return float2(0.0, 0.0);
    float2 dir = V.xz / max(V.y, 0.2);
    float h1 = WaterSampleFFTUv(baseTC.xy * 0.25).b * heightScale;
    float2 p = dir * h1;
    float h2 = WaterSampleFFTUv(baseTC.zw + p).b * heightScale;
    p += dir * h2;
    return p;
}

float SunSpecularLobes(float3 N, float3 V, float3 L, float shininess) {
    const float NoL = saturate(dot(N, L));
    const float NoV = saturate(dot(N, V));
    if (NoL <= 0.0f || NoV <= 0.0f) return 0.0f;

    const float3 H = normalize(V + L);
    const float NoH = saturate(dot(N, H));
    const float VoH = saturate(dot(V, H));
    const float F = FresnelSchlick(0.02f, VoH);
    // Normalized Blinn-Phong BRDF. The previous two unnormalized lobes reached
    // unit white before Fresnel and produced the large clipped blob in closeups.
    const float normalization = (shininess + 2.0f) * (1.0f / (8.0f * 3.14159265f));
    return normalization * pow(NoH, shininess) * F * NoL;
}

#endif 
