cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;            
    row_major float4x4 ModelMatrix;
    row_major float4x4 CurMVPNoJitter; 
    row_major float4x4 PrevMVP;       
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
    float4 curClip     : TEXCOORD3; 
    float4 prevClip    : TEXCOORD4; 
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos         = mul(float4(input.pos, 1.0f), MVP);
    o.worldPos    = mul(float4(input.pos, 1.0f), ModelMatrix).xyz;
    o.worldNormal = mul(input.normal, (float3x3)ModelMatrix);
    o.uv          = input.uv;
    o.curClip     = mul(float4(input.pos, 1.0f), CurMVPNoJitter);
    o.prevClip    = mul(float4(input.pos, 1.0f), PrevMVP);
    return o;
}
