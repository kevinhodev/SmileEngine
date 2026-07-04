// ReSTIR PT — Pass A (F0: megakernel DI+GI unificado, SEM reservoir).
//
// Por pixel: reconstroi x1 do G-buffer/depth (o raio primario ja foi pago pelo raster), path-traca
// com NEE do sol em cada vertice (direto e indireto no MESMO estimador — Enhanced §6.1), termina
// no DDGI (radiance cache) e escreve a radiancia COMPLETA (emissivo de x1 incluido) em PTOut.
// O DeferredLighting le PTOut como passthrough quando ReflectionParams.w == 3.
// F1 adiciona o reservoir temporal; F2 o hybrid shift; F3 o passe espacial.
//
// Debug (DebugParams.x == 1): media progressiva em Accum com a camera parada — ground truth
// para validar as fases seguintes.

#include "../GI/DDGICommon.hlsli"
#include "../GBuffer.hlsli"
#include "PTCommon.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
StructuredBuffer<DDGIVertex>    Vertices   : register(t4);
Buffer<uint>                    Indices    : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBufferA   : register(t7);
Texture2D<float4>               GBufferB   : register(t8);
Texture2D<float4>               GBufferC   : register(t9);
Texture2D<float2>               Velocity   : register(t10); // F1: reprojecao temporal

RWTexture2D<float4>             PTOut      : register(u0); // rgb = radiancia full, a = |x2-x1|
RWTexture2D<float4>             Accum      : register(u1); // debug: media progressiva

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" // ShadeSky, AlphaTestPass, SMILE_RT_PROCEED, DDGI sampling
#include "PTRng.hlsli"
#include "PTRayUtil.hlsli"
#include "PTMaterial.hlsli"
#include "PTPathTrace.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        PTOut[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ceu: o deferred descarta antes do passthrough
        return;
    }

    // Superficie primaria x1 direto do G-buffer (mesmo decode do deferred).
    GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                  GBufferB.Load(int3(px, 0)),
                                  GBufferC.Load(int3(px, 0)));
    FPTSurface s;
    s.Pos       = PT_WorldPosFromDepth(px, deviceZ);
    s.N         = g.WorldNormal;
    s.Albedo    = g.BaseColor;
    s.Emissive  = g.Emissive;
    s.Roughness = max(g.Roughness, 0.04f);
    s.Metallic  = g.Metallic;
    s.Foliage   = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE);

    float3 V    = normalize(CameraPos.xyz - s.Pos);
    uint   seed = PTPathSeed(px, (uint)TraceParams.x);

    float firstDist;
    float3 L = g.Emissive + PT_PathRadiance(s, V, seed,
                                            (uint)PathParams.x, (uint)PathParams.y,
                                            /*enableRR=*/true, firstDist);

    // Firefly clamp: limita outliers de luminancia (o denoiser/reuso nao tira bem ponto isolado).
    {
        float lum    = PT_Luminance(L);
        float maxLum = PathParams.z;
        if (maxLum > 0.0f && lum > maxLum) L *= maxLum / lum;
    }

    // Debug: acumulacao progressiva (media incremental) — ground truth com a camera parada.
    if ((uint)DebugParams.x == PT_DEBUG_ACCUM) {
        float  n    = DebugParams.y;
        float3 prev = (n > 0.5f) ? Accum[px].rgb : float3(0.0f, 0.0f, 0.0f);
        float3 avg  = prev + (L - prev) / (n + 1.0f);
        Accum[px] = float4(avg, 1.0f);
        L = avg;
    }

    PTOut[px] = float4(L, firstDist);
}
