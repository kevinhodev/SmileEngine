#ifndef GTAO_COMMON_HLSLI
#define GTAO_COMMON_HLSLI

cbuffer GTAOCB : register(b0) {
    float4 ProjA;        
    float4 ScreenParams; 
    float4 Params;       
    float4 Params2;   
    float4 Params3;      
    row_major float4x4 ViewMatrix; 
};

static const float GTAO_PI     = 3.14159265f;
static const float GTAO_HALFPI = 1.57079633f;

float GTAO_LinearizeDepth(float d) {
    return ProjA.w / (d - ProjA.z);
}

float3 GTAO_ViewPos(float2 px, float rawDepth) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float  zv  = GTAO_LinearizeDepth(rawDepth);
    float  ndcX = uv.x * 2.0f - 1.0f;
    float  ndcY = 1.0f - uv.y * 2.0f;
    float  xv  = ndcX * zv / ProjA.x;
    float  yv  = ndcY * zv / ProjA.y;
    return float3(xv, yv, zv);
}

float GTAO_IGN(float2 px) {
    return frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));
}

#endif
