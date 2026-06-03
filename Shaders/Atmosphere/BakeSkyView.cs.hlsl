// BakeSkyView.cs.hlsl
// Hillaire sky-view LUT: a 192x104 lat-long of the sky dome around the camera.
// For each (viewZenithCos, lightViewCos) texel, reconstruct the view ray in a
// local frame (up = +Y, sun azimuth = 0) and ray-march single scattering (with
// Rayleigh + Mie phase and transmittance-to-sun) plus the multi-scattering LUT
// contribution. Baked every frame (cheap) since it depends on sun + camera height.

#include "AtmosphereCommon.hlsli"

Texture2D<float4>   TransmittanceLUT : register(t0);
Texture2D<float4>   MultiScatterLUT  : register(t1);
RWTexture2D<float4> OutSkyView       : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)SkyViewSize.x;
    uint H = (uint)SkyViewSize.y;
    if (id.x >= W || id.y >= H) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(W, H);
    float  viewHeight = kViewHeight;

    float viewZenithCos, lightViewCos;
    UvToSkyViewParams(viewZenithCos, lightViewCos, viewHeight, uv);

    // Local frame: world up = +Y, sun placed at azimuth 0 in the X-Y plane.
    float sunZenithCos = clamp(SunDir.y, -1.0f, 1.0f);
    float sunZenithSin = sqrt(saturate(1.0f - sunZenithCos * sunZenithCos));
    float3 sunDir = float3(sunZenithSin, sunZenithCos, 0.0f);

    float viewZenithSin = sqrt(saturate(1.0f - viewZenithCos * viewZenithCos));
    float lightViewSin  = sqrt(saturate(1.0f - lightViewCos * lightViewCos));
    float3 viewDir = float3(viewZenithSin * lightViewCos,
                            viewZenithCos,
                            viewZenithSin * lightViewSin);

    float3 worldPos = float3(0.0f, viewHeight, 0.0f);

    float tGround = RaySphereNearest(worldPos, viewDir, kBottomR);
    float tAtmo   = RaySphereFar(worldPos, viewDir, kTopR);
    float tMax    = (tGround > 0.0f) ? tGround : tAtmo;

    float3 L = float3(0.0f, 0.0f, 0.0f);
    if (tMax > 0.0f) {
        int   steps = max(8, (int)AtmoSteps.z);
        float dt    = tMax / (float)steps;

        float cosSunView = dot(viewDir, sunDir);
        float rPhase = RayleighPhase(cosSunView);
        float mPhase = MiePhaseHG(kMiePhaseG, cosSunView);

        float3 transmittance = float3(1.0f, 1.0f, 1.0f);
        [loop]
        for (int i = 0; i < steps; ++i) {
            float  t = (i + 0.5f) * dt;
            float3 p = worldPos + t * viewDir;
            float  height = length(p);
            float  alt    = height - kBottomR;
            float3 up     = p / max(height, 1e-4f);

            float3 rS, mS, ext;
            SampleMedium(alt, rS, mS, ext);
            float3 safeExt = max(ext, 1e-6f);
            float3 sampleT = exp(-ext * dt);

            float  sunZen   = dot(up, sunDir);
            float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, height, sunZen);
            float  earthShadow = (RaySphereNearest(p, sunDir, kBottomR) > 0.0f) ? 0.0f : 1.0f;

            float3 phaseScatter = rS * rPhase + mS * mPhase;
            float3 psiMs        = SampleMultiScatterLUT(MultiScatterLUT, height, sunZen);
            float3 multiScatter = (rS + mS) * psiMs;

            float3 inScatter = earthShadow * sunTrans * phaseScatter + multiScatter;
            float3 sInt = (inScatter - inScatter * sampleT) / safeExt;
            L += transmittance * sInt;

            transmittance *= sampleT;
        }
        L *= kSunIlluminance;
    }

    OutSkyView[id.xy] = float4(L, 1.0f);
}
