#include "DDGICommon.hlsli"

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;     
    float4 DistAtlasParams; 
    float4 DebugParams;     
    float4 CameraPos;
};

Texture2D<float4> IrradAtlas : register(t2);
Texture2D<float4> DistAtlas  : register(t3);
SamplerState      LinearClamp : register(s0);

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 nrm      : NORMAL;
    nointerpolation float2 irrTile  : TEXCOORD0;
    nointerpolation float2 distTile : TEXCOORD1;
    nointerpolation float4 extra    : TEXCOORD2;
};

float3 Heat(float t) {
    t = saturate(t);
    float3 c = float3(0.0f, 0.0f, 0.0f);
    c += float3(0.0f, 0.0f, 1.0f) * saturate(1.0f - abs((t - 0.00f) * 4.0f));
    c += float3(0.0f, 1.0f, 1.0f) * saturate(1.0f - abs((t - 0.25f) * 4.0f));
    c += float3(0.0f, 1.0f, 0.0f) * saturate(1.0f - abs((t - 0.50f) * 4.0f));
    c += float3(1.0f, 1.0f, 0.0f) * saturate(1.0f - abs((t - 0.75f) * 4.0f));
    c += float3(1.0f, 0.0f, 0.0f) * saturate(1.0f - abs((t - 1.00f) * 4.0f));
    return c;
}

float4 main(VSOut i) : SV_Target {
    int    mode = (int)DebugParams.x;
    float3 N    = normalize(i.nrm);
    float3 color;

    if (mode == 0) {
        float2 inv = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
        float3 irr = DDGI_SampleProbe(IrradAtlas, LinearClamp, (int2)i.irrTile,
                                      (int)AtlasParams.x, inv, N);
        color = irr / (irr + 1.0f);
    } else if (mode == 1) {
        float2 inv = float2(1.0f / DistAtlasParams.y, 1.0f / DistAtlasParams.z);
        float2 md  = DDGI_SampleProbeRG(DistAtlas, LinearClamp, (int2)i.distTile,
                                        (int)DistAtlasParams.x, inv, N);
        color = Heat(md.x / max(DistAtlasParams.w, 1e-3f));
    } else if (mode == 2) {
        color = Heat(i.extra.x);
    } else if (mode == 3) {
        float w = i.extra.w; 
        if (w < 0.0f)        color = float3(0.10f, 0.25f, 1.0f);  
        else if (w > 0.20f)  color = float3(0.10f, 0.90f, 1.0f);  
        else                 color = float3(1.0f, 0.55f, 0.0f);   
    } else {
       float w = i.extra.w;
        if (w < 0.0f) color = float3(0.18f, 0.18f, 0.22f);           
        else          color = Heat(saturate(w / max(DebugParams.w, 1e-3f))); 
    }
    
    float shade = 0.55f + 0.45f * saturate(dot(N, normalize(float3(0.4f, 0.8f, 0.35f))));
    return float4(color * shade, 1.0f);
}
