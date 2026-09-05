#ifndef SMILE_MATERIALCB_HLSLI
#define SMILE_MATERIALCB_HLSLI

// Material do raster, em DOIS caminhos que expoem os MESMOS NOMES.
//
//   classico (default)  — cbuffer b1 + texturas numa tabela de descritores, os dois amarrados
//                         POR DRAW (FMaterial::Bind).
//   SMILE_MATERIAL_BINDLESS — uma linha da tabela canonica (FGpuMaterialRecord) lida por indice
//                         no heap; por draw so muda uma constante de root de 4 B.
//
// Os nomes serem os mesmos e o ponto da Fase 3: o corpo dos shaders nao muda entre os dois, entao
// qualquer diferenca visual no A/B so pode vir do ACESSO AOS DADOS. Um `#define` por campo, e nao
// uma reescrita por caminho.
//
// Quem migra um shader faz duas coisas: chama `SMILE_MATERIAL_LOAD()` como primeira instrucao do
// `main` (no-op no classico) e poe as proprias declaracoes de `Texture2D` sob
// `#ifndef SMILE_MATERIAL_BINDLESS` — no caminho bindless as oito vem daqui.

#ifdef SMILE_MATERIAL_BINDLESS

#include "Scene/MaterialAccess.hlsli"

// Globais ESTATICOS, e nao locais do main: os helpers de raster (ApplyNormalMap,
// ParallaxOcclusionMapping, ParallaxFade, MaterialAlphaClip) sao funcoes livres que leem os campos
// do material. Passa-los por parametro mudaria a assinatura de todas elas e faria o A/B comparar
// dois corpos de codigo diferentes, que e exatamente o que esta fase quer evitar.
static GpuMaterialRecord SmileMaterial;

static Texture2D AlbedoMap;
static Texture2D NormalMap;
static Texture2D MetallicRoughnessMap;
static Texture2D AOMap;
static Texture2D EmissiveMap;
static Texture2D HeightMap;
static Texture2D MetalnessMap;
static Texture2D RoughnessMap;

#define SMILE_MATERIAL_LOAD()                                                                   \
    SmileMaterial        = Smile_LoadRasterMaterial();                                          \
    AlbedoMap            = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_ALBEDO)];   \
    NormalMap            = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_NORMAL)];   \
    MetallicRoughnessMap = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_MR)];       \
    AOMap                = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_AO)];       \
    EmissiveMap          = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_EMISSIVE)]; \
    HeightMap            = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_HEIGHT)];   \
    MetalnessMap         = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_METAL)];    \
    RoughnessMap         = ResourceDescriptorHeap[Smile_RasterTextureSlot(SmileMaterial, SMILE_MAT_TEX_ROUGH)]

// Campos escalares: alias direto para a linha.
#define BaseColorFactor      SmileMaterial.BaseColor
#define MetallicFactor       SmileMaterial.EmissiveFactor.w
#define RoughnessFactor      SmileMaterial.RoughnessFactor
#define AOStrength           SmileMaterial.RasterEmissive.w
#define SubsurfaceColor      SmileMaterial.SubsurfaceColor
#define AlphaCutoff          SmileMaterial.AlphaCutoff
#define NormalStrength       SmileMaterial.NormalStrength
#define HeightScale          SmileMaterial.HeightScale
#define ParallaxMinSteps     SmileMaterial.ParallaxMinSteps
#define ParallaxMaxSteps     SmileMaterial.ParallaxMaxSteps
#define ParallaxFadeStart    SmileMaterial.ParallaxFadeStart
#define ParallaxFadeRange    SmileMaterial.ParallaxFadeRange
#define ParallaxRefineSteps  SmileMaterial.ParallaxRefineSteps
#define ShadingModel         SmileMaterial.ShadingModel

// Os `uint` que viraram bits. Rendem `uint` 0/1, e nao `bool`, para que a PARIDADE de tipo com o
// cbuffer seja exata: o GBuffer.ps escreve `if (NormalFlipY == 0)`, e comparar bool com inteiro
// depende de promocao — funciona, mas por um caminho diferente do original.
#define HasAlbedoMap            ((SmileMaterial.Flags & SMILE_MAT_FLAG_ALBEDOMAP) != 0u ? 1u : 0u)
#define HasNormalMap            ((SmileMaterial.Flags & SMILE_MAT_FLAG_NORMALMAP) != 0u ? 1u : 0u)
#define HasMetallicRoughnessMap ((SmileMaterial.Flags & SMILE_MAT_FLAG_MRMAP) != 0u ? 1u : 0u)
#define HasAOMap                ((SmileMaterial.Flags & SMILE_MAT_FLAG_AOMAP) != 0u ? 1u : 0u)
#define HasEmissiveMap          ((SmileMaterial.Flags & SMILE_MAT_FLAG_EMISSIVEMAP) != 0u ? 1u : 0u)
#define HasHeightMap            ((SmileMaterial.Flags & SMILE_MAT_FLAG_HEIGHTMAP) != 0u ? 1u : 0u)
#define HasMetalnessMap         ((SmileMaterial.Flags & SMILE_MAT_FLAG_METALMAP) != 0u ? 1u : 0u)
#define HasRoughnessMap         ((SmileMaterial.Flags & SMILE_MAT_FLAG_ROUGHMAP) != 0u ? 1u : 0u)
#define SpecularPacking         ((SmileMaterial.Flags & SMILE_MAT_FLAG_SPECPACK) != 0u ? 1u : 0u)
#define AlphaTest               ((SmileMaterial.Flags & SMILE_MAT_FLAG_ALPHATEST) != 0u ? 1u : 0u)
#define NormalFlipY             ((SmileMaterial.Flags & SMILE_MAT_FLAG_NORMALFLIPY) != 0u ? 1u : 0u)
#define NormalReconstructZ      ((SmileMaterial.Flags & SMILE_MAT_FLAG_NORMALRECONZ) != 0u ? 1u : 0u)
#define ParallaxRefine          ((SmileMaterial.Flags & SMILE_MAT_FLAG_PARALLAXREFINE) != 0u ? 1u : 0u)

// O emissivo do raster e o unico campo que NAO e alias: no CBV ele vinha como fator e forca
// separados e o shader multiplicava por pixel; na linha ele ja chega multiplicado (e sem o
// RTEmissiveScale, que so vale para o mundo indireto). Um macro nomeado deixa a divergencia
// visivel no shader em vez de escondida atras de um alias que mentiria sobre o que ele contem.
#define SMILE_MATERIAL_EMISSIVE (SmileMaterial.RasterEmissive.rgb)

#else // caminho classico: cbuffer + tabela de descritores por draw

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
//   - o total (164B usados, RTEmissiveScale em 160) cabe no CBV de 256B que o FMaterial::Bind
//     liga em b1.
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
    // Nenhum shader de raster le este campo (o RT o consome ao empacotar o InstanceGeo), mas ele
    // esta aqui porque a regra acima e espelhar MaterialConstants campo a campo — um buraco no
    // espelho e exatamente como os offsets divergem em silencio.
    float  RTEmissiveScale;
};

// No caminho classico nao ha nada a carregar: o CBV e a tabela ja vieram amarrados pelo draw.
#define SMILE_MATERIAL_LOAD()

// Aqui o produto e feito por pixel, como sempre foi. Ver a versao bindless acima.
#define SMILE_MATERIAL_EMISSIVE (EmissiveFactor.rgb * EmissiveStrength)

#endif // SMILE_MATERIAL_BINDLESS

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
