#include "GBuffer.hlsli"

cbuffer DebugCB : register(b0) {
    uint Mode;
    uint3 _pad;
};

Texture2D GBufferA  : register(t0); 
Texture2D GBufferB  : register(t1); 
Texture2D GBufferC  : register(t2);
Texture2D Velocity  : register(t3); 

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target {
    int2 px = int2(input.pos.xy);
    float4 a = GBufferA.Load(int3(px, 0));
    float4 b = GBufferB.Load(int3(px, 0));
    float4 c = GBufferC.Load(int3(px, 0));
    GBufferData g = DecodeGBuffer(a, b, c);

    float3 outColor;
    switch (Mode) {
        case 1:  outColor = g.BaseColor;                       break; 
        case 2:  outColor = g.WorldNormal * 0.5f + 0.5f;       break; 
        case 3:  outColor = g.Roughness.xxx;                   break;
        case 4:  outColor = g.Metallic.xxx;                    break;
        case 5:  outColor = g.Emissive;                        break;
        case 6:  outColor = g.AO.xxx;                          break;
        case 7: { 
            outColor = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE)
                     ? float3(0.1f, 0.9f, 0.2f) : float3(0.5f, 0.5f, 0.5f);
            break;
        }
        case 8: { 
            float2 vUV;
            Velocity.GetDimensions(vUV.x, vUV.y);
            float2 velPx = Velocity.Load(int3(px, 0)).rg * vUV; 
            outColor = float3(0.5f, 0.5f, 0.5f) + float3(velPx * 0.05f, 0.0f);
            break;
        }
        default: outColor = g.BaseColor;                       break;
    }
    return float4(outColor, 1.0f);
}
