Texture2D SourceTex : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer DownsampleCB : register(b0) {
    float2 TexelSize; 
    float2 Padding;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float x = TexelSize.x;
    float y = TexelSize.y;

    float3 a = SourceTex.Sample(LinearSampler, input.uv + float2(-2*x, 2*y)).rgb;
    float3 b = SourceTex.Sample(LinearSampler, input.uv + float2(0, 2*y)).rgb;
    float3 c = SourceTex.Sample(LinearSampler, input.uv + float2(2*x, 2*y)).rgb;

    float3 d = SourceTex.Sample(LinearSampler, input.uv + float2(-x, y)).rgb;
    float3 e = SourceTex.Sample(LinearSampler, input.uv + float2(x, y)).rgb;

    float3 f = SourceTex.Sample(LinearSampler, input.uv + float2(-2*x, 0)).rgb;
    float3 g = SourceTex.Sample(LinearSampler, input.uv).rgb;
    float3 h = SourceTex.Sample(LinearSampler, input.uv + float2(2*x, 0)).rgb;

    float3 i = SourceTex.Sample(LinearSampler, input.uv + float2(-x, -y)).rgb;
    float3 j = SourceTex.Sample(LinearSampler, input.uv + float2(x, -y)).rgb;

    float3 k = SourceTex.Sample(LinearSampler, input.uv + float2(-2*x, -2*y)).rgb;
    float3 l = SourceTex.Sample(LinearSampler, input.uv + float2(0, -2*y)).rgb;
    float3 m = SourceTex.Sample(LinearSampler, input.uv + float2(2*x, -2*y)).rgb;

    float3 color = (a+c+k+m)*0.03125f + (b+f+h+l)*0.0625f + (d+e+i+j)*0.125f + g*0.125f;
    return float4(color, 1.0f);
}
