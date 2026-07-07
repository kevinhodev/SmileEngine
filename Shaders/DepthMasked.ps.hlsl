// Variante masked/two-sided do depth pre-pass SEM normal (modo depth-only): so o
// alpha-clip, nenhuma escrita de cor. Ver DepthNormalMasked.ps.hlsl.

cbuffer MaterialCB : register(b1) {
    float4 BaseColorFactor;
    float  MetallicFactor;
    float  RoughnessFactor;
    float  AOStrength;
    float  EmissiveStrength;
    float4 EmissiveFactor;
    uint   HasAlbedoMap;
    uint   HasNormalMap;
    uint   HasMetallicRoughnessMap;
    uint   HasAOMap;
    uint   HasEmissiveMap;
    float  NormalStrength;
    uint   NormalFlipY;

    uint   HasHeightMap;
    float  HeightScale;
    float  ParallaxMinSteps;
    float  ParallaxMaxSteps;
    uint   ParallaxSelfShadow;
    float  ParallaxShadowSteps;
    float  ParallaxFadeStart;
    float  ParallaxFadeRange;
    uint   ParallaxRefine;
    uint   ParallaxRefineSteps;

    uint   HasMetalnessMap;
    uint   HasRoughnessMap;

    uint   SpecularPacking;
    uint   AlphaTest;
    float  AlphaCutoff;
    uint   NormalReconstructZ;
};

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
        clip(a.a * BaseColorFactor.a - AlphaCutoff);
    }
}
