// BakeTransmittance.cs.hlsl
// Hillaire transmittance LUT: for each (viewHeight, viewZenithCos) texel, ray-march
// optical depth from the point to the top of the atmosphere and store exp(-depth)
// (RGB transmittance). Independent of sun direction — baked once when params change.

#include "AtmosphereCommon.hlsli"

RWTexture2D<float4> OutTransmittance : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)LutSize.x;
    uint H = (uint)LutSize.y;
    if (id.x >= W || id.y >= H) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(W, H);

    float viewHeight, viewZenithCos;
    UvToTransmittanceParams(viewHeight, viewZenithCos, uv);

    // Sample point on the +Y axis at the given height; ray makes zenith angle with up.
    float3 ro = float3(0.0f, viewHeight, 0.0f);
    float  sinZ = sqrt(saturate(1.0f - viewZenithCos * viewZenithCos));
    float3 rd = float3(sinZ, viewZenithCos, 0.0f);

    float tMax = RaySphereFar(ro, rd, kTopR);
    if (tMax <= 0.0f) {
        OutTransmittance[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    int   steps = max(2, (int)AtmoSteps.x);
    float dt    = tMax / (float)steps;

    float3 opticalDepth = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (int i = 0; i < steps; ++i) {
        float  t = (i + 0.5f) * dt;
        float3 p = ro + t * rd;
        float  altitude = length(p) - kBottomR;

        float3 rS, mS, ext;
        SampleMedium(altitude, rS, mS, ext);
        opticalDepth += ext * dt;
    }

    float3 transmittance = exp(-opticalDepth);
    OutTransmittance[id.xy] = float4(transmittance, 1.0f);
}
