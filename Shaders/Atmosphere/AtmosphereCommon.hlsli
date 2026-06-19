#ifndef SMILE_ATMOSPHERE_COMMON_HLSLI
#define SMILE_ATMOSPHERE_COMMON_HLSLI

static const float PI = 3.14159265358979323846f;

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

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);

float RayleighPhase(float cosTheta) {
    return (3.0f / (16.0f * PI)) * (1.0f + cosTheta * cosTheta);
}

float MiePhaseHG(float g, float cosTheta) {
    float g2    = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * PI * pow(max(denom, 1e-4f), 1.5f));
}

float UniformPhase() { return 1.0f / (4.0f * PI); }

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
    float b    = dot(ro, rd);
    float c    = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float s  = sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}

float RaySphereFar(float3 ro, float3 rd, float radius) {
    float b    = dot(ro, rd);
    float c    = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + sqrt(disc);
}

void UvToTransmittanceParams(out float viewHeight, out float viewZenithCos, float2 uv) {
    float xMu = uv.x;
    float xR  = uv.y;
    float H    = sqrt(max(0.0f, kTopR * kTopR - kBottomR * kBottomR));
    float rho  = H * xR;
    viewHeight = sqrt(max(0.0f, rho * rho + kBottomR * kBottomR));

    float dMin = kTopR - viewHeight;
    float dMax = rho + H;
    float d    = dMin + xMu * (dMax - dMin);
    viewZenithCos = (d == 0.0f) ? 1.0f
                                : (H * H - rho * rho - d * d) / (2.0f * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0f, 1.0f);
}

float2 TransmittanceParamsToUv(float viewHeight, float viewZenithCos) {
    float H   = sqrt(max(0.0f, kTopR * kTopR - kBottomR * kBottomR));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - kBottomR * kBottomR));

    float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0f)
                       + kTopR * kTopR;
    float d = max(0.0f, -viewHeight * viewZenithCos + sqrt(max(0.0f, discriminant)));

    float dMin = kTopR - viewHeight;
    float dMax = rho + H;
    float xMu  = (d - dMin) / max(dMax - dMin, 1e-5f);
    float xR   = rho / max(H, 1e-5f);
    return float2(xMu, xR);
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
    float vHorizon = sqrt(max(0.0f, viewHeight * viewHeight - kBottomR * kBottomR));
    float cosBeta  = vHorizon / max(viewHeight, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = PI - beta;

    float viewZenithAngle;
    if (uv.y < 0.5f) {
        float coord = 2.0f * uv.y;
        coord = 1.0f - coord;
        coord = 1.0f - coord * coord;
        viewZenithAngle = zenithHorizonAngle * coord;
    } else {
        float coord = uv.y * 2.0f - 1.0f;
        coord = coord * coord;
        viewZenithAngle = zenithHorizonAngle + beta * coord;
    }
    viewZenithCos = cos(viewZenithAngle);

    float coord = uv.x;
    coord = coord * coord;
    lightViewCos = -(coord * 2.0f - 1.0f);
}

float2 SkyViewParamsToUv(float viewZenithCos, float lightViewCos, float viewHeight) {
    float vHorizon = sqrt(max(0.0f, viewHeight * viewHeight - kBottomR * kBottomR));
    float cosBeta  = vHorizon / max(viewHeight, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = PI - beta;

    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0f, 1.0f));
    float u, v;
    if (viewZenithAngle < zenithHorizonAngle) {
        float coord = viewZenithAngle / max(zenithHorizonAngle, 1e-4f);
        coord = 1.0f - coord;
        coord = 1.0f - sqrt(max(0.0f, coord));
        v = coord * 0.5f;
    } else {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-4f);
        coord = sqrt(max(0.0f, coord));
        v = coord * 0.5f + 0.5f;
    }
    {
        float coord = -lightViewCos * 0.5f + 0.5f;
        u = sqrt(max(0.0f, coord));
    }
    return float2(u, v);
}

#endif 
