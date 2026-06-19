#include "Common.hlsli"

cbuffer PushConstants : register(b0) {
    uint OutputSize;  
    uint SrcMip;      
    uint _Pad0; uint _Pad1;
    uint _Pad2; uint _Pad3; uint _Pad4; uint _Pad5;
};

TextureCube<float4>      EnvCube     : register(t0);
SamplerState             LinearClamp : register(s0);
RWTexture2DArray<float4> OutputMip   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID) {
    if (dispatchID.x >= OutputSize || dispatchID.y >= OutputSize) return;

    float2 uv  = (float2(dispatchID.xy) + 0.5f) / float(OutputSize);
    float3 dir = CubeFaceToDirection(dispatchID.z, uv);

    float4 c = EnvCube.SampleLevel(LinearClamp, dir, float(SrcMip));
    OutputMip[uint3(dispatchID.xy, dispatchID.z)] = c;
}
