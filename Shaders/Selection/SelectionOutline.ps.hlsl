Texture2D<float> SelectionMask : register(t0);
SamplerState     MaskSampler   : register(s0);

cbuffer OutlineParams : register(b0) {
    float2 InvSize;     
    float  Thickness;   
    float  FillStrength;
    float4 Color;       
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET0 {
    const float2 uv = input.uv;
    const float  c  = SelectionMask.Sample(MaskSampler, uv); 
    const float2 o = InvSize * Thickness;
    float edge = 0.0f;
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2( o.x, 0.0)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2(-o.x, 0.0)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2(0.0,  o.y)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2(0.0, -o.y)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2( o.x,  o.y)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2( o.x, -o.y)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2(-o.x,  o.y)) - c));
    edge = max(edge, abs(SelectionMask.Sample(MaskSampler, uv + float2(-o.x, -o.y)) - c));

    float fillA = c * FillStrength;
    float edgeA = saturate(edge) * Color.a;
    return float4(Color.rgb, max(fillA, edgeA));
}
