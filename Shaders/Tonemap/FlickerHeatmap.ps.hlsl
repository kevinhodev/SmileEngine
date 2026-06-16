cbuffer FlickerCB : register(b0) {
    float Alpha;     
    float HeatScale; 
    float Mode;     
    float Reset;    
    uint  ScrW;
    uint  ScrH;
    uint2 _pad;
};

Texture2D<float4>   Input : register(t0); 
RWTexture2D<float2> Stats : register(u0); 

struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 HeatColor(float t) {
    t = saturate(t);
    return float3(saturate(t * 3.0f), saturate(t * 3.0f - 1.0f), saturate(t * 3.0f - 2.0f));
}

float4 main(VSOutput input) : SV_TARGET {
    int2 p = int2(input.pos.xy);
    float3 c    = Input.Load(int3(p, 0)).rgb;
    float  luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));

    float2 s;
    if (Reset > 0.5f) {
        s = float2(luma, luma * luma);
    } else {
        float2 prev = Stats[p];
        s = float2(lerp(prev.x, luma, Alpha), lerp(prev.y, luma * luma, Alpha));
    }
    Stats[p] = s;

    float variance = max(s.y - s.x * s.x, 0.0f);
    float stdDev   = sqrt(variance);
    float t        = saturate(stdDev * HeatScale);
    float3 heat    = HeatColor(t);

    float3 outc = (Mode > 1.5f) ? heat : lerp(c * 0.25f, heat, saturate(t * 1.5f));
    return float4(outc, 1.0f);
}
