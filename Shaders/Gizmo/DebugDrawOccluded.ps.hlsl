#include "../Common/DepthConfig.hlsli"

// Variante depth-testada do DebugDraw: compara o fragmento com o depth da CENA (SRV) e
// descarta atras da geometria — teste manual no PS porque o debug desenha no backbuffer
// pos-tonemap (res nativa, sem DSV) e o depth da cena vive em res de RENDER (FSR/SSAA);
// o sample por UV normalizado absorve a diferenca de resolucao. Usado pros volumes de luz
// do editor (esfera/cone), que "param" no chao em vez de atravessar; gizmo e markers ficam
// no caminho sem teste (sempre visiveis).
cbuffer DebugDrawCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 Params; // xy = 1/backbuffer, z = bias de depth (NDC), w = -
};

Texture2D<float> SceneDepth : register(t0);
SamplerState     PointClamp : register(s0);

struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

float4 main(VSOutput input) : SV_TARGET {
    float2 uv    = input.pos.xy * Params.xy;
    float  scene = SceneDepth.SampleLevel(PointClamp, uv, 0.0f);
    float  frag  = input.pos.z; // depth pos-viewport [0,1], mesmo espaco do SceneDepth

    // Visivel se o fragmento esta mais perto que a cena (bias evita serrilhado de linha
    // encostada na superficie). Reverse-Z: mais perto = MAIOR.
#if SMILE_REVERSE_Z
    if (frag + Params.z < scene) discard;
#else
    if (frag - Params.z > scene) discard;
#endif

    return float4(input.color, 1.0f);
}
