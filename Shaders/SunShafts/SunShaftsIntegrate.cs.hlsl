#include "SunShaftsCommon.hlsli"

Texture3D<float4> ScatteringVolume : register(t0);
RWTexture3D<float4> OutIntegrated : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(GridParams.xy)))
        return;

    float2 uv = (float2(id.xy) + 0.5f) / GridParams.xy;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farH = mul(float4(ndc, 0.0f, 1.0f), InvViewProj);
    float3 farWorld = farH.xyz / max(farH.w, 1e-6f);
    float rayLengthPerViewDepth =
        length(farWorld - CameraWorldPos.xyz) / max(FogLayer2.w, 1e-4f);

    float4 accumulated = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float previousDepth = 0.0f;

    [loop] for (uint z = 0; z < (uint)GridParams.z; ++z) {
        float sliceEnd = ShaftSliceDepth(float(z + 1));
        float stepLength = (sliceEnd - previousDepth) * rayLengthPerViewDepth;
        float4 scatteringExtinction = ScatteringVolume.Load(int4(id.xy, z, 0));

        float extinction = max(scatteringExtinction.a, 0.0f);
        float transmittance = exp(-extinction * stepLength);
        float3 integratedSource = scatteringExtinction.rgb *
            ((1.0f - transmittance) / max(extinction, 1e-6f));

        accumulated.rgb += integratedSource * accumulated.a;
        accumulated.a *= transmittance;
        OutIntegrated[uint3(id.xy, z)] = max(accumulated, 0.0f);
        previousDepth = sliceEnd;
    }
}
