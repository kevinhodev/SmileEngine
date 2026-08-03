#ifndef SMILE_ATMOSPHERE_COMMON_HLSLI
#define SMILE_ATMOSPHERE_COMMON_HLSLI

// A matematica vive no AtmosphereMath.hlsli (sem cbuffer). Este header e a camada que injeta o
// AtmosphereCB nela: todas as funcoes abaixo sao wrappers de uma linha. Quem NAO pode declarar
// o b0 da atmosfera (os shaders de trace do GI/reflexoes) inclui so o math e passa os
// parametros do planeta a mao — mesma implementacao, sem copia.
#include "AtmosphereMath.hlsli"

static const float PI = kAtmoPI;

cbuffer AtmosphereCB : register(b0) {
    float4 RayleighScattering; 
    float4 MieScattering;      
    float4 MieExtinction;      
    float4 OzoneAbsorption;   
    float4 OzoneTent;          
    float4 GroundAlbedo;      
    float4 PlanetRadii;       
    float4 SunDir;             
    float4 AtmoSteps;          
    float4 LutSize;            
    float4 SkyViewSize;        
    float4 SunDisk;            
    row_major float4x4 InvViewProjNoTrans; 

    row_major float4x4 InvViewProj; 
    float4 CameraWorldPos;          
    float4 AerialParams;            

    float4 MoonDir;
    float4 MoonParams;
    float4 StarAxis;
    float4 NightSky;

    row_major float4x4 ViewProjNoTrans;
    row_major float4x4 StarMatrix;
    float4 StarView;
};

#define kKmPerWorldUnit (CameraWorldPos.w)
#define kAerialDepthKm  (AerialParams.x)
#define kAerialSlices   (AerialParams.y)
#define kAerialStartKm  (AerialParams.z)
#define kAerialSamples  (AerialParams.w)

#define kRayleighScaleH (RayleighScattering.w)
#define kMieScaleH      (MieScattering.w)
#define kMiePhaseG      (MieExtinction.w)
#define kOzoneCenter    (OzoneTent.x)
#define kOzoneHalfW     (OzoneTent.y)
#define kBottomR        (PlanetRadii.x)
#define kTopR           (PlanetRadii.y)
#define kViewHeight     (SkyViewSize.z)
#define kSunDiskCos     (SunDisk.x)
#define kSunDiskInt     (SunDisk.y)
#define kSunIlluminance (SunDisk.z)
#define kSunGlareInt    (SunDisk.w)
#define kMoonSkyIll     (NightSky.x)
#define kMoonCorona     (NightSky.y)
#define kStarCatalogOn   (NightSky.z)
#define kOutputW         (StarView.x)
#define kOutputH         (StarView.y)
#define kRenderToOutputX (StarView.z)
#define kRenderToOutputY (StarView.w)

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);

float RayleighPhase(float cosTheta)        { return AtmoRayleighPhase(cosTheta); }
float MiePhaseHG(float g, float cosTheta)  { return AtmoMiePhaseHG(g, cosTheta); }
float UniformPhase()                       { return AtmoUniformPhase(); }

void SampleMedium(float altitudeKm, out float3 rayleighScatter,
                  out float3 mieScatter, out float3 extinction) {
    float densR = exp(-altitudeKm / kRayleighScaleH);
    float densM = exp(-altitudeKm / kMieScaleH);
    float densO = saturate(1.0f - abs(altitudeKm - kOzoneCenter) / kOzoneHalfW);

    rayleighScatter = RayleighScattering.rgb * densR;
    mieScatter      = MieScattering.rgb      * densM;

    float3 rayleighExt = rayleighScatter;                 
    float3 mieExt      = MieExtinction.rgb   * densM;
    float3 ozoneExt    = OzoneAbsorption.rgb * densO;     
    extinction = rayleighExt + mieExt + ozoneExt;
}

float RaySphereNearest(float3 ro, float3 rd, float radius) {
    return AtmoRaySphereNearest(ro, rd, radius);
}

float RaySphereFar(float3 ro, float3 rd, float radius) {
    return AtmoRaySphereFar(ro, rd, radius);
}

void UvToTransmittanceParams(out float viewHeight, out float viewZenithCos, float2 uv) {
    AtmoUvToTransmittanceParams(viewHeight, viewZenithCos, uv, kBottomR, kTopR);
}

float2 TransmittanceParamsToUv(float viewHeight, float viewZenithCos) {
    return AtmoTransmittanceParamsToUv(viewHeight, viewZenithCos, kBottomR, kTopR);
}

float3 SampleTransmittanceToTop(Texture2D<float4> tlut, float viewHeight, float viewZenithCos) {
    float2 uv = TransmittanceParamsToUv(viewHeight, viewZenithCos);
    return tlut.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;
}

float3 SampleMultiScatterLUT(Texture2D<float4> mslut, float viewHeight, float sunZenithCos) {
    float u = saturate(sunZenithCos * 0.5f + 0.5f);
    float v = saturate((viewHeight - kBottomR) / max(kTopR - kBottomR, 1e-4f));
    return mslut.SampleLevel(LinearClampSampler, float2(u, v), 0.0f).rgb;
}

void UvToSkyViewParams(out float viewZenithCos, out float lightViewCos,
                       float viewHeight, float2 uv) {
    AtmoUvToSkyViewParams(viewZenithCos, lightViewCos, uv, viewHeight, kBottomR);
}

float2 SkyViewParamsToUv(float viewZenithCos, float lightViewCos, float viewHeight) {
    return AtmoSkyViewParamsToUv(viewZenithCos, lightViewCos, viewHeight, kBottomR);
}

#endif 
