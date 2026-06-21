// ReSTIR GI -> NRD: empacota os inputs comuns + o sinal DIFUSO do REBLUR_DIFFUSE_SPECULAR (Fase C0).
// O sinal ESPECULAR (reflexao) e empacotado por ReflectionNrdPack; MV/NormalRough/ViewZ aqui sao
// compartilhados pelos dois sinais.
//   IN_VIEWZ              = view.z linear (positivo); ceu = grande (> denoisingRange) p/ ser ignorado
//   IN_NORMAL_ROUGHNESS  = NRD_FrontEnd_PackNormalAndRoughness (R10G10B10A2 — casa com o decode do NRD)
//   IN_MV                = velocity (curUV - prevUV); o NRD usa motionVectorScale=(-1,-1,0) p/ inverter
//   IN_DIFF_RADIANCE_HITDIST = REBLUR_FrontEnd_PackRadianceAndNormHitDist (hitDist NORMALIZADO via
//                              REBLUR_FrontEnd_GetNormHitDist com NrdHitDistParams = {A,B,C} = ReblurSettings)
//
// Inclui o NRD.hlsli (via -I D:/Engines/NRD/Shaders) p/ o encode normal/roughness bater EXATAMENTE
// com o decode interno do NRD. Os macros de encoding sao fixados aqui (normal=R10G10B10A2, rough=linear).

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"
#include "DDGICommon.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;            // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 ShadeParams;
    float4 ReuseParams;
    float4 SpatialParams;
    row_major float4x4 View;        // anexado p/ o pack: worldPos -> view.z (IN_VIEWZ)
    float4 NrdHitDistParams;        // xyz = ReblurHitDistanceParameters {A,B,C} (igual ao C++)
};

Texture2D<float4> GITex    : register(t0); // rgb = gi (radiancia), a = hitDist
Texture2D<float4> GBuffer  : register(t1); // GBufferB: octNormal+rough+metal
Texture2D<float>  Depth    : register(t2);
Texture2D<float2> Velocity : register(t3); // curUV - prevUV

RWTexture2D<float>  OutViewZ       : register(u0);
RWTexture2D<float4> OutNormalRough : register(u1);
RWTexture2D<float2> OutMv          : register(u2);
RWTexture2D<float4> OutDiffRadHit  : register(u3);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;

    float4 gb     = GBuffer.Load(int3(px, 0));
    float3 N      = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    float  rough  = max(gb.b, 0.04f);

    float viewZ;
    if (deviceZ <= 0.0f) {
        viewZ = 1.0e8f; // ceu: > denoisingRange -> ignorado pelo NRD
    } else {
        float2 uv  = (px + 0.5f) * ScreenParams.zw;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
        float3 worldPos = wH.xyz / wH.w;
        float4 viewPos  = mul(float4(worldPos, 1.0f), View);
        viewZ = abs(viewPos.z); // sinal a ajustar no teste GPU se houver ghosting
    }

    float4 gi = GITex.Load(int3(px, 0));

    // REBLUR exige hit distance NORMALIZADO ([0;1]) via a mesma curva (NrdHitDistParams) configurada
    // no ReblurSettings do driver. Sinal DIFUSO -> roughness = 1.0 (lobe difuso = espalhamento maximo);
    // usar a roughness da superficie aqui colapsaria a normalizacao.
    float normHitDist = REBLUR_FrontEnd_GetNormHitDist(gi.a, viewZ, NrdHitDistParams.xyz, 1.0f);

    OutViewZ[px]       = viewZ;
    OutNormalRough[px] = NRD_FrontEnd_PackNormalAndRoughness(N, rough, 0.0f);
    OutMv[px]          = Velocity.Load(int3(px, 0)).rg;
    OutDiffRadHit[px]  = REBLUR_FrontEnd_PackRadianceAndNormHitDist(gi.rgb, normHitDist, true);
}
