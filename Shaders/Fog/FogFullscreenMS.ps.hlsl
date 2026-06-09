// FogFullscreenMS.ps.hlsl — deferred fog apply, MSAA scene depth (sample 0).
#include "FogCommon.hlsli"

Texture2DMS<float> SceneDepth : register(t0);
float FogSampleDepth(int2 px) { return SceneDepth.Load(px, 0); }

#include "FogApply.hlsli"

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    return FogApplyMain(svpos.xy);
}
