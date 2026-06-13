// VS do serviço de DebugDraw (overlay 3D imediato): projeta a geometria submetida (world-space)
// pela ViewProj do frame. Cor por-vértice. Usado pela tooling do editor (gizmo etc.).
cbuffer DebugDrawCB : register(b0) {
    row_major float4x4 ViewProj;
};

struct VSInput {
    float3 pos   : POSITION;
    float3 color : COLOR;
};

struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos   = mul(float4(input.pos, 1.0f), ViewProj);
    o.color = input.color;
    return o;
}
