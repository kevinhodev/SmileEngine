// Triangulo Foreground: sem teste de depth, sempre visivel.
#include "DebugDrawCommon.hlsli"

struct VSOutput {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

float4 main(VSOutput input) : SV_TARGET {
    if (input.color.a <= 0.002f) discard;
    return input.color;
}
