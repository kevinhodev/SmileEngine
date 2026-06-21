#include "../GI/DDGICommon.hlsli" 

cbuffer CompositeCB : register(b0) {
    row_major float4x4 InvViewProj; 
    float4 CameraPos;       
    float4 ScreenParams;    
    float4 ReflectParams;   
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 HalfScreenParams;
    row_major float4x4 PrevViewProj;
    float4 TemporalParams;
    float4 DebugParams;
    row_major float4x4 View;        // (padding p/ casar o layout do ReflectionConstants)
    float4 NrdSpecParams;           // w = 1 -> OUT_SPEC do NRD em YCoCg (desempacotar)
};

Texture2D<float4> Reflection : register(t0); 
Texture2D<float4> GBuffer    : register(t1); 
Texture2D<float>  Depth      : register(t2); 
Texture2D<float4> BRDFLut    : register(t3); 

SamplerState LinearClamp : register(s0);

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET {
    int2 px = int2(input.pos.xy);

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  metallic  = gb.a;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f || combineAlpha <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint dbg = (uint)DebugParams.x;
    if (dbg == 1u) {
        float frames = Reflection.Load(int3(px, 0)).a;
        float t = saturate(frames / max(DebugParams.y, 1.0f));
        return float4(lerp(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), t), 0.0f);
    }
    if (dbg == 2u) {
        bool isMirror = roughness <= TemporalParams.w;
        return float4((isMirror ? float3(0.0f, 0.7f, 0.0f) : float3(0.7f, 0.0f, 0.0f)), 0.0f);
    }

    float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 V = normalize(CameraPos.xyz - worldPos);
    float  NoV = saturate(dot(N, V));

    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), float3(1.0f, 1.0f, 1.0f), metallic);
    float2 brdf = BRDFLut.SampleLevel(LinearClamp, float2(NoV, roughness), 0.0f).rg;

    float3 reflRad = Reflection.Load(int3(px, 0)).rgb;
    // NRD REBLUR escreve a OUT_SPEC em YCoCg -> desempacota p/ linear (_NRD_YCoCgToLinear).
    if (NrdSpecParams.w > 0.5f) {
        float t = reflRad.x - reflRad.z;
        reflRad = max(float3(t + reflRad.y, reflRad.x + reflRad.z, t - reflRad.y), 0.0f);
    }
    float3 spec    = reflRad * (F0 * brdf.x + brdf.y) * combineAlpha;

    return float4(spec, 0.0f); 
}
