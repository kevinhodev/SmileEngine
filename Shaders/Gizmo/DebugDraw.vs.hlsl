// VS dos triangulos do DebugDraw (pontas do gizmo). Linha tem caminho proprio e instanciado
// (DebugDrawLine.*); aqui e vertice a vertice mesmo.
#include "DebugDrawCommon.hlsli"

struct VSInput {
    float3 pos   : POSITION;
    float4 color : COLOR;   // R8G8B8A8_UNORM: chega ja em 0..1
};

struct VSOutput {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos   = mul(float4(input.pos, 1.0f), ViewProj);
    o.color = input.color;
    return o;
}
