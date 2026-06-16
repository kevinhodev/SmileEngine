#include "AtmosphereCommon.hlsli"

Texture2D<float4>   TransmittanceLUT : register(t0);
RWTexture2D<float4> OutMultiScatter  : register(u0);

#define SQRT_SAMPLES 8   

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)LutSize.z;
    uint H = (uint)LutSize.w;
    if (id.x >= W || id.y >= H) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(W, H);

    float cosSunZenith = uv.x * 2.0f - 1.0f;
    float viewHeight   = kBottomR + saturate(uv.y) * (kTopR - kBottomR);
    viewHeight         = clamp(viewHeight, kBottomR + 0.5f, kTopR - 0.5f);

    float3 worldPos = float3(0.0f, viewHeight, 0.0f);
    float  sinSun   = sqrt(saturate(1.0f - cosSunZenith * cosSunZenith));
    float3 sunDir   = float3(sinSun, cosSunZenith, 0.0f);

    int   msSteps = max(4, (int)AtmoSteps.y);
    float invPhase = UniformPhase();

    float3 lumTotal = float3(0.0f, 0.0f, 0.0f); 
    float3 fmsTotal = float3(0.0f, 0.0f, 0.0f); 

    [loop]
    for (int si = 0; si < SQRT_SAMPLES; ++si) {
        [loop]
        for (int sj = 0; sj < SQRT_SAMPLES; ++sj) {
            float cosTheta = 1.0f - 2.0f * (si + 0.5f) / (float)SQRT_SAMPLES;
            float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
            float phi      = 2.0f * PI * (sj + 0.5f) / (float)SQRT_SAMPLES;
            float3 rd = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));

            float tGround = RaySphereNearest(worldPos, rd, kBottomR);
            float tAtmo   = RaySphereFar(worldPos, rd, kTopR);
            float tMax    = (tGround > 0.0f) ? tGround : tAtmo;
            if (tMax <= 0.0f) continue;

            float  dt = tMax / (float)msSteps;
            float3 transmittance = float3(1.0f, 1.0f, 1.0f);
            float3 lum    = float3(0.0f, 0.0f, 0.0f);
            float3 lumFac = float3(0.0f, 0.0f, 0.0f);

            [loop]
            for (int step = 0; step < msSteps; ++step) {
                float  t = (step + 0.5f) * dt;
                float3 p = worldPos + t * rd;
                float  height   = length(p);
                float  altitude = height - kBottomR;
                float3 up = p / max(height, 1e-4f);

                float3 rS, mS, ext;
                SampleMedium(altitude, rS, mS, ext);
                float3 safeExt = max(ext, 1e-6f);
                float3 sampleT = exp(-ext * dt);

                float3 scatterNoPhase = rS + mS;
                float3 fInt = (scatterNoPhase - scatterNoPhase * sampleT) / safeExt;
                lumFac += transmittance * fInt;

                float  sunZenith = dot(up, sunDir);
                float3 sunT = SampleTransmittanceToTop(TransmittanceLUT, height, sunZenith);

                float  earthHit = RaySphereNearest(p, sunDir, kBottomR);
                if (earthHit > 0.0f) sunT = float3(0.0f, 0.0f, 0.0f);

                float3 inScatter = (rS + mS) * invPhase * sunT;
                float3 sInt = (inScatter - inScatter * sampleT) / safeExt;
                lum += transmittance * sInt;

                transmittance *= sampleT;
            }

            if (tGround > 0.0f) {
                float3 gp   = worldPos + tGround * rd;
                float3 gN   = normalize(gp);
                float  gNoL = saturate(dot(gN, sunDir));
                float3 sunT = SampleTransmittanceToTop(TransmittanceLUT, kBottomR, dot(gN, sunDir));
                lum += transmittance * GroundAlbedo.rgb * (gNoL / PI) * sunT;
            }

            lumTotal += lum;
            fmsTotal += lumFac;
        }
    }

    float invSamples = 1.0f / (float)(SQRT_SAMPLES * SQRT_SAMPLES);
    lumTotal *= invSamples;
    fmsTotal *= invSamples;

    float3 fms2 = fmsTotal * fmsTotal;
    float3 sumContribution = 1.0f + fmsTotal + fms2 + fmsTotal * fms2 + fms2 * fms2;
    float3 psiMs = lumTotal * sumContribution;
    OutMultiScatter[id.xy] = float4(psiMs, 1.0f);
}
