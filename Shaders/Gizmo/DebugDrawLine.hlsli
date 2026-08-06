#ifndef SMILE_DEBUGDRAW_LINE_HLSLI
#define SMILE_DEBUGDRAW_LINE_HLSLI

#include "DebugDrawCommon.hlsli"

// Linha grossa com espessura em PIXELS e anti-aliasing, estilo o caminho de linha grossa do
// FBatchedElements da Unreal — mas expandindo no VS por instancia, e nao na CPU: o VB guarda um
// comando por segmento (A, B, cor, espessura) e o SV_VertexID gera os 4 cantos do quad.
struct LineVSInput {
    float3 A         : POSITION0;
    float3 B         : POSITION1;
    float4 Color     : COLOR0;
    float  Thickness : TEXCOORD0; // pixels
    uint   VertexId  : SV_VertexID;
};

struct LineVSOutput {
    float4 Pos    : SV_POSITION;
    float4 Color  : COLOR;
    float  DistPx : TEXCOORD0; // distancia assinada ao eixo da linha, em pixels
    float  HalfPx : TEXCOORD1; // meia-espessura pedida, em pixels
};

LineVSOutput SmileDebugLineVS(LineVSInput In) {
    LineVSOutput Out;

    float4 Pa = mul(float4(In.A, 1.0f), ViewProj);
    float4 Pb = mul(float4(In.B, 1.0f), ViewProj);

    // Clipa contra o near ANTES de expandir. A expansao acontece em NDC, entao um endpoint com
    // w<=0 produziria NDC lixo e o quad sairia montado errado — o rasterizador nao teria como
    // salvar depois. Este era o trabalho que o LINELIST deixava de graca pro hardware.
    const float kMinW = 1e-4f;
    if (Pa.w < kMinW && Pb.w < kMinW) { // segmento inteiro atras da camera: degenera
        Out.Pos = float4(0.0f, 0.0f, 0.0f, 0.0f);
        Out.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        Out.DistPx = 0.0f;
        Out.HalfPx = 0.0f;
        return Out;
    }
    if (Pa.w < kMinW)      Pa = lerp(Pa, Pb, (kMinW - Pa.w) / (Pb.w - Pa.w));
    else if (Pb.w < kMinW) Pb = lerp(Pb, Pa, (kMinW - Pb.w) / (Pa.w - Pb.w));

    float2 Na    = Pa.xy / Pa.w;
    float2 Nb    = Pb.xy / Pb.w;
    float2 DeltaPx = (Nb - Na) * 0.5f * Screen.xy;
    float  Len     = length(DeltaPx);
    float2 Dir     = (Len > 1e-6f) ? (DeltaPx / Len) : float2(1.0f, 0.0f);
    float2 Normal  = float2(-Dir.y, Dir.x);

    // Expande 1px alem da meia-espessura: e a folga onde o PS desenha a rampa de cobertura.
    float HalfPx = max(In.Thickness, 1.0f) * 0.5f;
    float Expand = HalfPx + 1.0f;

    // Triangle strip de 4 vertices: bit 0 escolhe o endpoint, bit 1 escolhe o lado.
    float4 P    = (In.VertexId & 1) ? Pb : Pa;
    float  Side = (In.VertexId & 2) ? 1.0f : -1.0f;

    // Pixels -> NDC, vezes w porque o divide de perspectiva vai desfazer a multiplicacao.
    P.xy += Normal * Side * Expand * (2.0f / Screen.xy) * P.w;

    Out.Pos    = P;
    Out.Color  = In.Color;
    Out.DistPx = Side * Expand;
    Out.HalfPx = HalfPx;
    return Out;
}

float4 SmileDebugLinePS(LineVSOutput In) {
    // Rampa linear de 1px na borda: |dist| <= half fica cheio, half+1 zera.
    float Coverage = saturate(In.HalfPx - abs(In.DistPx) + 0.5f);
    float Alpha    = In.Color.a * Coverage;

#if SMILE_DD_DEPTH_TEST
    if (!SmileDebugApplyDepth(In.Pos, Alpha)) discard;
#else
    if (Alpha <= 0.002f) discard;
#endif

    return float4(In.Color.rgb, Alpha);
}

#endif // SMILE_DEBUGDRAW_LINE_HLSLI
