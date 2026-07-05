// ReSTIR PT -> NRD (F4): empacota os inputs do REBLUR_DIFFUSE_SPECULAR. Com o PT ativo o
// ReSTIR GI e as reflexoes estao OFF, entao ESTE pack preenche TUDO: os comuns (ViewZ /
// NormalRoughness / MV) + os dois sinais (diff e spec), separados pelo lobe do 1o segmento
// (PTGiDiff = parcela difusa; spec = combinado - difuso).
//
// DEMODULACAO (licao do prototipo P4: denoisar com albedo embutido borra a textura):
//   diff -> / max(albedoDiff, 0.01)           (albedoDiff = baseColor * (1-metallic))
//   spec -> / max(PT_EnvBRDF(F0,NoV,rough), 0.01)  (fator analitico; o composite REmodula pelo
//           MESMO fator, entao o erro da aproximacao cancela exato)
// hitDist = |x2-x1| da amostra selecionada (PTOut.a), normalizado via REBLUR_FrontEnd_GetNormHitDist
// com os MESMOS {A,B,C} do driver (NrdHitDistParams); difuso usa roughness=1 (lobe difuso =
// espalhamento maximo), especular usa a roughness da superficie — igual aos packs do GI/reflexao.

#define NRD_NORMAL_ENCODING    2 // R10G10B10A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1 // LINEAR
#include "NRD.hlsli"               // via -I D:/Engines/NRD/Shaders
#include "../GBuffer.hlsli"
#include "PTCommon.hlsli"

Texture2D<float4> GiTex    : register(t0); // rgb = gi combinado (C_sel*W), a = hitDist
Texture2D<float4> GiDiff   : register(t1); // rgb = parcela difusa do gi
Texture2D<float4> GBufferA : register(t2); // baseColor + ao
Texture2D<float4> GBufferB : register(t3); // octNormal + rough + metal
Texture2D<float>  Depth    : register(t4);
Texture2D<float2> Velocity : register(t5); // curUV - prevUV

RWTexture2D<float>  OutViewZ       : register(u0);
RWTexture2D<float4> OutNormalRough : register(u1);
RWTexture2D<float2> OutMv          : register(u2);
RWTexture2D<float4> OutDiffRadHit  : register(u3);
RWTexture2D<float4> OutSpecRadHit  : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    float4 gbA = GBufferA.Load(int3(px, 0));
    float4 gbB = GBufferB.Load(int3(px, 0));
    float3 N     = GBuffer_OctDecode(gbB.rg);
    float  rough = max(gbB.b, 0.04f);
    float  metal = gbB.a;

    float viewZ = 1.0e8f; // ceu: > denoisingRange -> ignorado pelo NRD
    float nov   = 0.0f;
    if (deviceZ > 0.0f) {
        float3 worldPos = PT_WorldPosFromDepth(px, deviceZ);
        float4 viewPos  = mul(float4(worldPos, 1.0f), View);
        viewZ = abs(viewPos.z);
        nov   = saturate(dot(N, normalize(CameraPos.xyz - worldPos)));
    }

    float4 gi     = GiTex.Load(int3(px, 0));
    float3 giDiff = GiDiff.Load(int3(px, 0)).rgb;
    float3 giSpec = max(gi.rgb - giDiff, 0.0f);

    float3 albedoDiff = max(gbA.rgb * (1.0f - metal), 0.01f);
    float3 f0         = lerp(float3(0.04f, 0.04f, 0.04f), gbA.rgb, metal);
    float3 envBrdf    = max(PT_EnvBRDF(f0, nov, rough), 0.01f);

    float normHitDiff = REBLUR_FrontEnd_GetNormHitDist(gi.a, viewZ, NrdHitDistParams.xyz, 1.0f);
    float normHitSpec = REBLUR_FrontEnd_GetNormHitDist(gi.a, viewZ, NrdHitDistParams.xyz, rough);

    OutViewZ[px]       = viewZ;
    OutNormalRough[px] = NRD_FrontEnd_PackNormalAndRoughness(N, rough, 0.0f);
    OutMv[px]          = Velocity.Load(int3(px, 0)).rg;
    OutDiffRadHit[px]  = REBLUR_FrontEnd_PackRadianceAndNormHitDist(giDiff / albedoDiff, normHitDiff, true);
    OutSpecRadHit[px]  = REBLUR_FrontEnd_PackRadianceAndNormHitDist(giSpec / envBrdf, normHitSpec, true);
}
