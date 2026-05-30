// Cosine-weighted convolution of an environment cubemap into an irradiance
// cubemap. Output is small (32x32x6) so brute-force phi/theta is fine.
// Dispatch: ceil(size/8) x ceil(size/8) x 6, thread group 8x8x1.

#include "Common.hlsli"

cbuffer PushConstants : register(b0) {
    uint OutputSize;
    uint _Pad0; uint _Pad1; uint _Pad2;
    uint _Pad3; uint _Pad4; uint _Pad5; uint _Pad6;
};

TextureCube<float4>      EnvCube     : register(t0);
SamplerState             LinearClamp : register(s0);
RWTexture2DArray<float4> OutputCube  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID) {
    if (dispatchID.x >= OutputSize || dispatchID.y >= OutputSize) return;

    float2 uv  = (float2(dispatchID.xy) + 0.5f) / float(OutputSize);
    float3 N   = CubeFaceToDirection(dispatchID.z, uv);

    // Tangent frame around N.
    float3 up        = abs(N.y) < 0.999f ? float3(0,1,0) : float3(0,0,1);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    float3 irradiance = 0.0f.xxx;
    const float deltaPhi   = 0.025f;
    const float deltaTheta = 0.025f;
    float sampleCount = 0.0f;

    for (float phi = 0.0f; phi < TwoPI; phi += deltaPhi) {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += deltaTheta) {
            float3 tangentSample = float3(sin(theta) * cos(phi),
                                          sin(theta) * sin(phi),
                                          cos(theta));
            float3 sampleVec = tangent * tangentSample.x
                             + bitangent * tangentSample.y
                             + N * tangentSample.z;
            // cos(theta) * sin(theta) = the solid-angle weight for the hemisphere integral.
            irradiance += EnvCube.SampleLevel(LinearClamp, sampleVec, 0.0f).rgb
                        * cos(theta) * sin(theta);
            sampleCount += 1.0f;
        }
    }
    irradiance = PI * irradiance / sampleCount;
    OutputCube[uint3(dispatchID.xy, dispatchID.z)] = float4(irradiance, 1.0f);
}
