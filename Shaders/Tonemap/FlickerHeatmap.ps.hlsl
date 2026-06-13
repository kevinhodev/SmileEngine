// FlickerHeatmap.ps.hlsl — debug view: heatmap de flicker temporal (variancia por pixel).
//
// Porte do que era feito offline (numpy: std temporal por pixel ao longo de N frames) p/ a engine,
// em tempo real. Mantem uma media movel exponencial (EMA) de luma e luma^2 por pixel num buffer
// persistente (Stats, RG32F); std = sqrt(E[x^2] - E[x]^2). Roda no HDR (pre-tonemap) entre o TAA e
// o bloom -> destaca glint especular/borda que cintila. Quente (vermelho/branco) = mais flicker.

cbuffer FlickerCB : register(b0) {
    float Alpha;     // peso do frame atual na EMA (menor = janela mais longa/suave)
    float HeatScale; // std -> [0,1] do colormap (maior = mais sensivel)
    float Mode;      // 1 = overlay sobre a cena escurecida, 2 = heatmap puro
    float Reset;     // 1 = (re)inicia a EMA com o frame atual (std 0)
    uint  ScrW;
    uint  ScrH;
    uint2 _pad;
};

Texture2D<float4>   Input : register(t0); // cor HDR (saida do TAA / resolvido)
RWTexture2D<float2> Stats : register(u0); // EMA persistente (mean, meanSq) de luma

struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Colormap "hot": preto -> vermelho -> amarelo -> branco.
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

    // Overlay (cena escura + calor onde cintila) ou heatmap puro.
    float3 outc = (Mode > 1.5f) ? heat : lerp(c * 0.25f, heat, saturate(t * 1.5f));
    return float4(outc, 1.0f);
}
