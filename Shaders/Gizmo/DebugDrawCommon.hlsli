#ifndef SMILE_DEBUGDRAW_COMMON_HLSLI
#define SMILE_DEBUGDRAW_COMMON_HLSLI

#include "../Common/DepthConfig.hlsli"

// Estado compartilhado do DebugDraw. b0 e por FRAME; b1 e por DRAW (root constants), porque
// Scene e XRay usam o MESMO PSO e diferem so no quanto a parte oculta sobrevive.
cbuffer DebugDrawCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 Params;   // xy = 1/backbuffer, z = bias de depth (NDC), w = -
    float4 CamRight; // xyz = eixo X da camera (mundo)
    float4 CamUp;    // xyz = eixo Y da camera (mundo)
    float4 Screen;   // xy = backbuffer em pixels, zw = -
};

cbuffer DebugDrawDrawCB : register(b1) {
    // Quanto do alpha sobrevive atras da geometria: 0 = Scene (some), >0 = XRay (translucido).
    float OccludedAlpha;
    float3 _DrawPad;
};

Texture2D<float> SceneDepth : register(t0);
SamplerState     PointClamp : register(s0);

// Fragmento esta atras da geometria da cena? O teste e manual porque o debug desenha no
// backbuffer pos-tonemap (res nativa, sem DSV) e o depth da cena vive em res de RENDER
// (FSR/SSAA); o sample por UV normalizado absorve a diferenca de resolucao.
bool SmileDebugOccluded(float4 svPos) {
    float2 uv    = svPos.xy * Params.xy;
    float  scene = SceneDepth.SampleLevel(PointClamp, uv, 0.0f);
    float  frag  = svPos.z; // depth pos-viewport [0,1], mesmo espaco do SceneDepth
    // Bias evita o serrilhado de linha encostada na superficie. Reverse-Z: mais perto = MAIOR.
#if SMILE_REVERSE_Z
    return (frag + Params.z) < scene;
#else
    return (frag - Params.z) > scene;
#endif
}

// Aplica o modo de depth ao alpha. Devolve false quando o fragmento nao deve sair.
bool SmileDebugApplyDepth(float4 svPos, inout float alpha) {
    if (SmileDebugOccluded(svPos)) alpha *= OccludedAlpha;
    return alpha > 0.002f;
}

#endif // SMILE_DEBUGDRAW_COMMON_HLSLI
