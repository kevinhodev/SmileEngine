// GGX-importance-sampled prefiltering of an environment cubemap, one mip per
// dispatch (caller sets root constants Roughness/OutputSize for each mip).
// Uses Unreal's solid-angle mip-bias to fight fireflies at low roughness.
// Dispatch: ceil(OutputSize/8) x ceil(OutputSize/8) x 6, thread group 8x8x1.

#include "Common.hlsli"

cbuffer PushConstants : register(b0) {
    uint  OutputSize;  // edge length of THIS mip
    uint  SourceSize;  // edge length of source cube mip 0
    float Roughness;   // [0,1]
    uint  NumSamples;  // typically 512
    uint  _Pad0; uint _Pad1; uint _Pad2; uint _Pad3;
};

TextureCube<float4>      EnvCube     : register(t0);
SamplerState             LinearClamp : register(s0);
RWTexture2DArray<float4> OutputCube  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID) {
    if (dispatchID.x >= OutputSize || dispatchID.y >= OutputSize) return;

    float2 uv = (float2(dispatchID.xy) + 0.5f) / float(OutputSize);
    float3 N  = CubeFaceToDirection(dispatchID.z, uv);
    float3 R  = N;
    float3 V  = N; // Karis split-sum assumption: V = R = N.

    float3 prefiltered = 0.0f.xxx;
    float  weight      = 0.0f;

    // Source texel solid angle (uniform across a cube face for a given mip).
    // saTexel = 4*PI / (6 * size * size).
    float saTexel = 4.0f * PI / (6.0f * float(SourceSize) * float(SourceSize));

    for (uint i = 0; i < NumSamples; ++i) {
        float2 Xi = Hammersley(i, NumSamples);
        float3 H  = ImportanceSampleGGX(Xi, N, Roughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);
        float  NoL = saturate(dot(N, L));
        if (NoL <= 0.0f) continue;

        // Mip bias from solid angle (Krivanek/Karis). Without it, low-roughness
        // mips alias the bright sun pixel and produce fireflies.
        float NoH    = saturate(dot(N, H));
        float HoV    = saturate(dot(H, V));
        float D      = D_GGX(NoH, Roughness);
        float pdf    = (D * NoH) / max(4.0f * HoV, 1e-4f);
        float saSamp = 1.0f / (float(NumSamples) * max(pdf, 1e-4f));
        float mip    = (Roughness < 1e-3f) ? 0.0f
                                            : 0.5f * log2(saSamp / saTexel);
        mip = max(mip, 0.0f);

        // Karis HDR clamp: cap the env sample to kill bright-pixel fireflies
        // that GGX importance sampling would otherwise concentrate in a few
        // texels (sun disc on a sunset HDR is the classic offender).
        float3 sample = EnvCube.SampleLevel(LinearClamp, L, mip).rgb;
        sample = min(sample, 50.0f.xxx);
        prefiltered += sample * NoL;
        weight      += NoL;
    }
    prefiltered /= max(weight, 1e-4f);
    OutputCube[uint3(dispatchID.xy, dispatchID.z)] = float4(prefiltered, 1.0f);
}
