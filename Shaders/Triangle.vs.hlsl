cbuffer TransformCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
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
    o.worldPos    = mul(float4(input.pos, 1.0f), ModelMatrix).xyz;
    o.worldNormal = mul(input.normal, (float3x3)ModelMatrix);
    o.uv          = input.uv;
    return o;
}
