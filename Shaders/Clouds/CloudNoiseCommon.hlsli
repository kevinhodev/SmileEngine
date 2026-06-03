// CloudNoiseCommon.hlsli
// Tileable Perlin and Worley (cellular) noise for the Nubis/Schneider cloud
// noise volumes. All noise wraps over the given frequency so the baked 3D
// textures sample seamlessly with wrap addressing.

#ifndef SMILE_CLOUD_NOISE_COMMON_HLSLI
#define SMILE_CLOUD_NOISE_COMMON_HLSLI

float3 Hash33(float3 p) {
    p = float3(dot(p, float3(127.1f, 311.7f, 74.7f)),
               dot(p, float3(269.5f, 183.3f, 246.1f)),
               dot(p, float3(113.5f, 271.9f, 124.6f)));
    return frac(sin(p) * 43758.5453123f);
}

// Wrap a cell coordinate into [0, freq) for tiling.
float3 WrapCell(float3 cell, float freq) {
    return cell - freq * floor(cell / freq);
}

// Tileable Worley (cellular) noise. Returns ~[0,1], billowy (high near features).
float WorleyNoise(float3 uvw, float freq) {
    float3 p  = uvw * freq;
    float3 id = floor(p);
    float3 fr = frac(p);

    float minDist = 1.0f;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int z = -1; z <= 1; ++z) {
        float3 offset = float3(x, y, z);
        float3 cell   = WrapCell(id + offset, freq);
        float3 fp     = Hash33(cell);
        float3 diff   = offset + fp - fr;
        minDist = min(minDist, dot(diff, diff));
    }
    return 1.0f - saturate(sqrt(minDist));
}

// Tileable gradient (Perlin) noise. Returns ~[0,1].
float3 RandGrad(float3 cell, float freq) {
    float3 h = Hash33(WrapCell(cell, freq)) * 2.0f - 1.0f;
    return normalize(h + 1e-5f);
}

float PerlinNoise(float3 uvw, float freq) {
    float3 p  = uvw * freq;
    float3 id = floor(p);
    float3 f  = frac(p);
    float3 u  = f * f * (3.0f - 2.0f * f);

    float n000 = dot(RandGrad(id + float3(0,0,0), freq), f - float3(0,0,0));
    float n100 = dot(RandGrad(id + float3(1,0,0), freq), f - float3(1,0,0));
    float n010 = dot(RandGrad(id + float3(0,1,0), freq), f - float3(0,1,0));
    float n110 = dot(RandGrad(id + float3(1,1,0), freq), f - float3(1,1,0));
    float n001 = dot(RandGrad(id + float3(0,0,1), freq), f - float3(0,0,1));
    float n101 = dot(RandGrad(id + float3(1,0,1), freq), f - float3(1,0,1));
    float n011 = dot(RandGrad(id + float3(0,1,1), freq), f - float3(0,1,1));
    float n111 = dot(RandGrad(id + float3(1,1,1), freq), f - float3(1,1,1));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);
    float nxyz = lerp(nxy0, nxy1, u.z);
    return nxyz * 0.5f + 0.5f;
}

// FBM helpers (3 octaves).
float WorleyFBM(float3 uvw, float freq) {
    return WorleyNoise(uvw, freq)        * 0.625f
         + WorleyNoise(uvw, freq * 2.0f) * 0.25f
         + WorleyNoise(uvw, freq * 4.0f) * 0.125f;
}

float PerlinFBM(float3 uvw, float freq) {
    return PerlinNoise(uvw, freq)        * 0.625f
         + PerlinNoise(uvw, freq * 2.0f) * 0.25f
         + PerlinNoise(uvw, freq * 4.0f) * 0.125f;
}

float Remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5f) * (nhi - nlo);
}

// Schneider Perlin-Worley: dilate Perlin by the inverted Worley FBM.
float PerlinWorley(float3 uvw, float freq) {
    float perlin = PerlinFBM(uvw, freq);
    float worley = WorleyFBM(uvw, freq);
    return saturate(Remap(perlin, worley - 1.0f, 1.0f, 0.0f, 1.0f));
}

#endif // SMILE_CLOUD_NOISE_COMMON_HLSLI
