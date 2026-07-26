// Variante masked/two-sided do depth pre-pass com normal (GTAO F4, estilo UE masked
// prepass): folhagem/grade/fio entram no depth+normal do prepass com alpha-clip, entao
// o GTAO ve essa geometria (fim dos speckles brancos = AO do fundo) e ela projeta AO.
// Mesmo clip do GBuffer.ps/ShadowDepth.ps; cull NONE no PSO (cards de folhagem).

#include "MaterialCB.hlsli"

Texture2D    AlbedoMap       : register(t0);
SamplerState MaterialSampler : register(s0);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0 {
    if (AlphaTest) {
        float4 a = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                : float4(1.0f, 1.0f, 1.0f, 1.0f);
        MaterialAlphaClip(a.a);
    }
    float3 n = normalize(input.worldNormal);
    return float4(n * 0.5f + 0.5f, 1.0f);
}
