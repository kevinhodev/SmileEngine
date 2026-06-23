#include "../GI/DDGICommon.hlsli" 

cbuffer ReflectionCB : register(b0) {
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
};

Texture2D<float4> History : register(t0); 
Texture2D<float4> GBuffer : register(t1); 
Texture2D<float>  Depth   : register(t2); 

RWTexture2D<float4> RWDenoised : register(u0);

float3 ReconstructWorld(int2 px, float deviceZ) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wH.xyz / wH.w;
}

uint PCG(uint v) { v = v * 747796405u + 2891336453u; uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u; return (w >> 22u) ^ w; }

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 px = (int2)DTid.xy;
    if (px.x >= (int)ScreenParams.x || px.y >= (int)ScreenParams.y) return;

    float4 c        = History.Load(int3(px, 0));
    float  deviceZ  = Depth.Load(int3(px, 0)).r;
    float4 gb       = GBuffer.Load(int3(px, 0));
    float  roughness    = gb.b;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));

    float frames    = c.a;
    float maxFrames = max(TemporalParams.x, 1.0f);
    float deficit   = saturate(1.0f - frames / maxFrames); 
    
    float roughTerm = saturate(roughness * 2.0f);     
    float drive     = max(deficit, roughTerm * 0.7f);

    if (deviceZ <= 0.0f || combineAlpha <= 0.0f || drive <= 0.01f) {
        RWDenoised[px] = c;
        return;
    }

    float radius = TemporalParams.z * drive; 
    if (radius < 1.0f) { RWDenoised[px] = c; return; }

    float3 N        = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    float3 worldPos = ReconstructWorld(px, deviceZ);
    float  planeThresh = max(length(worldPos - CameraPos.xyz) * 0.05f, 1e-3f);

    float3 nbSum = float3(0.0f, 0.0f, 0.0f);
    float  nbW   = 0.0f;
    const int kTaps = 12;
    uint  seed = PCG(px.x + PCG(px.y + PCG((uint)TraceParams.x)));
    float ang0 = (seed & 0xffffu) * (6.2831853f / 65536.0f);
    [loop] for (int i = 0; i < kTaps; ++i) {
        float t = (i + 0.5f) / kTaps;
        float r = radius * sqrt(t);
        float a = ang0 + t * 6.2831853f * 3.0f; 
        int2  sp = px + int2(cos(a) * r, sin(a) * r);
        if (any(sp < 0) || sp.x >= (int)ScreenParams.x || sp.y >= (int)ScreenParams.y) continue;
        float sz = Depth.Load(int3(sp, 0)).r;
        if (sz <= 0.0f) continue;
        float3 sW = ReconstructWorld(sp, sz);
        if (abs(dot(sW - worldPos, N)) > planeThresh) continue; 
        float4 sc = History.Load(int3(sp, 0));
        float  wg = exp(-2.0f * t); 
        nbSum += sc.rgb * wg;
        nbW   += wg;
    }

    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    float3 nbAvg = (nbW > 1e-4f) ? (nbSum / nbW) : c.rgb;
    float  cl = dot(c.rgb, kLuma);
    float  nl = dot(nbAvg, kLuma);
    float3 cClamped = (cl > nl * 2.0f + 1e-3f) ? c.rgb * ((nl * 2.0f + 1e-3f) / max(cl, 1e-4f)) : c.rgb;

    float  wc   = lerp(1.0f, 0.25f, drive);
    float3 outc = (cClamped * wc + nbSum) / (wc + nbW);
    RWDenoised[px] = float4(outc, frames);
}
