#ifndef GTAO_COMMON_HLSLI
#define GTAO_COMMON_HLSLI

cbuffer GTAOCB : register(b0) {
    float4 ProjA;        
    float4 ScreenParams; 
    float4 Params;       
    float4 Params2;   
    float4 Params3;      
    row_major float4x4 ViewMatrix; 
    float4 ProjOffset;
};

static const float GTAO_PI     = 3.14159265f;
static const float GTAO_HALFPI = 1.57079633f;

float GTAO_LinearizeDepth(float d) {
    return ProjA.w / (d - ProjA.z);
}

float3 GTAO_ViewPos(float2 px, float rawDepth) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float  zv  = GTAO_LinearizeDepth(rawDepth);
    float  ndcX = uv.x * 2.0f - 1.0f - ProjOffset.x;
    float  ndcY = 1.0f - uv.y * 2.0f - ProjOffset.y;
    float  xv  = ndcX * zv / ProjA.x;
    float  yv  = ndcY * zv / ProjA.y;
    return float3(xv, yv, zv);
}

float GTAO_IGN(float2 px) {
    return frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));
}

float3 GTAO_ViewNormal(float3 encoded) {
    float3 n = mul(encoded * 2.0f - 1.0f, (float3x3)ViewMatrix);
    return n * rsqrt(max(dot(n, n), 1e-8f));
}

// Symmetric tangent-plane test preserves slopes while rejecting separated surfaces.
// Normal rejection also protects folds whose depths happen to coincide.
float GTAO_BilateralWeight(float3 p0, float3 n0, float3 p1, float3 n1) {
    float3 delta = p1 - p0;
    float residual = max(abs(dot(n0, delta)), abs(dot(n1, delta)));
    float sigma = max(0.005f, min(max(Params.x, 0.0f) * 0.1f,
                                 max(p0.z, p1.z) * 0.002f));
    float normalWeight = pow(saturate(dot(n0, n1)), 8.0f);
    float normalizedResidual = residual / sigma;
    return normalWeight * exp2(-normalizedResidual * normalizedResidual);
}

#endif
