// ReSTIR DI -> instancia DIRETA do NRD/RELAX. O resolve espacial ja separa os lobos
// modulados; aqui removemos a cor-base/F0 antes da acumulacao e entregamos hit distance cru.

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"
#include "../GBuffer.hlsli"

cbuffer ReSTIRDICB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params;
    float4 Sampling;
    float4 Reuse;
    float4 TemporalPolicy;
    float4 RayEpsA;
    float4 RayEpsB;
    row_major float4x4 View;
};

Texture2D<float4> RawDiffuse  : register(t0);
Texture2D<float4> RawSpecular : register(t1);
Texture2D<float4> GBufferA    : register(t2);
Texture2D<float4> GBufferB    : register(t3);
Texture2D<float4> GBufferC    : register(t4);
Texture2D<float>  Depth       : register(t5);
Texture2D<float2> Velocity    : register(t6);
Texture2D<float4> ShadowMotion : register(t7);

RWTexture2D<float>  OutViewZ       : register(u0);
RWTexture2D<float4> OutNormalRough : register(u1);
RWTexture2D<float2> OutMv          : register(u2);
RWTexture2D<float4> OutDiffRadHit  : register(u3);
RWTexture2D<float4> OutSpecRadHit  : register(u4);

float3 SafeDemodulate(float3 signal, float3 modulation) {
    // O ramo preserva a energia na remodulacao; um piso no divisor escureceria albedos baixos.
    return float3(modulation.x > 1.0e-4f ? signal.x / modulation.x : 0.0f,
                  modulation.y > 1.0e-4f ? signal.y / modulation.y : 0.0f,
                  modulation.z > 1.0e-4f ? signal.z / modulation.z : 0.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const float deviceZ = Depth.Load(int3(px, 0));
    if (deviceZ <= 0.0f) {
        OutViewZ[px] = 1.0e8f;
        OutNormalRough[px] = NRD_FrontEnd_PackNormalAndRoughness(
            float3(0.0f, 1.0f, 0.0f), 1.0f, 0.0f);
        OutMv[px] = Velocity.Load(int3(px, 0));
        OutDiffRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(0.0f, 0.0f, true);
        OutSpecRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(0.0f, 0.0f, true);
        return;
    }

    const GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                        GBufferB.Load(int3(px, 0)),
                                        GBufferC.Load(int3(px, 0)));
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 worldPos = wh.xyz / wh.w;
    const float viewZ = abs(mul(float4(worldPos, 1.0f), View).z);

    const float4 rawDiffuse = RawDiffuse.Load(int3(px, 0));
    const float4 rawSpecular = RawSpecular.Load(int3(px, 0));
    const float3 diffuseAlbedo = g.BaseColor * (1.0f - g.Metallic);
    const float3 f0 = lerp(0.04f.xxx, g.BaseColor, g.Metallic);
    const float kRelaxMaxRadiance = 250.0f;
    const float3 diffuse = min(SafeDemodulate(rawDiffuse.rgb, diffuseAlbedo),
                               kRelaxMaxRadiance);
    const float3 specular = min(SafeDemodulate(rawSpecular.rgb, f0),
                                kRelaxMaxRadiance);

    OutViewZ[px] = viewZ;
    OutNormalRough[px] = NRD_FrontEnd_PackNormalAndRoughness(
        g.WorldNormal, max(g.Roughness, 0.04f), 0.0f);
    const float2 baseMotion = Velocity.Load(int3(px, 0));
    const float4 shadowMotion = ShadowMotion.Load(int3(px, 0));
    OutMv[px] = lerp(baseMotion, shadowMotion.xy,
                     saturate(shadowMotion.z) * saturate(shadowMotion.w));
    OutDiffRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(
        diffuse, max(rawDiffuse.a, 0.0f), true);
    OutSpecRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(
        specular, max(rawSpecular.a, 0.0f), true);
}
