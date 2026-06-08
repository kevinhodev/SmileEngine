Texture2D LowResTex : register(t0);
Texture2D HighResTex : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer UpsampleCB : register(b0) {
    float2 TexelSize; // unused but matches 256-byte aligned offset struct
    float FilterRadius; // control blur width / tent radius
    float Padding;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float d = FilterRadius;

    // 9-tap 2D tent upsampler (Bilinear interpolation)
    float3 a = LowResTex.Sample(LinearSampler, input.uv + float2(-d, d)).rgb;
    float3 b = LowResTex.Sample(LinearSampler, input.uv + float2(0, d)).rgb * 2.0;
    float3 c = LowResTex.Sample(LinearSampler, input.uv + float2(d, d)).rgb;

    float3 d_ = LowResTex.Sample(LinearSampler, input.uv + float2(-d, 0)).rgb * 2.0;
    float3 e = LowResTex.Sample(LinearSampler, input.uv).rgb * 4.0;
    float3 f = LowResTex.Sample(LinearSampler, input.uv + float2(d, 0)).rgb * 2.0;

    float3 g = LowResTex.Sample(LinearSampler, input.uv + float2(-d, -d)).rgb;
    float3 h = LowResTex.Sample(LinearSampler, input.uv + float2(0, -d)).rgb * 2.0;
    float3 i = LowResTex.Sample(LinearSampler, input.uv + float2(d, -d)).rgb;

    float3 upsampled = (a + b + c + d_ + e + f + g + h + i) / 16.0;
    float3 high = HighResTex.Sample(LinearSampler, input.uv).rgb;

    // Additive blend: current level + blurred upsampled lower resolution
    return float4(high + upsampled, 1.0f);
}
