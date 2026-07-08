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

    float sunZenithCos = clamp(SunDir.y, -1.0f, 1.0f);
    float sunZenithSin = sqrt(saturate(1.0f - sunZenithCos * sunZenithCos));
    float3 sunDir = float3(sunZenithSin, sunZenithCos, 0.0f);

    // Lua = 2a luz atmosferica (estilo Light0/Light1 da UE), trazida pro frame do LUT
    // (X = azimute do sol) preservando o azimute relativo sol->lua.
    float3 moonDir;
    {
        float  cosZm = clamp(MoonDir.y, -1.0f, 1.0f);
        float  sinZm = sqrt(saturate(1.0f - cosZm * cosZm));
        float2 sunH  = SunDir.xz  / max(length(SunDir.xz),  1e-5f);
        float2 moonH = MoonDir.xz / max(length(MoonDir.xz), 1e-5f);
        float  cosRel = dot(sunH, moonH);
        float  sinRel = sunH.x * moonH.y - sunH.y * moonH.x;
        moonDir = float3(sinZm * cosRel, cosZm, sinZm * sinRel);
    }

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

            float3 inScatter = kSunIlluminance *
                               (earthShadow * sunTrans * phaseScatter + multiScatter);

            if (kMoonSkyIll > 0.0f) {
                // Fase UNIFORME de proposito: o LUT e dobrado no azimute do sol, um lobo
                // direcional da lua apareceria espelhado (brilho fantasma). O lobo Mie da lua
                // vive na corona per-pixel do sky pass, posicionada corretamente.
                float  moonZen    = dot(up, moonDir);
                float3 moonTrans  = SampleTransmittanceToTop(TransmittanceLUT, height, moonZen);
                float  moonShadow = (RaySphereNearest(p, moonDir, kBottomR) > 0.0f) ? 0.0f : 1.0f;
                float3 psiMsMoon  = SampleMultiScatterLUT(MultiScatterLUT, height, moonZen);
                float3 inScatterM = moonShadow * moonTrans * (rS + mS) * UniformPhase()
                                  + (rS + mS) * psiMsMoon;
                inScatter += kMoonSkyIll * inScatterM;
            }

            float3 sInt = (inScatter - inScatter * sampleT) / safeExt;
            L += transmittance * sInt;

            transmittance *= sampleT;
        }
    }

    OutSkyView[id.xy] = float4(L, 1.0f);
}
