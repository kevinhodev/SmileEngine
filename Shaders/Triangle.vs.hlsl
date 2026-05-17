// Vertex shader com transformacao MVP via Constant Buffer.
// Recebe POSITION (float3) + COLOR (float4) do Input Assembler,
// multiplica pela matriz MVP e passa a cor interpolada ao Pixel Shader.

cbuffer TransformCB : register(b0) {
    row_major float4x4 MVP; // row_major: C++ escreve em row-major, HLSL deve ler igual
    row_major float4x4 ModelMatrix;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
};

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos         = mul(float4(input.pos, 1.0), MVP);
    o.worldPos    = mul(float4(input.pos, 1.0), ModelMatrix).xyz;
    // Multiplicando como vetor normal (sem translação)
    o.worldNormal = mul(input.normal, (float3x3)ModelMatrix); 
    return o;
}
