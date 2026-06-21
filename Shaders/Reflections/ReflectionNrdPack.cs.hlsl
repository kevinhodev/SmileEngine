// Reflexao -> NRD: empacota o sinal ESPECULAR do REBLUR_DIFFUSE_SPECULAR (IN_SPEC_RADIANCE_HITDIST).
// Os inputs comuns (MV/NormalRough/ViewZ) sao escritos pelo pack do ReSTIR GI (ReSTIRNrdPack); aqui
// so produzimos o radiance+hitDist especular a partir do Resolved da reflexao (radiancia DEMODULADA;
// a modulacao F0*BRDF fica no composite, que e o ideal p/ o denoiser).
//   Resolved.rgb = radiancia crua do reflexo (full-res)
//   Resolved.a   = hit distance (full-res, vindo do resolve)
// REBLUR exige hitDist NORMALIZADO via REBLUR_FrontEnd_GetNormHitDist com os MESMOS {A,B,C} do driver.

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"               // via -I D:/Engines/NRD/Shaders (igual ao ReSTIRNrdPack)

// Layout IDENTICO ao ReflectionConstants (C++). So lemos o prefixo + View + NrdSpecParams (no fim).
cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;     // x=W, y=H, z=1/W, w=1/H
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
    row_major float4x4 View;         // worldPos -> view.z (IN_VIEWZ); anexado p/ o pack
    float4 NrdSpecParams;            // xyz = ReblurHitDistanceParameters {A,B,C} (igual ao driver)
};

Texture2D<float4> Resolved : register(t0); // rgb = radiancia crua, a = hitDist
Texture2D<float4> GBuffer  : register(t1); // GBufferB: octNormal+rough+metal
Texture2D<float>  Depth    : register(t2);

RWTexture2D<float4> OutSpecRadHit : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    float  rough  = max(GBuffer.Load(int3(px, 0)).b, 0.04f);

    float viewZ;
    if (deviceZ <= 0.0f) {
        viewZ = 1.0e8f; // ceu / sem reflexo -> ignorado pelo NRD
    } else {
        float2 uv  = (px + 0.5f) * ScreenParams.zw;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
        float3 worldPos = wH.xyz / wH.w;
        float4 viewPos  = mul(float4(worldPos, 1.0f), View);
        viewZ = abs(viewPos.z);
    }

    float4 refl = Resolved.Load(int3(px, 0));
    float  normHitDist = REBLUR_FrontEnd_GetNormHitDist(refl.a, viewZ, NrdSpecParams.xyz, rough);

    OutSpecRadHit[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(refl.rgb, normHitDist, true);
}
