// VS do preview de material (Editor de Materiais). Mesh unica centrada na origem;
// camera orbital. CB compartilhado com o PS (b0).

cbuffer PreviewCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 Model;
    float4 CameraPos;       // xyz = posicao da camera
    float4 SunDirIntensity; // xyz = direcao PARA o sol, w = intensidade
    float4 SunColor;        // rgb
    float4 IBLParams;       // x = intensidade, y = rotacao do env (rad), z = mip max do specular, w = enabled
};

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

struct VSOutput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos         = mul(float4(input.pos, 1.0f), MVP);
    o.worldPos    = mul(float4(input.pos, 1.0f), Model).xyz;
    o.worldNormal = mul(input.normal, (float3x3)Model);
    o.uv          = input.uv;
    return o;
}
