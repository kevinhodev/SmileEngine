// Triangulo que enxerga o depth da CENA. Quanto sobra da parte oculta vem do root constant
// OccludedAlpha: 0 = Scene (some atras da geometria), >0 = XRay (fica translucido).
#include "DebugDrawCommon.hlsli"

struct VSOutput {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

float4 main(VSOutput input) : SV_TARGET {
    float Alpha = input.color.a;
    if (!SmileDebugApplyDepth(input.pos, Alpha)) discard;
    return float4(input.color.rgb, Alpha);
}
