#ifndef SMILE_MATERIALCB_HLSLI
#define SMILE_MATERIALCB_HLSLI

// Constant buffer do material (b1) — ESPELHO de Smile::MaterialConstants (Material.h).
//
// Antes este bloco era transcrito a mao em 6 shaders (GBuffer, ForwardBlend, DepthMasked,
// DepthNormalMasked, Shadow/ShadowDepth, Preview/MaterialPreview). As copias estavam
// consistentes, mas nada garantia isso: cbuffer e contrato de OFFSET, nao de nome, e ninguem
// valida C++ x HLSL. Inserir um campo no meio de MaterialConstants deslocava tudo em 6 arquivos
// silenciosamente — e dois deles ja divergiam (paravam em NormalReconstructZ, sem ShadingModel
// nem SubsurfaceColor; funcionava por acidente, porque so liam campos anteriores).
//
// REGRA: mexeu em MaterialConstants, mexe AQUI, e so aqui. A ordem tem que bater campo a campo
// com o struct C++. As duas regras de empacotamento que importam:
//   - HLSL nao deixa um membro atravessar fronteira de 16B, entao float4 sempre cai em multiplo
//     de 16. Hoje BaseColorFactor@0, EmissiveFactor@32 e SubsurfaceColor@144 ja caem alinhados
//     naturalmente — nao ha padding implicito divergindo do C++.
//   - o total (160B usados) cabe no CBV de 256B que o FMaterial::Bind liga em b1.
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

    uint   ShadingModel;
    float4 SubsurfaceColor;
};

// Recorte do cutout. Era a MESMA expressao escrita cinco vezes (GBuffer.ps, ShadowDepth.ps,
// DepthMasked.ps, DepthNormalMasked.ps, MaterialPreview.ps); o RT tem a sua propria copia em
// GI/RTAlphaTest.hlsli, que le do InstanceGeo em vez do cbuffer e por isso nao da p/ compartilhar
// — mas as duas TEM que casar (o alfa da textura multiplicado pela opacidade do material), senao
// a silhueta do raster e a dos raios divergem.
//
// O chamador decide de QUAL amostra vem o alfa: o G-buffer, por exemplo, amostra o alfa sem o
// MipBias negativo do FSR de proposito (silhueta estavel no upscale), enquanto o RGB leva o bias.
void MaterialAlphaClip(float sampledAlpha) {
    clip(sampledAlpha * BaseColorFactor.a - AlphaCutoff);
}

#endif
