// Converts an equirectangular (latlong) HDR into a 6-face cubemap, mip 0.
// Dispatch: ceil(size/8) x ceil(size/8) x 6, thread group 8x8x1.

#include "Common.hlsli"

cbuffer PushConstants : register(b0) {
    uint OutputSize; // edge length of the cube face
    uint _Pad0; uint _Pad1; uint _Pad2;
    uint _Pad3; uint _Pad4; uint _Pad5; uint _Pad6;
};

Texture2D<float4>          EquirectMap : register(t0);
SamplerState               LinearClamp : register(s0);
RWTexture2DArray<float4>   OutputCube  : register(u0);

float2 DirectionToEquirectUV(float3 dir) {
    // atan2(z, x) maps to longitude in [-PI, PI]; asin(y) maps to latitude in [-PI/2, PI/2].
    float u = atan2(dir.z, dir.x) * (1.0f / TwoPI) + 0.5f;
    float v = 0.5f - asin(dir.y) * InvPI;
    return float2(u, v);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID) {
    if (dispatchID.x >= OutputSize || dispatchID.y >= OutputSize) return;

    float2 uv = (float2(dispatchID.xy) + 0.5f) / float(OutputSize);
    float3 dir = CubeFaceToDirection(dispatchID.z, uv);
    float2 src = DirectionToEquirectUV(dir);

    float3 color = EquirectMap.SampleLevel(LinearClamp, src, 0.0f).rgb;
    OutputCube[uint3(dispatchID.xy, dispatchID.z)] = float4(color, 1.0f);
}
