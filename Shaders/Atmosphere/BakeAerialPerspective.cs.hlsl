#include "AtmosphereCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

Texture2D<float4>   TransmittanceLUT : register(t0);
Texture2D<float4>   MultiScatterLUT  : register(t1);
RWTexture3D<float4> OutAerial        : register(u0);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 VolWH = float2(32.0f, 32.0f);
    const float  Slices = max(kAerialSlices, 1.0f);
    if (id.x >= 32u || id.y >= 32u || (float)id.z >= Slices) return;

    float2 uv  = (float2(id.xy) + 0.5f) / VolWH;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farH = mul(float4(ndc, SMILE_NDC_FAR, 1.0f), InvViewProj); 
    float3 farW = farH.xyz / farH.w;
    float3 worldDir = normalize(farW - CameraWorldPos.xyz);
    
    float sliceN = ((float)id.z + 0.5f) / Slices;
    sliceN *= sliceN;                               
    float kmPerWU = max(kKmPerWorldUnit, 1e-9f);
    float startKm = kAerialStartKm;
    float tKm     = startKm + sliceN * (kAerialDepthKm - startKm); 

    float3 camKm = float3(CameraWorldPos.x * kmPerWU,
                          kBottomR + CameraWorldPos.y * kmPerWU,
                          CameraWorldPos.z * kmPerWU);

    float3 sunDir = normalize(SunDir.xyz);

    int   steps = max(1, (int)(((float)id.z + 1.0f) * max(kAerialSamples, 1.0f)));
    float dt    = tKm / (float)steps;

    float cosSunView = dot(worldDir, sunDir);
    float rPhase = RayleighPhase(cosSunView);
    float mPhase = MiePhaseHG(kMiePhaseG, cosSunView);

    float3 L            = float3(0.0f, 0.0f, 0.0f);
    float3 transmittance = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (int i = 0; i < steps; ++i) {
        float  t   = (i + 0.5f) * dt;
        float3 p   = camKm + t * worldDir;
        float  hgt = length(p);
        float  alt = hgt - kBottomR;
        float3 up  = p / max(hgt, 1e-4f);

        float3 rS, mS, ext;
        SampleMedium(alt, rS, mS, ext);
        float3 safeExt = max(ext, 1e-6f);
        float3 sampleT = exp(-ext * dt);

        float  sunZen   = dot(up, sunDir);
        float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, hgt, sunZen);
        float  earthShadow = (RaySphereNearest(p, sunDir, kBottomR) > 0.0f) ? 0.0f : 1.0f;

        float3 phaseScatter = rS * rPhase + mS * mPhase;
        float3 psiMs        = SampleMultiScatterLUT(MultiScatterLUT, hgt, sunZen);
        float3 multiScatter = (rS + mS) * psiMs;

        float3 inScatter = earthShadow * sunTrans * phaseScatter + multiScatter;
        float3 sInt      = (inScatter - inScatter * sampleT) / safeExt;
        L += transmittance * sInt;

        transmittance *= sampleT;
    }
    L *= kSunIlluminance;

    float avgT = dot(transmittance, float3(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f));
    OutAerial[id] = float4(L, avgT);
}
