#ifndef SMILE_SUN_SHAFT_BLOOM_COMMON
#define SMILE_SUN_SHAFT_BLOOM_COMMON

cbuffer SunShaftBloomCB : register(b0) {
    float4 SunPosParams;
    float4 BloomParams;
    float4 BlurParams;
    float4 TintDepth;
    float4 ScreenParams;
    row_major float4x4 InvViewProj;
    float4 CameraWorldPos;
};

SamplerState LinearClamp : register(s0);
SamplerState PointClamp  : register(s1);

#define SUN_SHAFT_BLOOM_SAMPLES 12

#endif
