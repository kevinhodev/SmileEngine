// Variante masked/two-sided do depth pre-pass SEM normal (modo depth-only): so o
// alpha-clip, nenhuma escrita de cor. Ver DepthNormalMasked.ps.hlsl.

#include "MaterialCB.hlsli"

Texture2D    AlbedoMap       : register(t0);
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

void main(PSInput input) {
    if (AlphaTest) {
        float4 a = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                : float4(1.0f, 1.0f, 1.0f, 1.0f);
        MaterialAlphaClip(a.a);
    }
}
