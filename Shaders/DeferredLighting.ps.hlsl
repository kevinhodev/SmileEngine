#include "GBuffer.hlsli"
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

Texture2D        GBufferA   : register(t0); 
Texture2D        GBufferB   : register(t1); 
Texture2D        GBufferC   : register(t2); 
Texture2D<float> SceneDepth : register(t3);

TextureCube IrradianceMap  : register(t8);
TextureCube PrefilteredMap : register(t9);
Texture2D   BRDFLut        : register(t10);

Texture2D<float4> DDGIIrradianceAtlas : register(t12);
Texture2D<float4> DDGIDistanceAtlas   : register(t13);
Buffer<float4>    DDGIProbeData        : register(t15);
Texture2D<float>  SceneAO             : register(t14);

// ReSTIR GI: irradiancia difusa por pixel (final-gather sobre o DDGI). Ativa via ReflectionParams.w.
// Quando ligada, substitui o termo difuso do DDGI; o DDGI segue como cache no trace (multi-bounce).
Texture2D<float4> ReSTIRGITex         : register(t16);

SamplerState IBLSampler : register(s1); 

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Amostra o indireto do DDGI respeitando os flags do frame (Chebyshev/skip de probes inativos).
// Caminho UNICO p/ geometria e folhagem — com os pisos defensivos do Chebyshev (DDGICommon),
// folhagem densa nao crusha mais a preto e nao precisa de sampling separado.
float3 SampleSceneDDGI(float3 worldPos, float3 N) {
    float2 atlasInvSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
    int  giFlags      = (int)DDGIDistParams.w;
    bool useChebyshev = (giFlags & 1) != 0;
    bool skip         = (giFlags & 2) != 0;
    bool fallback     = (giFlags & 4) != 0;
    uint skipMode     = skip ? (fallback ? 2u : 1u) : 0u;
    if (useChebyshev) {
        float2 distInvSize = float2(1.0f / DDGIDistParams.y, 1.0f / DDGIDistParams.z);
        return SampleDDGIIrradianceCheb(DDGIIrradianceAtlas, DDGIDistanceAtlas, IBLSampler,
                   worldPos, N, DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                   (int)DDGIParams.y, atlasInvSize, (int)DDGIDistParams.x, distInvSize, 0.25f,
                   DDGIProbeData, skipMode);
    }
    return SampleDDGIIrradiance(DDGIIrradianceAtlas, IBLSampler, worldPos, N,
               DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
               (int)DDGIParams.y, atlasInvSize);
}

float4 main(VSOutput input) : SV_Target {
    int2  px       = int2(input.pos.xy);
    float rawDepth = SceneDepth.Load(int3(px, 0));
    if (rawDepth <= 0.0f) discard;

    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, rawDepth, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;

    float4 gA = GBufferA.Load(int3(px, 0));
    float4 gB = GBufferB.Load(int3(px, 0));
    float4 gC = GBufferC.Load(int3(px, 0));
    GBufferData g = DecodeGBuffer(gA, gB, gC);

    float3 N         = g.WorldNormal;
    float3 BaseColor = g.BaseColor;
    float  Metallic  = g.Metallic;
    float  Roughness = max(g.Roughness, 0.04f);
    float3 Emissive  = g.Emissive;
    float  matAO     = g.AO;
    bool   IsFoliage = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE);

    float3 V = normalize(CameraPosition.xyz - worldPos);

    float ssao  = IsFoliage ? 1.0f : SceneAO.Load(int3(px, 0));
    float AOAll = matAO * ssao;

    if (Time.w > 0.5f) return float4(ssao, ssao, ssao, 1.0f); 

    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    float3 TransColor = IsFoliage ? (BaseColor * 0.6f) : float3(0.0f, 0.0f, 0.0f);

    float a   = Roughness * Roughness;
    float a2  = a * a;
    float NoV = saturate(dot(N, V));

    float ReflectCombine = ReflectionParams.z > 0.5f
        ? saturate((ReflectionParams.x - Roughness) / max(ReflectionParams.y, 1e-4f))
        : 0.0f;
    float SpecAmbientScale = 1.0f - ReflectCombine;

    float3 Lighting = float3(0.0f, 0.0f, 0.0f);
    {
        float3 Lsun        = normalize(SunDirection.xyz);
        float3 SunRadiance = SunColor.rgb * SunDirection.w;
        float3 SunLit      = BRDF_Direct(N, V, Lsun, SunRadiance,
                                         DiffuseColor, SpecularColor, Metallic, Roughness, a2, TransColor);

        float3 MoonLit = float3(0.0f, 0.0f, 0.0f);
        if (MoonDirection.w > 0.0f) {
            float3 Lmoon        = normalize(MoonDirection.xyz);
            float3 MoonRadiance = MoonColor.rgb * MoonDirection.w;
            MoonLit = BRDF_Direct(N, V, Lmoon, MoonRadiance,
                                  DiffuseColor, SpecularColor, Metallic, Roughness, a2, TransColor);
        }

        Lighting += (SunLit + MoonLit) * SampleCSM(worldPos, N, input.pos.xy);
    }

    float3 Ambient = float3(0.0f, 0.0f, 0.0f);

    bool UseGI         = DDGIGridCount.w > 0.5f;
    bool UseReSTIR     = ReflectionParams.w > 0.5f; // ReSTIR GI ativo -> substitui o difuso do DDGI
    bool UseAtmoAmbient = SkyAmbientColor.w > 0.5f;

    if (UseAtmoAmbient && !UseGI) {
        float  hemi       = saturate(N.y * 0.5f + 0.5f);
        float3 ambientCol = lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi);
        float3 KdAmb      = (1.0f - Metallic);
        Ambient += KdAmb * DiffuseColor * ambientCol * AOAll * GroundAmbientColor.w;
    }

    if (IBLParams.w > 0.5f) {
        float3 RotN = RotateY(N, IBLParams.y);
        float3 R    = reflect(-V, N);
        float3 RotR = RotateY(R, IBLParams.y);

        float3 F     = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        float3 KdIBL = (1.0f - F) * (1.0f - Metallic);

        float3 Irradiance  = IrradianceMap.SampleLevel(IBLSampler, RotN, 0.0f).rgb;
        float3 DiffuseIBL  = KdIBL * DiffuseColor * Irradiance;

        float  Mip         = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y) * SpecAmbientScale;

        float3 IBLContrib = (UseGI || UseAtmoAmbient) ? SpecularIBL : (DiffuseIBL + SpecularIBL);
        Ambient += IBLContrib * AOAll * IBLParams.x;
    }

    float3 DbgGI = float3(0.0f, 0.0f, 0.0f);
    if (UseGI || UseReSTIR) {
        float3 gi;
        // Folhagem (two-sided, estilo UE "two-sided foliage"): indireto = frente + 0.6*tras
        // (transmissao pela folha — mesmo fator do TransColor no direto e do SubsurfaceColor
        // do loader). ADITIVO, nao media: hera colada na parede tem o lado -N preto (dentro do
        // predio) e a media derrubava o indireto pela metade. Mantem o Chebyshev do caminho
        // comum: os pisos defensivos (DDGICommon) ja evitam o colapso a preto que motivava
        // pula-lo, e SEM ele a trilinear mistura probes de DENTRO da parede (hera mais escura
        // que a propria parede). Folhagem segue FORA do gather per-pixel do ReSTIR (se auto-
        // oclui em folhagem densa; o Lumen pula screen traces backface pelo mesmo motivo).
        bool foliageFill = IsFoliage && UseGI;
        if (foliageFill) {
            gi = SampleSceneDDGI(worldPos, N) + 0.6f * SampleSceneDDGI(worldPos, -N);
        } else if (UseReSTIR) {
            // ReSTIR full-res no A0-A3 (Load por pixel); o A4 passa p/ meia-res + upsample bilateral.
            gi = ReSTIRGITex.Load(int3(px, 0)).rgb;
            // ReflectionParams.w == 2 -> saida do NRD REBLUR em YCoCg: desempacota p/ linear
            // (_NRD_YCoCgToLinear). Sem isto a cena fica avermelhada (YCoCg lido como RGB).
            if (ReflectionParams.w > 1.5f) {
                float t = gi.x - gi.z;
                gi = max(float3(t + gi.y, gi.x + gi.z, t - gi.y), 0.0f);
            }
        } else {
            gi = SampleSceneDDGI(worldPos, N);
        }
        DbgGI = gi;

        // Intensidade do GI: usa o slider do DDGI quando ha grid; senao (ReSTIR sem DDGI) cai em 1.
        float giIntensity = (UseGI && DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f;
        float3 KdGI = (1.0f - Metallic);
        Ambient += KdGI * DiffuseColor * gi * giIntensity * AOAll;

        if (IBLParams.w <= 0.5f) {
            float3 Fa = F_SchlickRoughness(SpecularColor, NoV, Roughness);
            Ambient += Fa * gi * giIntensity * AOAll * SpecAmbientScale;
        }
    }

    float3 FinalColor = Lighting + Ambient + Emissive;

    if (DDGIGridCount.w > 1.5f) return float4(DbgGI, 1.0f); 

    if (CSM_DebugEnabled()) {
        int ci = CSM_SelectCascade(worldPos, N);
        if (ci >= 0)
            FinalColor = lerp(FinalColor, CSM_CascadeColor(ci), 0.45f);
    }

    return float4(FinalColor, 1.0f);
}
