// PS do serviço de DebugDraw: cor sólida (unlit). Desenhado pós-tonemap no backbuffer LDR, sem
// depth (sempre por cima), então a cor vai direta.
struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

float4 main(VSOutput input) : SV_TARGET {
    return float4(input.color, 1.0f);
}
