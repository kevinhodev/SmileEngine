Texture2D InputTex : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer BlurCB : register(b0) {
    float4 Direction; 
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float2 texelSize = Direction.xy;
    float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

    float3 color = InputTex.Sample(LinearSampler, input.uv).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        color += InputTex.Sample(LinearSampler, input.uv + texelSize * (float)i).rgb * weights[i];
        color += InputTex.Sample(LinearSampler, input.uv - texelSize * (float)i).rgb * weights[i];
    }
    return float4(color, 1.0f);
}
