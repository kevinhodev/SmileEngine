#include "TerrainCommon.hlsli"

// VS da vista principal (z-prepass e G-buffer usam o MESMO shader — depth EQUAL bate
// bit a bit). Terreno e estatico: motion vector reduz ao termo de camera.

struct VSInput {
    float2 uv    : TEXCOORD0;
    float4 morph : TEXCOORD1; // pesos baricentricos por quadrante (pow 0.3, ver FTerrain)
};

struct VSOutput {
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

VSOutput main(VSInput input) {
    VSOutput o;
    const float3 world = TerrainLocalToWorld(TerrainVertexLocal(input.uv, input.morph));
    o.pos      = mul(float4(world, 1.0f), TerrainViewProj);
    o.worldPos = world;
    o.curClip  = mul(float4(world, 1.0f), TerrainViewProjNoJitter);
    o.prevClip = mul(float4(world, 1.0f), TerrainPrevViewProj);
    return o;
}
