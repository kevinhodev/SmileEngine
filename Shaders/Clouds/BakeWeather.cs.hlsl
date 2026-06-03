// BakeWeather.cs.hlsl
// Procedural 2D weather map: R = coverage, G = cloud type (0 stratus → 1 cumulonimbus),
// B = wetness/precipitation. Low-frequency noise so coverage varies over large
// areas (some sky clear, some dense). Regenerable from a seed.

#include "CloudNoiseCommon.hlsli"

cbuffer WeatherParams : register(b0) {
    uint Resolution;
    uint Seed;
    uint _Pad0;
    uint _Pad1;
};

RWTexture2D<float4> OutWeather : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Resolution || id.y >= Resolution) return;

    float2 uv = (float2(id.xy) + 0.5f) / (float)Resolution;
    float  s  = (float)Seed * 0.131f;

    // Coverage: large soft blobs of cloudy vs clear.
    float coverage = PerlinFBM(float3(uv * 3.0f, 0.3f + s), 3.0f);
    coverage = saturate(Remap(coverage, 0.35f, 0.85f, 0.0f, 1.0f));

    // Cloud type: another low-freq field (rolling between stratus and cumulus).
    float cloudType = WorleyFBM(float3(uv * 2.0f, 0.7f + s), 2.0f);
    cloudType = saturate(cloudType * 1.2f);

    // Wetness: finer field, biased dark in the densest coverage.
    float wetness = PerlinFBM(float3(uv * 5.0f, 0.9f + s), 4.0f);

    OutWeather[id.xy] = float4(coverage, cloudType, wetness, 1.0f);
}
