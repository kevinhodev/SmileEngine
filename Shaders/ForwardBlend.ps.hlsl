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
    // Nao usados aqui; mantem os offsets do FrameConstants ate o DDGIBiasParams, que o
    // SampleSceneDDGI abaixo le. Espelha Renderer.h — acrescentar campo la exige acrescentar aqui.
    float4 RenderParams;
    float4 CloudShadowParams;
    float4 CloudShadowParams2;
    float4 LightParams;
    float4 LightParams2;
    float4 DDGIBiasParams;     // x = escala do bias (0.2 legado), y = teto em metros (0 = sem teto)
    float4 AtmoLightParams;    // x = raio do planeta (km), y = raio do topo (km),
                               // z = km/unidade de mundo, w = transmitancia POR PIXEL (0/1)
    float4 SunColorRaw;        // rgb = sol SEM transmitancia e SEM HorizonFade
    float4 MoonColorRaw;       // rgb = lua SEM transmitancia
    float4 SkyAmbientSHR;      // SH-L1 do ceu, canal R: (c0, c1, c2, c3)
    float4 SkyAmbientSHG;
    float4 SkyAmbientSHB;
    float4 SkyAmbientSHParams; // x = usar SH (0 = 2 cores chapadas)
};

#include "Atmosphere/AtmosphereMath.hlsli"
#include "Atmosphere/SkyAmbientSH.hlsli"

// Ver DeferredLighting.ps: com a SH ligada o ambiente ganha termo direcional.
float3 SkyAmbientForNormal(float3 N) {
    if (SkyAmbientSHParams.x > 0.5f)
        return EvalSkyAmbientSH(SkyAmbientSHR, SkyAmbientSHG, SkyAmbientSHB, N);
    float hemi = saturate(N.y * 0.5f + 0.5f);
    return lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi);
}

#include "MaterialCB.hlsli"

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

// Mesmo LUT e mesma conta do DeferredLighting.ps — vidro tambem tem que ver o sol atenuado
// pela altitude dele. Ver o comentario extenso la (sombra do planeta, HorizonFade fora).
Texture2D<float4> AtmoTransmittanceLUT : register(t21);

SamplerState MaterialSampler : register(s0);
SamplerState IBLSampler      : register(s1);

float3 AtmoLightTransmittance(float3 worldPos, float3 dirToLight) {
    const float bottomR = AtmoLightParams.x;
    const float topR    = AtmoLightParams.y;
    const float kmPerWU = AtmoLightParams.z;

    const float h  = bottomR + max(worldPos.y, 0.0f) * kmPerWU;
    const float3 p = float3(0.0f, clamp(h, bottomR, topR - 1.0f), 0.0f);

    if (AtmoRaySphereNearest(p, dirToLight, bottomR) > 0.0f)
        return float3(0.0f, 0.0f, 0.0f); // sombra do planeta

    const float2 uv = AtmoTransmittanceParamsToUv(p.y, dirToLight.y, bottomR, topR);
    return AtmoTransmittanceLUT.SampleLevel(IBLSampler, uv, 0.0f).rgb;
}

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
// Espelha o DeferredLighting: fora do volume de sondas, o indireto vira o que este shader usaria
// sem GI — atmosferico hemisferico, senao difuso do IBL, senao nada — ja dividido pelo
// giIntensity que o caller aplica (o ambiente de fora do volume nao segue o slider do GI).
float3 DDGI_FallbackAmbient(float3 N) {
    float3 amb = float3(0.0f, 0.0f, 0.0f);
    if (SkyAmbientColor.w > 0.5f) {
        amb = SkyAmbientForNormal(N) * GroundAmbientColor.w;
    } else if (IBLParams.w > 0.5f) {
        amb = IrradianceMap.SampleLevel(IBLSampler, RotateY(N, IBLParams.y), 0.0f).rgb
            * IBLParams.x;
    }
    float giI = (DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f;
    return amb / giI;
}

float3 SampleSceneDDGI(float3 worldPos, float3 N) {
    float2 atlasInvSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
    // Empacotamento 2D do atlas (ver DDGI_TileOrigin); derivado da largura, vale p/ os dois atlas.
    int  tilesPerRow  = DDGI_TilesPerRow(DDGIParams.z, (int)DDGIParams.y);
    int  giFlags      = (int)DDGIDistParams.w;
    bool useChebyshev = (giFlags & 1) != 0;
    bool skip         = (giFlags & 2) != 0;
    bool fallback     = (giFlags & 4) != 0;
    uint skipMode     = skip ? (fallback ? 2u : 1u) : 0u;

    float volW = DDGI_VolumeWeight(worldPos, DDGIGridMin.xyz, DDGIGridMin.w,
                                   (int3)DDGIGridCount.xyz, DDGIBiasParams.z);
    if (volW <= 0.0f) return DDGI_FallbackAmbient(N);

    float3 gi;
    if (useChebyshev) {
        float2 distInvSize = float2(1.0f / DDGIDistParams.y, 1.0f / DDGIDistParams.z);
        float3 V = normalize(CameraPosition.xyz - worldPos);
        float3 biasVec = DDGI_SurfaceBias(N, V, DDGIGridMin.w,
                                          DDGIBiasParams.x, DDGIBiasParams.y);
        gi = SampleDDGIIrradianceCheb(DDGIIrradianceAtlas, DDGIDistanceAtlas, IBLSampler,
                 worldPos, N, DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                 (int)DDGIParams.y, atlasInvSize, (int)DDGIDistParams.x, distInvSize, biasVec,
                 DDGIProbeData, skipMode, tilesPerRow);
    } else {
        gi = SampleDDGIIrradiance(DDGIIrradianceAtlas, IBLSampler, worldPos, N,
                 DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                 (int)DDGIParams.y, atlasInvSize, tilesPerRow);
    }
    return (volW >= 1.0f) ? gi : lerp(DDGI_FallbackAmbient(N), gi, volW);
}

struct ForwardBlendOutput {
    float4 Color       : SV_Target0;
    float  Reactive    : SV_Target1;
    float  Composition : SV_Target2;
};

ForwardBlendOutput main(PSInput input) {
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
        float3 SunRadiance = (AtmoLightParams.w > 0.5f)
            ? SunColorRaw.rgb * AtmoLightTransmittance(input.worldPos, Lsun) * SunDirection.w
            : SunColor.rgb * SunDirection.w;
        BRDF_DirectSplit(N, V, Lsun, SunRadiance,
                         DiffuseColor, SpecularColor, Roughness, a2,
                         float3(0.0f, 0.0f, 0.0f), DirectDiffuse, DirectSpecular);
        if (MoonDirection.w > 0.0f) {
            float3 Lmoon = normalize(MoonDirection.xyz);
            float3 MoonDiffuse, MoonSpecular;
            float3 MoonRadiance = (AtmoLightParams.w > 0.5f)
                ? MoonColorRaw.rgb * AtmoLightTransmittance(input.worldPos, Lmoon) * MoonDirection.w
                : MoonColor.rgb * MoonDirection.w;
            BRDF_DirectSplit(N, V, Lmoon, MoonRadiance,
                             DiffuseColor, SpecularColor, Roughness, a2,
                             float3(0.0f, 0.0f, 0.0f), MoonDiffuse, MoonSpecular);
            DirectDiffuse  += MoonDiffuse;
            DirectSpecular += MoonSpecular;
        }
        // Mesmo gate do deferred: sem luz direta nao ha o que sombrear. Aqui e ainda mais
        // simples porque o ForwardBlend passa TransColor = 0 nas duas chamadas do BRDF, entao
        // difuso e especular zerados significam N.L <= 0 no sol E na lua.
        [branch] if (any(DirectDiffuse > 0.0f) || any(DirectSpecular > 0.0f)) {
            float Shadow = SampleCSM(input.worldPos, N, input.pos.xy);
            DirectDiffuse  *= Shadow;
            DirectSpecular *= Shadow;
        }
    }

    // --- Ambiente difuso: DDGI quando ha grid; senao hemisferio atmosferico. ---
    float3 AmbientDiffuse = float3(0.0f, 0.0f, 0.0f);
    bool UseGI          = DDGIGridCount.w > 0.5f;
    bool UseAtmoAmbient = SkyAmbientColor.w > 0.5f;
    if (UseGI) {
        float giIntensity = (DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f;
        // Sem (1 - Metallic): ele ja esta no DiffuseColor (convencao em BRDF.hlsli).
        AmbientDiffuse = DiffuseColor * SampleSceneDDGI(input.worldPos, N) * giIntensity;
    } else if (UseAtmoAmbient) {
        float3 ambientCol = SkyAmbientForNormal(N);
        AmbientDiffuse = DiffuseColor * ambientCol * GroundAmbientColor.w;
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
    ForwardBlendOutput Out;
    Out.Color = float4(OutColor, OutAlpha);
    // O vidro e composicao forward sem depth proprio. O alpha efetivo (inclui Fresnel) e o peso
    // conservador que o FSR deve rejeitar/reduzir no historico temporal.
    Out.Reactive = OutAlpha;
    Out.Composition = OutAlpha;
    return Out;
}
