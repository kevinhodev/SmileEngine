#include "Common.hlsli"

cbuffer PushConstants : register(b0) {
    uint OutputSize;
    uint NumSamples; 
    uint _Pad0; uint _Pad1;
    uint _Pad2; uint _Pad3; uint _Pad4; uint _Pad5;
};

Texture2D<float4> Unused : register(t0);
SamplerState      LinearClamp : register(s0);
RWTexture2D<float2> OutputLut : register(u0);

float2 IntegrateBRDF(float NoV, float roughness) {
    float3 V;
    V.x = sqrt(1.0f - NoV * NoV);
    V.y = 0.0f;
    V.z = NoV;

    float A = 0.0f;
    float B = 0.0f;
    float3 N = float3(0, 0, 1);

    for (uint i = 0; i < NumSamples; ++i) {
        float2 Xi = Hammersley(i, NumSamples);
        float3 H  = ImportanceSampleGGX(Xi, N, roughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NoL = saturate(L.z);
        float NoH = saturate(H.z);
        float VoH = saturate(dot(V, H));

        if (NoL > 0.0f) {
            float G     = G_Smith_IBL(NoV, NoL, roughness);
            float G_Vis = (G * VoH) / max(NoH * NoV, 1e-5f);
            float Fc    = pow(1.0f - VoH, 5.0f);
            A += (1.0f - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    return float2(A, B) / float(NumSamples);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID) {
    if (dispatchID.x >= OutputSize || dispatchID.y >= OutputSize) return;

    float NoV       = (float(dispatchID.x) + 0.5f) / float(OutputSize);
    float roughness = (float(dispatchID.y) + 0.5f) / float(OutputSize);

    float keep = Unused.Load(int3(0, 0, 0)).r * 0.0f;

    OutputLut[dispatchID.xy] = IntegrateBRDF(max(NoV, 1e-4f), max(roughness, 0.04f)) + keep;
}
