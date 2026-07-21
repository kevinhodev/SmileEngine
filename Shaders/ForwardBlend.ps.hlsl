// Passe forward de translucidos (vidro do Bistro, decals com opacidade): desenha os materiais
// Blend por cima do HDR ja iluminado (deferred + reflexos), com depth read-only.
//
// Blend PREMULTIPLICADO (SrcBlend=ONE, DestBlend=INV_SRC_ALPHA): a parte "difusa" (tinte do
// vidro) e pesada por alpha AQUI no shader, e o especular — direto (sol/lua) e ambiente
// (IBL) — soma inteiro, estilo ThinTranslucent da UE: um vidro quase transparente continua
// refletindo e mantem o glint do sol. O alpha de saida ganha o Fresnel medio, entao
// em angulo rasante o vidro oculta mais o fundo (comportamento de janela real).
//
// Deliberadamente mais simples que o GBuffer.ps: sem normal map (vidro e liso; usa a normal
// geometrica), sem POM/AO/emissivo (vidro emissivo fica OPACO no cooker justamente p/ manter
// o caminho deferred). Iluminacao espelha o DeferredLighting: sol+lua com CSM, ambiente por
// DDGI (ou hemisferio atmosferico) + especular IBL.
#include "BRDF.hlsli"
#include "Shadow/CSMCommon.hlsli"
#include "GI/DDGICommon.hlsli"

cbuffer FrameCB : register(b0) {
    float4 CameraPosition;
    float4 IBLParams;
    float4 Time;
    float4 SunDirection;
    float4 SunColor;
    float4 SkyAmbientColor;
    float4 GroundAmbientColor;
    float4 DDGIGridMin;
    float4 DDGIGridCount;
    float4 DDGIParams;
    float4 DDGIDistParams;
    float4 ReflectionParams;
    float4 MoonDirection;
    float4 MoonColor;
    row_major float4x4 InvViewProj;
};

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

Texture2D AlbedoMap            : register(t0);
Texture2D MetallicRoughnessMap : register(t2);
Texture2D MetalnessMap         : register(t6);
Texture2D RoughnessMap         : register(t7);

TextureCube IrradianceMap  : register(t8);
TextureCube PrefilteredMap : register(t9);
Texture2D   BRDFLut        : register(t10);

Texture2D<float4> DDGIIrradianceAtlas : register(t12);
Texture2D<float4> DDGIDistanceAtlas   : register(t13);
Buffer<float4>    DDGIProbeData       : register(t15);

SamplerState MaterialSampler : register(s0);
SamplerState IBLSampler      : register(s1);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    float4 curClip     : TEXCOORD3;
    float4 prevClip    : TEXCOORD4;
    bool   frontFace   : SV_IsFrontFace;
};

// Mesmo sampling de DDGI do DeferredLighting (flags de Chebyshev/skip do frame).
float3 SampleSceneDDGI(float3 worldPos, float3 N) {
    float2 atlasInvSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
    int  giFlags      = (int)DDGIDistParams.w;
    bool useChebyshev = (giFlags & 1) != 0;
    bool skip         = (giFlags & 2) != 0;
    bool fallback     = (giFlags & 4) != 0;
    uint skipMode     = skip ? (fallback ? 2u : 1u) : 0u;
    if (useChebyshev) {
        float2 distInvSize = float2(1.0f / DDGIDistParams.y, 1.0f / DDGIDistParams.z);
        float3 V = normalize(CameraPosition.xyz - worldPos);
        float3 biasVec = DDGI_SurfaceBias(N, V, DDGIGridMin.w);
        return SampleDDGIIrradianceCheb(DDGIIrradianceAtlas, DDGIDistanceAtlas, IBLSampler,
                   worldPos, N, DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                   (int)DDGIParams.y, atlasInvSize, (int)DDGIDistParams.x, distInvSize, biasVec,
                   DDGIProbeData, skipMode);
    }
    return SampleDDGIIrradiance(DDGIIrradianceAtlas, IBLSampler, worldPos, N,
               DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
               (int)DDGIParams.y, atlasInvSize);
}

float4 main(PSInput input) : SV_Target {
    float3 N = normalize(input.worldNormal);
    if (!input.frontFace) N = -N; // two-sided (cull none no PSO)

    float3 V   = normalize(CameraPosition.xyz - input.worldPos);
    float  NoV = saturate(dot(N, V));

    float4 AlbedoSample = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, input.uv)
                                       : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 BaseColor = BaseColorFactor.rgb * AlbedoSample.rgb;
    float  Alpha     = saturate(BaseColorFactor.a * AlbedoSample.a);

    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float4 MR = MetallicRoughnessMap.Sample(MaterialSampler, input.uv);
        if (SpecularPacking) { Roughness *= MR.g; Metallic *= MR.b; }
        else                 { Metallic  *= MR.r; Roughness *= MR.g; }
    }
    if (HasMetalnessMap) Metallic  *= MetalnessMap.Sample(MaterialSampler, input.uv).r;
    if (HasRoughnessMap) Roughness *= RoughnessMap.Sample(MaterialSampler, input.uv).r;
    Roughness = max(Roughness, 0.04f);

    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);
    float  a  = Roughness * Roughness;
    float  a2 = a * a;

    // --- Direto: sol + lua, sombreados pelo CSM (mesmo caminho do deferred). Difuso e
    // especular SEPARADOS: o difuso e tinte (pesado por alpha), o especular e reflexo da
    // superficie e soma inteiro — mesmo racional do especular IBL abaixo. ---
    float3 DirectDiffuse  = float3(0.0f, 0.0f, 0.0f);
    float3 DirectSpecular = float3(0.0f, 0.0f, 0.0f);
    {
        float3 Lsun = normalize(SunDirection.xyz);
        BRDF_DirectSplit(N, V, Lsun, SunColor.rgb * SunDirection.w,
                         DiffuseColor, SpecularColor, Metallic, Roughness, a2,
                         float3(0.0f, 0.0f, 0.0f), DirectDiffuse, DirectSpecular);
        if (MoonDirection.w > 0.0f) {
            float3 Lmoon = normalize(MoonDirection.xyz);
            float3 MoonDiffuse, MoonSpecular;
            BRDF_DirectSplit(N, V, Lmoon, MoonColor.rgb * MoonDirection.w,
                             DiffuseColor, SpecularColor, Metallic, Roughness, a2,
                             float3(0.0f, 0.0f, 0.0f), MoonDiffuse, MoonSpecular);
            DirectDiffuse  += MoonDiffuse;
            DirectSpecular += MoonSpecular;
        }
        float Shadow = SampleCSM(input.worldPos, N, input.pos.xy);
        DirectDiffuse  *= Shadow;
        DirectSpecular *= Shadow;
    }

    // --- Ambiente difuso: DDGI quando ha grid; senao hemisferio atmosferico. ---
    float3 AmbientDiffuse = float3(0.0f, 0.0f, 0.0f);
    bool UseGI          = DDGIGridCount.w > 0.5f;
    bool UseAtmoAmbient = SkyAmbientColor.w > 0.5f;
    if (UseGI) {
        float giIntensity = (DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f;
        AmbientDiffuse = (1.0f - Metallic) * DiffuseColor *
                         SampleSceneDDGI(input.worldPos, N) * giIntensity;
    } else if (UseAtmoAmbient) {
        float  hemi       = saturate(N.y * 0.5f + 0.5f);
        float3 ambientCol = lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi);
        AmbientDiffuse = (1.0f - Metallic) * DiffuseColor * ambientCol * GroundAmbientColor.w;
    }

    // --- Especular ambiente (IBL): e o que faz vidro parecer vidro. Soma SEM peso de alpha. ---
    float3 F = F_SchlickRoughness(SpecularColor, NoV, Roughness);
    float3 SpecularIBL = float3(0.0f, 0.0f, 0.0f);
    if (IBLParams.w > 0.5f) {
        float3 R    = reflect(-V, N);
        float3 RotR = RotateY(R, IBLParams.y);
        float  Mip  = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y) * IBLParams.x;
    }

    // Saida premultiplicada: tinte (difuso direto + ambiente) pesado por alpha + especular
    // inteiro (sol/lua + IBL — vidro quase transparente continua com glint). O alpha de
    // saida ganha o Fresnel medio p/ o vidro ocultar mais o fundo em angulo rasante.
    float  Favg     = (F.x + F.y + F.z) * (1.0f / 3.0f);
    float  OutAlpha = saturate(Alpha + Favg * (1.0f - Alpha));
    float3 OutColor = (DirectDiffuse + AmbientDiffuse) * Alpha + DirectSpecular + SpecularIBL;
    return float4(OutColor, OutAlpha);
}
