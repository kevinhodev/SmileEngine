// CloudDensity.hlsli  (Phase B3)
// Schneider/Nubis cloud density model, shared by the raymarch and (later) the
// shadow pass. Split into a cheap base-shape sample (low-freq shape + weather
// coverage + height gradient) and a detail erosion step (high-freq Worley carve),
// so empty space and light marches can skip the expensive part.

#ifndef SMILE_CLOUD_DENSITY_HLSLI
#define SMILE_CLOUD_DENSITY_HLSLI

struct CloudCtx {
    float bottomR, innerR, outerR;
    float coverage, densityScale, noiseScale;
    float weatherScale, erosionStrength, detailScale, cloudTypeBias;
    float3 wind;
};

float CloudRemap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5f) * (nhi - nlo);
}

float CloudGradientRamp(float h, float a, float b, float c, float d) {
    return saturate(CloudRemap(h, a, b, 0.0f, 1.0f)) * saturate(CloudRemap(h, c, d, 1.0f, 0.0f));
}

// Height gradient blended across stratus → cumulus → cumulonimbus by cloud type.
float CloudHeightGradient(float h, float cloudType) {
    float stratus      = CloudGradientRamp(h, 0.0f, 0.08f, 0.20f, 0.30f);
    float cumulus      = CloudGradientRamp(h, 0.0f, 0.15f, 0.45f, 0.80f);
    float cumulonimbus = CloudGradientRamp(h, 0.0f, 0.08f, 0.80f, 1.00f);
    float g = lerp(stratus, cumulus, saturate(cloudType * 2.0f));
    g = lerp(g, cumulonimbus, saturate((cloudType - 0.5f) * 2.0f));
    return g;
}

// Cheap base density: low-frequency Perlin-Worley shape × height gradient,
// remapped by the weather coverage. No erosion, no detail volume. Also returns
// the normalized height in the layer (for the erosion step).
float CloudBaseDensity(float3 p, Texture3D<float4> baseNoise, Texture2D<float4> weather,
                       SamplerState samp, CloudCtx c, out float outHeightFrac) {
    float height = length(p);
    float heightFrac = saturate((height - c.innerR) / max(c.outerR - c.innerR, 1e-4f));
    outHeightFrac = heightFrac;
    if (heightFrac <= 0.0f || heightFrac >= 1.0f) return 0.0f;

    // Weather (horizontal): R = coverage, G = cloud type, B = wetness.
    float2 wuv = p.xz * c.weatherScale;
    float4 w   = weather.SampleLevel(samp, wuv, 0.0f);
    float coverage  = saturate(w.r * (0.5f + c.coverage));   // global coverage biases the map
    float cloudType = saturate(w.g + c.cloudTypeBias);

    float gradient = CloudHeightGradient(heightFrac, cloudType);
    if (gradient <= 0.0f) return 0.0f;

    // Base shape (R channel is the baked Perlin-Worley).
    float3 sp   = float3(p.x, height - c.bottomR, p.z) * c.noiseScale + c.wind;
    float4 base = baseNoise.SampleLevel(samp, sp, 0.0f);
    float shape = base.r * gradient;

    // Medium erosion from the base volume's Worley FBM channels (G,B,A).
    float worleyFBM = base.g * 0.625f + base.b * 0.25f + base.a * 0.125f;
    shape = saturate(CloudRemap(shape, worleyFBM * 0.4f, 1.0f, 0.0f, 1.0f));

    float density = saturate(CloudRemap(shape, 1.0f - coverage, 1.0f, 0.0f, 1.0f));
    return density;
}

// High-frequency erosion: carve wispy edges with the 32^3 detail Worley volume,
// stronger toward the cloud top. baseDensity comes from CloudBaseDensity.
float CloudErode(float baseDensity, float3 p, float heightFrac,
                 Texture3D<float4> detailNoise, SamplerState samp, CloudCtx c) {
    if (baseDensity <= 0.0f) return 0.0f;

    float height = length(p);
    float3 dp = float3(p.x, height - c.bottomR, p.z) * (c.noiseScale * c.detailScale) + c.wind * 2.0f;
    float3 dn = detailNoise.SampleLevel(samp, dp, 0.0f).rgb;
    float detailFBM = dn.r * 0.625f + dn.g * 0.25f + dn.b * 0.125f;

    // Carve more at the wispy top, less at the dense base.
    float carve = detailFBM * c.erosionStrength * lerp(1.0f, 0.4f, 1.0f - heightFrac);
    float density = saturate(CloudRemap(baseDensity, carve, 1.0f, 0.0f, 1.0f));
    return density * c.densityScale;
}

#endif // SMILE_CLOUD_DENSITY_HLSLI
