Texture2D FullResHDR : register(t0);
Texture2D BloomTex   : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer PostParamsCB : register(b0) {
    float BloomIntensity;
    float Exposure;
    float2 Padding;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Stephen Hill ACES Fit (PBR/HDR color to sRGB display color)
static const float3x3 ACESInputMat = {
    0.59719f, 0.35458f, 0.04823f,
    0.07608f, 0.90834f, 0.01558f,
    0.02244f, 0.08207f, 0.89548f
};

static const float3x3 ACESOutputMat = {
    1.60475f, -0.53108f, -0.07367f,
    -0.10210f,  1.10813f, -0.00603f,
    -0.00327f, -0.07276f,  1.07602f
};

float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.432951f) + 0.238081f;
    return a / b;
}

float3 ACESFilm(float3 color) {
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

float4 main(PSInput input) : SV_TARGET {
    float3 hdrColor = FullResHDR.Sample(LinearSampler, input.uv).rgb;
    float3 bloomColor = BloomTex.Sample(LinearSampler, input.uv).rgb;

    // Combine HDR + Bloom
    float3 color = hdrColor * Exposure + bloomColor * BloomIntensity;

    // Apply ACES Filmic curve
    float3 tonemapped = ACESFilm(color);

    // Convert to display space (Gamma 2.2)
    float3 srgb = pow(tonemapped, 1.0f / 2.2f);

    return float4(srgb, 1.0f);
}
