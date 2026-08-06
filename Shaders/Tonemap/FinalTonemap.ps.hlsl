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

float InterleavedGradientNoise(float2 pixelPosition) {
    // Jimenez-style screen-space hash: high-frequency, deterministic and free of textures.
    return frac(52.9829189f *
                frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

float TriangularQuantizationDither(float2 pixelPosition) {
    // Inverse CDF of a zero-mean triangular distribution on [-1, 1], matching the
    // backbuffer-quantization strategy used by UE. A static screen-space pattern avoids
    // shimmer because this pass runs after the temporal resolve.
    float uniformNP = InterleavedGradientNoise(pixelPosition) * 2.0f - 1.0f;
    float grainSign = uniformNP < 0.0f ? -1.0f : 1.0f;
    return grainSign * (1.0f - sqrt(saturate(1.0f - abs(uniformNP))));
}

float4 main(PSInput input) : SV_TARGET {
    float3 hdrColor = FullResHDR.Sample(LinearSampler, input.uv).rgb;
    float3 bloomColor = BloomTex.Sample(LinearSampler, input.uv).rgb;

    float3 color = hdrColor * Exposure + bloomColor * BloomIntensity;

    float3 tonemapped = ACESFilm(color);

    float3 srgb = pow(tonemapped, 1.0f / 2.2f);

    // The swapchain is R8G8B8A8_UNORM: dither in display space immediately before its
    // 8-bit quantization. One triangular LSB breaks dark-sky contours without changing
    // the HDR signal, exposure, bloom or tone curve.
    float dither = TriangularQuantizationDither(floor(input.pos.xy));
    srgb = saturate(srgb + dither * (1.0f / 255.0f));

    return float4(srgb, 1.0f);
}
