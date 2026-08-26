#include "../MaterialCB.hlsli"

cbuffer SelectionMaskParams : register(b0) {
    uint AlphaMode; // 1 = masked, 2 = blend
    uint3 _pad;
};

Texture2D AlbedoMap          : register(t0);
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float main(PSInput input) : SV_TARGET {
    const float SampledAlpha = HasAlbedoMap
        ? AlbedoMap.Sample(MaterialSampler, input.uv).a : 1.0f;
    if (AlphaMode == 1)
        MaterialAlphaClip(SampledAlpha);
    else
        clip(SampledAlpha * BaseColorFactor.a - 0.02f);
    return 1.0f;
}
