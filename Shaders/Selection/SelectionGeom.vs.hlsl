cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
};

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

float4 main(VSInput input) : SV_POSITION {
    return mul(float4(input.pos, 1.0f), MVP);
}
