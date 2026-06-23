cbuffer SkyboxCB : register(b0) {
    row_major float4x4 InvViewProjNoTranslation;
    float IBLIntensity;
    float IBLRotation;   
    float _Pad0;
    float _Pad1;
};

TextureCube<float4> EnvCube     : register(t0);
SamplerState        IBLSampler  : register(s0);

struct PSInput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

float3 RotateY(float3 v, float angle) {
    float s = sin(angle), c = cos(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float4 main(PSInput input) : SV_TARGET {
    float4 worldFar = mul(float4(input.clipXY, 1.0f, 1.0f), InvViewProjNoTranslation);
    float3 dir = normalize(worldFar.xyz);
    dir = RotateY(dir, IBLRotation);

    float3 color = EnvCube.SampleLevel(IBLSampler, dir, 0.0f).rgb * IBLIntensity;
    return float4(color, 1.0f);
}
