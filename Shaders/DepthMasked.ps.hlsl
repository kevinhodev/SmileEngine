// Variante masked/two-sided do depth pre-pass SEM normal (modo depth-only): so o
// alpha-clip, nenhuma escrita de cor. Ver DepthNormalMasked.ps.hlsl.

#include "MaterialCB.hlsli"

// No caminho bindless o AlbedoMap vem do MaterialCB.hlsli como global estatico.
#ifndef SMILE_MATERIAL_BINDLESS
Texture2D    AlbedoMap       : register(t0);
#endif
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

void main(PSInput input) {
    SMILE_MATERIAL_LOAD();
    if (AlphaTest) {
        float4 a = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                : float4(1.0f, 1.0f, 1.0f, 1.0f);
        MaterialAlphaClip(a.a);
    }
}
