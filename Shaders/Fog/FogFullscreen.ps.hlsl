#include "FogCommon.hlsli"

Texture2D<float> SceneDepth : register(t0);
float FogSampleDepth(int2 px) { return SceneDepth.Load(int3(px, 0)); }

#include "FogApply.hlsli"

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    return FogApplyMain(svpos.xy);
}
