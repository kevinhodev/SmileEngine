// Reflexao -> NRD: empacota o sinal ESPECULAR do RELAX_DIFFUSE_SPECULAR (IN_SPEC_RADIANCE_HITDIST).
// Os inputs comuns (MV/NormalRough/ViewZ) sao escritos pelo pack do ReSTIR GI (ReSTIRNrdPack); aqui
// so produzimos o radiance+hitDist especular a partir do Resolved da reflexao (radiancia DEMODULADA;
// a modulacao F0*BRDF fica no composite, que e o ideal p/ o denoiser).
//   Resolved.rgb = radiancia crua do reflexo (full-res)
//   Resolved.a   = hit distance (full-res, vindo do resolve)
// RELAX consome hitDist CRU (unidades de mundo) — sem a normalizacao que o REBLUR exigia. O hitDist
// especular alimenta o tracking de reflexo (motion vector virtual), entao vai "as is".

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"               // via -I D:/Engines/NRD/Shaders (igual ao ReSTIRNrdPack)

// Layout IDENTICO ao ReflectionConstants (C++). So lemos o prefixo + View (no fim).
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
    float4 PrevCameraPos;
    float4 TemporalParams;
    float4 DebugParams;
    row_major float4x4 View;         // worldPos -> view.z (IN_VIEWZ); anexado p/ o pack
};

// t1/t2 seguem declarados p/ casar a tabela de 3 SRVs do passe: com o RELAX nao ha mais
// normalizacao de hitDist, logo nem roughness (GBuffer) nem viewZ (Depth) sao lidos aqui —
// o IN_VIEWZ (inclusive o 1e8 do ceu, que tira o pixel do denoisingRange) vem do ReSTIRNrdPack.
Texture2D<float4> Resolved : register(t0); // rgb = radiancia crua, a = hitDist
Texture2D<float4> GBuffer  : register(t1);
Texture2D<float>  Depth    : register(t2);

RWTexture2D<float4> OutSpecRadHit : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float4 refl = Resolved.Load(int3(px, 0));

    // Faixa HDR sa do RELAX: ele rastreia SEGUNDO MOMENTO do sinal, entao x^2 precisa caber em FP16
    // (o REBLUR nao rastreava e tolerava faixa maior). O resolve limita o PESO do vizinho mas nao a
    // radiancia, e um reflexo de espelho do disco solar/ceu passa fácil dos 250 recomendados pelo
    // NRD — acima disso a variancia satura e a saida vira mancha. Clamp por CANAL, sem renormalizar
    // (nao vale distorcer a cor de um pixel que ja e um outlier).
    const float kRelaxMaxRadiance = 250.0f;
    float3 radiance = min(refl.rgb, kRelaxMaxRadiance);

    OutSpecRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(radiance, refl.a, true);
}
