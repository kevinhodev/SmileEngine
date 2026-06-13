#include "CloudNoiseCommon.hlsli"

cbuffer NoiseParams : register(b0) {
    uint Resolution;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

RWTexture3D<float4> OutNoise : register(u0);

[numthreads(8, 8, 8)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Resolution || id.y >= Resolution || id.z >= Resolution) return;

    float3 uvw = (float3(id) + 0.5f) / (float)Resolution;

    float r = WorleyNoise(uvw, 8.0f);
    float g = WorleyNoise(uvw, 16.0f);
    float b = WorleyNoise(uvw, 32.0f);

    OutNoise[id] = float4(r, g, b, 1.0f);
}
