// ReSTIR PT — composite pos-NRD (F4): remonta a radiancia final a partir das saidas denoisadas
// do REBLUR (OUT_DIFF/OUT_SPEC, lidas como UAV — evita transicionar recursos cujo estado e
// rastreado pelo driver do NRD; o caller poe um UAV barrier antes) + PTLit (direto + emissivo +
// Cemit, que NUNCA passam pelo denoiser — licao do P4: emissivo denoisado vira mancha).
//
//   final = PTLit + albedoDiff * diff_denoised + PT_EnvBRDF(F0,NoV,rough) * spec_denoised
//
// REmodula pelos MESMOS fatores que o pack dividiu (o erro do env-BRDF analitico cancela exato).
// REBLUR_BackEnd_UnpackRadianceAndNormHitDist ja desfaz o YCoCg interno do REBLUR (o deferred
// nao decodifica o caminho w==3 — quem entrega linear e este composite, via PTFinal).

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"               // via -I D:/Engines/NRD/Shaders
#include "../GBuffer.hlsli"
#include "PTCommon.hlsli"

Texture2D<float4> LitTex   : register(t0); // direto(x1) + Cemit (Pass A)
Texture2D<float4> GBufferA : register(t1); // baseColor + ao
Texture2D<float4> GBufferB : register(t2); // octNormal + rough + metal
Texture2D<float>  Depth    : register(t3);

RWTexture2D<float4> PTFinal : register(u0); // radiancia final (o deferred le via w==3)
RWTexture2D<float4> OutDiff : register(u1); // OUT_DIFF_RADIANCE_HITDIST do NRD (leitura)
RWTexture2D<float4> OutSpec : register(u2); // OUT_SPEC_RADIANCE_HITDIST do NRD (leitura)

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        PTFinal[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ceu: o deferred descarta antes
        return;
    }

    float4 gbA = GBufferA.Load(int3(px, 0));
    float4 gbB = GBufferB.Load(int3(px, 0));
    float3 N     = GBuffer_OctDecode(gbB.rg);
    float  rough = max(gbB.b, 0.04f);
    float  metal = gbB.a;

    float3 worldPos = PT_WorldPosFromDepth(px, deviceZ);
    float  nov      = saturate(dot(N, normalize(CameraPos.xyz - worldPos)));

    float3 diffD = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[px]).rgb;
    float3 specD = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutSpec[px]).rgb;

    float3 albedoDiff = max(gbA.rgb * (1.0f - metal), 0.01f);
    float3 f0         = lerp(float3(0.04f, 0.04f, 0.04f), gbA.rgb, metal);
    float3 envBrdf    = max(PT_EnvBRDF(f0, nov, rough), 0.01f);

    float3 L = LitTex.Load(int3(px, 0)).rgb + albedoDiff * diffD + envBrdf * specD;
    PTFinal[px] = float4(L, 1.0f);
}
