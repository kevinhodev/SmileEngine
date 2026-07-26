// Billboard dos icones de luz do editor (estilo sprite S_LightPoint da UE / ViewportIcons
// do Flax): o quad expande na CPU->VS com os eixos da camera (CamRight/CamUp do CB), tamanho
// em unidades de mundo ja escalado por distancia na CPU (constante em tela).
cbuffer DebugDrawCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 Params;   // xy = 1/backbuffer, z = bias de depth (nao usado aqui)
    float4 CamRight; // xyz = eixo X da camera (mundo)
    float4 CamUp;    // xyz = eixo Y da camera (mundo)
};

struct VSInput {
    float3 center : POSITION;
    float3 color  : COLOR;
    float2 corner : TEXCOORD0; // -1..1
    float3 misc   : TEXCOORD1; // x = meia-largura (mundo), y = tipo (0=point 1=spot), z = selecionada
};

struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
    float2 uv    : TEXCOORD0;
    float3 misc  : TEXCOORD1;
};

VSOutput main(VSInput v) {
    float3 w = v.center + (CamRight.xyz * v.corner.x + CamUp.xyz * v.corner.y) * v.misc.x;
    VSOutput o;
    o.pos   = mul(float4(w, 1.0f), ViewProj);
    o.color = v.color;
    o.uv    = v.corner;
    o.misc  = v.misc;
    return o;
}
