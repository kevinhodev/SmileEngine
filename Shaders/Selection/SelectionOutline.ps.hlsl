// Edge-detect fullscreen da mascara de selecao -> desenha a borda colorida sobre o backbuffer
// (LDR, depois do tonemap, p/ a cor nao passar por bloom/exposicao). Estilo Flax/Cry: amostra
// a mascara numa vizinhanca; onde ha transicao dentro<->fora (borda da silhueta), emite a cor
// com alpha. O blend alpha (no PSO) compoe so a borda; o resto sai com alpha 0 (sem mudanca).
//
// O VS e o fullscreen-triangle compartilhado (Tonemap/PostProcess.vs), que da SV_POSITION + uv.

Texture2D<float> SelectionMask : register(t0);
SamplerState     MaskSampler   : register(s0);

cbuffer OutlineParams : register(b0) {
    float2 InvSize;     // (1/largura, 1/altura) em texels
    float  Thickness;   // raio da borda em texels
    float  FillStrength;// lavagem de cor sobre o objeto inteiro (estilo highlight da Cry); 0 = so borda
    float4 Color;       // rgb = cor; a = intensidade (alpha do blend) da borda
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET0 {
    const float2 uv = input.uv;
    const float  c  = SelectionMask.Sample(MaskSampler, uv); // centro: 1 dentro, 0 fora

    // 8 vizinhos a distancia 'Thickness' (cardeais + diagonais). A borda existe onde o centro
    // difere de algum vizinho (|N - c|). Da uma linha de ~2*Thickness px straddling a silhueta.
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

    // Highlight estilo Cry: lavagem de cor sobre a silhueta inteira (alpha = mascara * FillStrength)
    // COMBINADA com a borda (alpha = edge * Color.a). O max() deixa a borda mais forte que o fill.
    float fillA = c * FillStrength;
    float edgeA = saturate(edge) * Color.a;
    return float4(Color.rgb, max(fillA, edgeA));
}
