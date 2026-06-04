Texture2D HDRColor : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float Luma(float3 color) {
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float4 main(PSInput input) : SV_TARGET {
    float3 color = HDRColor.Sample(LinearSampler, input.uv).rgb;
    float l = Luma(color);
    
    // Extract pixels with luminance > 1.0.
    // We use a slight knee/soft transition to avoid harsh aliasing around bright edges.
    float threshold = 1.0f;
    float knee = 0.1f;
    float soft = l - threshold + knee;
    soft = clamp(soft, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-5f);
    float contribution = max(soft, l - threshold) / max(l, 1e-5f);
    
    float3 bright = color * contribution;
    return float4(bright, 1.0f);
}
