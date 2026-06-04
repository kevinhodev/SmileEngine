// CloudRaymarch.cs.hlsl  (Phase B3 — realistic density)
// Ray-marches the spherical cloud shell using the Schneider/Nubis density model
// (CloudDensity.hlsli): low-freq shape + weather coverage + height gradient, then
// high-freq detail erosion. Cheap base density drives empty-space skipping and the
// light march; the eroded density drives the primary integration.

cbuffer CloudCB : register(b0) {
    row_major float4x4 InvViewProjNoTrans;
    float4 CameraPos;    // xyz = camera (atmosphere km-frame), w = view height
    float4 SunDir;       // xyz = direction TO sun, w = sun intensity
    float4 SunColor;     // rgb, w unused
    float4 PlanetRadii;  // x = bottomR, y = cloudInnerR, z = cloudOuterR (km)
    float4 CloudParams;  // x = coverage, y = densityScale, z = noiseScale, w = time
    float4 CloudParams2; // x = weatherScale, y = erosionStrength, z = detailScale, w = cloudTypeBias
    float4 WindParams;   // xyz = wind velocity (km/s), w unused
    float4 MarchParams;  // x = primary steps, y = light steps, z = ambient, w = maxDistKm
    float4 ScreenParams; // x = rtW, y = rtH, z = 1/rtW, w = 1/rtH
    float4 PhaseParams;  // x = g1, y = g2, z = blend, w = powderStrength
    float4 AtmoLink;     // x = atmoTopR, y = msOctaves, z = ambientScale, w unused
};

Texture3D<float4>   BaseNoise        : register(t0);
Texture3D<float4>   DetailNoise      : register(t1);
Texture2D<float4>   WeatherMap       : register(t2);
Texture2D<float4>   TransmittanceLUT : register(t3); // atmosphere coupling
Texture2D<float4>   MultiScatterLUT  : register(t4);
RWTexture2D<float4> OutClouds        : register(u0);

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);

#include "CloudDensity.hlsli"
#include "CloudLighting.hlsli"

float RaySphereNearest(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}
float RaySphereFar(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + sqrt(disc);
}
float Hash(float2 p) {
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}

CloudCtx MakeCtx() {
    CloudCtx c;
    c.bottomR = PlanetRadii.x; c.innerR = PlanetRadii.y; c.outerR = PlanetRadii.z;
    c.coverage = CloudParams.x; c.densityScale = CloudParams.y; c.noiseScale = CloudParams.z;
    c.weatherScale = CloudParams2.x; c.erosionStrength = CloudParams2.y;
    c.detailScale = CloudParams2.z; c.cloudTypeBias = CloudParams2.w;
    c.wind = WindParams.xyz * CloudParams.w;
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)ScreenParams.x;
    uint H = (uint)ScreenParams.y;
    if (id.x >= W || id.y >= H) return;

    float2 uv  = (float2(id.xy) + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldFar = mul(float4(ndc, 1.0f, 1.0f), InvViewProjNoTrans);
    float3 dir = normalize(worldFar.xyz);
    float3 cam = CameraPos.xyz;

    if (RaySphereNearest(cam, dir, PlanetRadii.x) > 0.0f) {
        OutClouds[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float tStart = RaySphereFar(cam, dir, PlanetRadii.y);
    float tEnd   = RaySphereFar(cam, dir, PlanetRadii.z);
    if (tEnd <= tStart || tEnd <= 0.0f) {
        OutClouds[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float maxDist = MarchParams.w;
    if (maxDist > 0.0f) tEnd = min(tEnd, tStart + maxDist);

    int   numSteps   = max(16, (int)MarchParams.x);
    int   lightSteps = max(1,  (int)MarchParams.y);

    float segLen = tEnd - tStart;
    float dt     = segLen / (float)numSteps;
    // Reduced jitter amplitude: trades a little banding for much less per-pixel
    // grain (no temporal accumulation yet to resolve full jitter).
    float jitter = Hash(float2(id.xy) + CloudParams.w);
    float t      = tStart + jitter * dt * 0.5f;
    float shellThickness = PlanetRadii.z - PlanetRadii.y;

    CloudCtx ctx = MakeCtx();

    // Lighting params (B4 — atmosphere coupling + dual-lobe phase).
    float g1 = PhaseParams.x, g2 = PhaseParams.y, blend = PhaseParams.z, powderStrength = PhaseParams.w;
    float atmoTopR    = AtmoLink.x;
    int   msOctaves   = max(1, (int)AtmoLink.y);
    float ambientScale = AtmoLink.z;
    float cosTheta  = dot(dir, SunDir.xyz);
    float basePhase = DualLobeHG(cosTheta, g1, g2, blend);

    float3 L = float3(0.0f, 0.0f, 0.0f);
    float  transmittance = 1.0f;

    [loop]
    for (int i = 0; i < numSteps; ++i) {
        if (t > tEnd) break;
        float3 p = cam + t * dir;

        float hf;
        float baseD = CloudBaseDensity(p, BaseNoise, WeatherMap, LinearWrapSampler, ctx, hf);
        if (maxDist > 0.0f)
            baseD *= saturate(1.0f - (t - 0.5f * maxDist) / (0.5f * maxDist));

        // Empty-space skip: low-freq base is smooth, so doubling the step is safe.
        if (baseD <= 0.001f) { t += dt * 2.0f; continue; }

        float d = CloudErode(baseD, p, hf, DetailNoise, LinearWrapSampler, ctx);
        if (d > 0.001f) {
            // --- B5: light march toward the sun --------------------------------
            // Quadratic step distribution (fine near the sample, coarse far away
            // — UE) so near-cloud self-shadowing keeps detail with few steps.
            // Adaptive LOD (Cry): fewer shadow steps once the primary transmittance
            // is already low (the sample is dark, shadow precision matters less).
            int   lSteps = (transmittance < 0.3f) ? max(2, lightSteps / 2) : lightSteps;
            float lBase  = shellThickness / (float)(lSteps * lSteps); // series spans ~thickness
            float lightOD   = 0.0f;
            float lightDist = 0.0f;
            [loop]
            for (int j = 0; j < lSteps; ++j) {
                float stepLen = lBase * (2.0f * j + 1.0f); // cumulative dist = (j+1)^2 * lBase
                lightDist += stepLen;
                float3 lp = p + SunDir.xyz * lightDist;
                float  lhf;
                lightOD += CloudBaseDensity(lp, BaseNoise, WeatherMap, LinearWrapSampler, ctx, lhf)
                         * ctx.densityScale * stepLen;
            }
            // Atmosphere coupling: sun color reaching the cloud is the atmospheric
            // transmittance toward the sun at this altitude; ambient is the physical
            // sky fill from the multi-scatter LUT.
            float  height = length(p);
            float  sunZen = dot(p / max(height, 1e-4f), SunDir.xyz);
            float3 atmoT  = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                    height, sunZen, ctx.bottomR, atmoTopR);
            float3 sunRadiance = SunColor.rgb * SunDir.w * atmoT;
            float sunY = SunDir.y;
            float day = saturate(sunY * 4.0f + 0.2f);
            float lowSun = saturate(1.0f - sunY * 2.5f);
            float3 zenithCol = float3(0.18f, 0.30f, 0.55f); // Azul Rayleigh
            float3 horizonCol = float3(0.60f, 0.40f, 0.26f); // Sunset quente
            float3 ambientSky = (zenithCol + (horizonCol - zenithCol) * lowSun) * day * SunColor.rgb;
            float3 ambientGround = ambientSky * 0.35f;

            // 2. Mistura hemisférica baseada na fração de altura da nuvem (hf)
            float3 hemiAmbient = lerp(ambientGround, ambientSky, hf) * ambientScale;

            // 3. Combina o multi-scattering atmosférico corrigido com o domo celeste
            float3 skyAmbient = (SampleAtmoMultiScatter(MultiScatterLUT, LinearClampSampler,
                                            height, sunZen, ctx.bottomR, atmoTopR) * SunColor.rgb 
                     + hemiAmbient) * SunDir.w;

            // Wrenninge multi-scatter octaves of the sun in-scatter.
            float sAtten = 1.0f, eAtten = 1.0f, pAtten = 1.0f;
            float sunScatter = 0.0f;
            [loop]
            for (int n = 0; n < msOctaves; ++n) {
                float ph = lerp(0.0795f, basePhase, pAtten); // 1/(4pi) isotropic floor
                sunScatter += sAtten * ph * exp(-lightOD * eAtten);
                sAtten *= 0.5f; eAtten *= 0.5f; pAtten *= 0.5f;
            }
            float3 sunContribution = sunRadiance * sunScatter * PowderTerm(d, powderStrength);

            float3 inLum   = sunContribution + skyAmbient;
            float  ext     = d;
            float  sampleT = exp(-ext * dt);
            float3 S       = inLum * d;
            float3 Sint    = (S - S * sampleT) / max(ext, 1e-6f);
            L += transmittance * Sint;
            transmittance *= sampleT;

            if (transmittance < 0.01f) break;
        }
        t += dt;
    }

    // --- Aerial perspective (cheap, no froxel) ---------------------------------
    // Dim the cloud by the atmospheric transmittance along the view ray from the
    // camera to the cloud entry, and add the sky in-scatter as haze. Distant /
    // grazing clouds fade into the atmosphere instead of staying bright white.
    if (transmittance < 0.999f) {
        float3 entryPos    = cam + tStart * dir;
        float  camHeight   = length(cam);
        float  entryHeight = length(entryPos);
        float  muCam   = dot(cam / camHeight, dir);
        float  muEntry = dot(entryPos / entryHeight, dir);

        float3 Tcam   = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                camHeight, muCam, ctx.bottomR, atmoTopR);
        float3 Tentry = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                entryHeight, muEntry, ctx.bottomR, atmoTopR);
        float3 viewTrans = saturate(Tcam / max(Tentry, 1e-3f));

        float3 haze = SampleAtmoMultiScatter(MultiScatterLUT, LinearClampSampler, camHeight,
                                             dot(cam / camHeight, SunDir.xyz), ctx.bottomR, atmoTopR)
                    * SunDir.w * ambientScale;

        float cloudOpacity = 1.0f - transmittance;
        L = L * viewTrans + haze * (1.0f - viewTrans) * cloudOpacity;
    }

    OutClouds[id.xy] = float4(L, transmittance);
}
