#include "../MaterialCB.hlsli"

Texture2D    AlbedoMap       : register(t0);
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

void main(PSInput input) {
    if (AlphaTest) {
        float4 a = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                : float4(1.0f, 1.0f, 1.0f, 1.0f);
        MaterialAlphaClip(a.a);
    }
}
