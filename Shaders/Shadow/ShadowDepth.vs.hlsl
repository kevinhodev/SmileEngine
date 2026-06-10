// Vertex shader do depth pass das cascatas CSM. Reusa o ObjectCB (b2) já preenchido
// pelo Renderer (lê só ModelMatrix) e aplica a LightViewProj da cascata corrente (b0).
// Passa uv para o alpha-test de folhagem (PS opcional; ignorado quando não há PS).

cbuffer ShadowCascadeCB : register(b0) {
    row_major float4x4 LightViewProj; // world -> clip da cascata (ortho)
};

cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;         // (não usado aqui)
    row_major float4x4 ModelMatrix; // local -> world
};

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput o;
    float3 world = mul(float4(input.pos, 1.0f), ModelMatrix).xyz;
    o.pos        = mul(float4(world, 1.0f), LightViewProj);
    o.uv         = input.uv;
    return o;
}
