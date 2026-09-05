#include "../MaterialCB.hlsli"

// No caminho bindless o AlbedoMap vem do MaterialCB.hlsli como global estatico.
#ifndef SMILE_MATERIAL_BINDLESS
Texture2D    AlbedoMap       : register(t0);
#endif
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

void main(PSInput input) {
    SMILE_MATERIAL_LOAD();
    if (AlphaTest) {
        float4 a = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                : float4(1.0f, 1.0f, 1.0f, 1.0f);
        MaterialAlphaClip(a.a);
    }
}
