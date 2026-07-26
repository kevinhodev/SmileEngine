#include "TerrainCommon.hlsli"

// Prepass depth+normal do terreno (GTAO le o NormalBuffer): mesma normal por pixel do
// geometry pass, no formato do DepthNormal.ps (n * 0.5 + 0.5 em R10G10B10A2).

struct PSInput {
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0 {
    const float3 n = TerrainNormal(input.worldPos.xz);
    return float4(n * 0.5f + 0.5f, 1.0f);
}
