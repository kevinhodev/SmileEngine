#include "TerrainCommon.hlsli"

// VS das cascatas do CSM: mesma geometria (mesmo LOD + morph da vista) projetada pela
// matriz da cascata — depth da sombra casa com o depth da vista, menos acne.

cbuffer ShadowCascadeCB : register(b2) {
    row_major float4x4 LightViewProj;
};

struct VSInput {
    float2 uv    : TEXCOORD0;
    float4 morph : TEXCOORD1;
};

float4 main(VSInput input) : SV_POSITION {
    const float3 world = TerrainLocalToWorld(TerrainVertexLocal(input.uv, input.morph));
    return mul(float4(world, 1.0f), LightViewProj);
}
