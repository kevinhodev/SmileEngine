// PBR Pixel Shader — Cook-Torrance BRDF with full PBR texture support.
// Texture slots (t0-t5) must match Material.h kMaterialTextureSlots order.

// --- Constant Buffers ---

cbuffer TransformCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    float4 CameraPosition;
    float4 LightDirection; // xyz = world position of point light
    float4 LightColor;     // rgb = color, w = brightness
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
    uint   HasHeightMap;
    uint   HasEmissiveMap;
    float  NormalStrength;
    float  HeightScale;
    uint   NormalFlipY;    // 1 = DirectX convention (G inverted), 0 = OpenGL
};

// --- Texture Slots ---

Texture2D AlbedoMap            : register(t0);
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2); // R=Metallic, G=Roughness (glTF convention)
Texture2D AOMap                : register(t3);
Texture2D HeightMap            : register(t4);
Texture2D EmissiveMap          : register(t5);

SamplerState MaterialSampler   : register(s0); // anisotropic wrap (static in root sig)

// --- Input ---

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

#define USE_KULLA_CONTY_ENERGY_CONSERVATION 1
#define USE_BURLEY_DIFFUSE 1
#define USE_EXACT_SMITH 1

// --- PBR Functions ---

static const float PI = 3.14159265359f;

// Analytical approximation of GGX directional albedo E(u, v) and Fresnel-weighted albedo E_f(u, v)
// From Unreal Engine 5 (derived from Kulla-Conty 2017 and Fdez-Aguera 2019)
void GGXEnergyLookup(float Roughness, float NoV, out float E, out float Ef) {
    float r = Roughness;
    float c = NoV;
    E = 1.0f - saturate(pow(r, c / (r + 1e-5f)) * ((r * c + 0.0266916f) / (0.466495f + c)));
    Ef = pow(1.0f - c, 5.0f) * pow(2.36651f * pow(c, 4.7703f * r) + 0.0387332f, r);
}

// Disney/Burley Diffuse BRDF
// [Burley 2012, "Physically-Based Shading at Disney"]
float3 Diffuse_Burley(float3 DiffuseColor, float Roughness, float NoV, float NoL, float VoH) {
    float FD90 = 0.5f + 2.0f * VoH * VoH * Roughness;
    float FdV = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoV, 5.0f);
    float FdL = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoL, 5.0f);
    return DiffuseColor * ((1.0f / PI) * FdV * FdL);
}

float D_GGX(float a2, float NoH) {
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / (PI * d * d);
}

float Vis_SmithJointApprox(float a2, float NoV, float NoL) {
    float a = sqrt(a2);
    float Vis_SmithV = NoL * (NoV * (1.0f - a) + a);
    float Vis_SmithL = NoV * (NoL * (1.0f - a) + a);
    return 0.5f / (Vis_SmithV + Vis_SmithL + 1e-5f);
}

// Exact Smith Joint visibility function
// [Smith 1967, Heitz 2014]
float Vis_SmithJointExact(float a2, float NoV, float NoL) {
    float Vis_SmithV = NoL * sqrt(NoV * (NoV - NoV * a2) + a2);
    float Vis_SmithL = NoV * sqrt(NoL * (NoL - NoL * a2) + a2);
    return 0.5f / (Vis_SmithV + Vis_SmithL + 1e-5f);
}

float3 F_Schlick(float3 SpecularColor, float VoH) {
    float Fc  = pow(1.0f - VoH, 5.0f);
    float f90 = saturate(50.0f * SpecularColor.g);
    return f90 * Fc + (1.0f - Fc) * SpecularColor;
}

// --- Tangent space from screen-space derivatives (no per-vertex tangent needed) ---
// Same technique used in CryEngine for meshes without pre-computed tangents.
float3 PerturbNormal(float3 N, float3 WorldPos, float2 UV, float3 SampledNormal) {
    float3 dPdx = ddx(WorldPos);
    float3 dPdy = ddy(WorldPos);
    float2 dUVdx = ddx(UV);
    float2 dUVdy = ddy(UV);

    float3 T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    float3 B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);
    float3x3 TBN = float3x3(T, B, N);

    // Remap [0,1] -> [-1,1], scale XY by NormalStrength
    float3 n = SampledNormal * 2.0f - 1.0f;
    if (NormalFlipY) n.y = -n.y; // DirectX → OpenGL: invert green channel
    n.xy *= NormalStrength;
    return normalize(mul(n, TBN));
}

// --- Parallax Occlusion Mapping ---
float2 ParallaxOcclusionMapping(float2 UV, float3 WorldPos, float3 N, out float OutDepth, out float2 OutdUVdx, out float2 OutdUVdy) {
    float3 V = normalize(CameraPosition.xyz - WorldPos);

    // Build view direction in tangent space via derivatives
    float3 dPdx = ddx(WorldPos);
    float3 dPdy = ddy(WorldPos);
    float2 dUVdx = ddx(UV);
    float2 dUVdy = ddy(UV);
    float3 T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    float3 B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);

    float3 viewTS;
    viewTS.x = dot(V, T);
    viewTS.y = dot(V, B);
    viewTS.z = dot(V, N);

    const int MaxSteps = 64;
    const int MinSteps = 8;
    // viewTS.z is dot(V, N) - 1.0 directly facing, 0.0 at grazing angle
    int NumSteps = (int)lerp(MaxSteps, MinSteps, saturate(viewTS.z));

    float  LayerDepth  = 1.0f / NumSteps;
    float2 UVDelta     = (viewTS.xy / (viewTS.z + 0.001f)) * HeightScale / NumSteps;

    float  CurrentDepth = 0.0f;
    float2 CurrentUV    = UV;
    float  SampledDepth = HeightMap.SampleGrad(MaterialSampler, CurrentUV, dUVdx, dUVdy).r;

    [loop]
    for (int i = 0; i < MaxSteps; ++i) {
        if (i >= NumSteps) break;
        if (CurrentDepth >= SampledDepth) break;
        CurrentUV    += UVDelta;
        SampledDepth  = HeightMap.SampleGrad(MaterialSampler, CurrentUV, dUVdx, dUVdy).r;
        CurrentDepth += LayerDepth;
    }

    // Refine with Iterative Secant (Regula Falsi) method
    float2 PrevUV = CurrentUV - UVDelta;
    float  PrevDepth = CurrentDepth - LayerDepth;
    
    float  Height0 = HeightMap.SampleGrad(MaterialSampler, PrevUV, dUVdx, dUVdy).r;
    float  Error0 = PrevDepth - Height0;
    float  Error1 = CurrentDepth - SampledDepth;

    float2 FinalUV = CurrentUV;
    float  FinalRayDepth = CurrentDepth;
    
    #ifndef POM_SECANT_STEPS
    #define POM_SECANT_STEPS 3
    #endif

    [unroll(POM_SECANT_STEPS)]
    for (int j = 0; j < POM_SECANT_STEPS; ++j) {
        float denom = Error0 - Error1;
        float w = (abs(denom) > 1e-5f) ? (Error0 / denom) : 0.5f;
        w = saturate(w);

        FinalUV = lerp(PrevUV, CurrentUV, w);
        FinalRayDepth = lerp(PrevDepth, CurrentDepth, w);
        float RefinedHeight = HeightMap.SampleGrad(MaterialSampler, FinalUV, dUVdx, dUVdy).r;
        float NewError = FinalRayDepth - RefinedHeight;

        if (NewError > 0.0f) {
            PrevUV = FinalUV;
            PrevDepth = FinalRayDepth;
            Error0 = NewError;
        } else {
            CurrentUV = FinalUV;
            CurrentDepth = FinalRayDepth;
            Error1 = NewError;
        }
    }
    
    OutDepth = FinalRayDepth;
    OutdUVdx = dUVdx;
    OutdUVdy = dUVdy;
    return FinalUV;
}

// --- Parallax Self Shadowing ---
float ParallaxSelfShadow(float2 UV, float3 L, float3 WorldPos, float3 N, float InitialDepth, float2 dUVdx, float2 dUVdy) {
    float NoL = dot(N, L);
    if (NoL <= 0.0f) return 0.0f;

    float3 dPdx = ddx(WorldPos);
    float3 dPdy = ddy(WorldPos);
    float3 T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    float3 B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);

    float3 lightTS;
    lightTS.x = dot(L, T);
    lightTS.y = dot(L, B);
    lightTS.z = dot(L, N);

    if (lightTS.z <= 0.0f) return 0.0f;

    const int ShadowSteps = 8;
    float StepSize = InitialDepth / ShadowSteps;
    float2 UVDelta = (lightTS.xy / (lightTS.z + 0.001f)) * HeightScale * StepSize;

    float CurrentDepth = InitialDepth;
    float2 CurrentUV = UV;
    float SampledDepth = HeightMap.SampleGrad(MaterialSampler, CurrentUV, dUVdx, dUVdy).r;

    float ShadowFactor = 1.0f;
    const float ShadowHardness = 1.0f; // Controls shadow soft transition
    float DistTravelled = 0.001f;

    float PrevDepth = CurrentDepth;
    float PrevSampledDepth = SampledDepth;

    [unroll(8)]
    for (int i = 0; i < ShadowSteps; ++i) {
        PrevDepth = CurrentDepth;
        PrevSampledDepth = SampledDepth;

        CurrentDepth -= StepSize;
        CurrentUV += UVDelta;
        SampledDepth = HeightMap.SampleGrad(MaterialSampler, CurrentUV, dUVdx, dUVdy).r;
        DistTravelled += StepSize;

        if (SampledDepth < CurrentDepth) {
            float den = (SampledDepth - PrevSampledDepth) - (CurrentDepth - PrevDepth);
            float t = (abs(den) > 1e-5f) ? (PrevDepth - PrevSampledDepth) / den : 0.5f;

            float InterpDepth = lerp(PrevDepth, CurrentDepth, t);
            float InterpHeight = lerp(PrevSampledDepth, SampledDepth, t);

            float CurrentShadow = (InterpDepth - InterpHeight) * ShadowHardness / (DistTravelled - StepSize * (1.0f - t));
            ShadowFactor = min(ShadowFactor, 1.0f - saturate(CurrentShadow));
            
            break;
        }
    }

    return ShadowFactor;
}

// --- PS Output Structure for Depth Modification ---
struct PSOutput {
    float4 color : SV_TARGET;
    float  depth : SV_Depth;
};

// --- Main ---

PSOutput main(PSInput input) {
    PSOutput output;
    float2 UV = input.uv;
    float POMDepth = 0.0f;
    float2 POMdUVdx = 0;
    float2 POMdUVdy = 0;

    // Parallax offset (applied before all other texture reads)
    if (HasHeightMap) {
        UV = ParallaxOcclusionMapping(UV, input.worldPos, normalize(input.worldNormal), POMDepth, POMdUVdx, POMdUVdy);
    }

    // --- Base Color ---
    float3 BaseColor = BaseColorFactor.rgb;
    if (HasAlbedoMap)
        BaseColor *= AlbedoMap.Sample(MaterialSampler, UV).rgb;

    // --- Normal ---
    float3 N = normalize(input.worldNormal);
    if (HasNormalMap) {
        float3 SampledNormal = NormalMap.Sample(MaterialSampler, UV).rgb;
        N = PerturbNormal(N, input.worldPos, UV, SampledNormal);
    }

    // --- Metallic / Roughness (glTF: R=Occlusion, G=Roughness, B=Metallic in some tools;
    //     we use R=Metallic, G=Roughness per Substance Painter default export) ---
    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float2 MR  = MetallicRoughnessMap.Sample(MaterialSampler, UV).rg;
        Metallic  *= MR.r;
        Roughness *= MR.g;
    }
    Roughness = max(Roughness, 0.04f); // avoid mirror-perfect surfaces

    // --- Specular Anti-Aliasing (Geometric Roughness Filter) ---
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float normalVariance = max(dot(dNdx, dNdx), dot(dNdy, dNdy));
    float geometricRoughness = saturate(normalVariance * 2.0f);
    Roughness = max(Roughness, geometricRoughness);

    // --- Ambient Occlusion ---
    float AO = 1.0f;
    if (HasAOMap)
        AO = lerp(1.0f, AOMap.Sample(MaterialSampler, UV).r, AOStrength);

    // --- Lighting vectors ---
    float3 V       = normalize(CameraPosition.xyz - input.worldPos);
    float3 LightP  = LightDirection.xyz;
    float3 LightV  = LightP - input.worldPos;
    float  Dist    = length(LightV);
    float3 L       = LightV / Dist;
    float3 H       = normalize(L + V);

    float Atten    = 1.0f / (Dist * Dist + 1.0f);

    float NoL = saturate(dot(N, L));
    float NoV = saturate(dot(N, V));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    // --- PBR material derivation ---
    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    float a  = Roughness * Roughness;
    float a2 = a * a;

    // --- Direct lighting (Cook-Torrance BRDF) ---
    float3 Lighting = float3(0.0f, 0.0f, 0.0f);
    if (NoL > 0.0f) {
        float  D       = D_GGX(a2, NoH);
        #if USE_EXACT_SMITH
            float  Vis     = Vis_SmithJointExact(a2, NoV, NoL);
        #else
            float  Vis     = Vis_SmithJointApprox(a2, NoV, NoL);
        #endif
        float3 F       = F_Schlick(SpecularColor, VoH);
        float3 Specular = (D * Vis) * F;
        
        float3 Kd = 1.0f - Metallic;

        #if USE_KULLA_CONTY_ENERGY_CONSERVATION
            // 1. Specular multiple-scattering energy compensation (Kulla-Conty)
            float E_val, Ef_val;
            GGXEnergyLookup(Roughness, NoV, E_val, Ef_val);

            // W = 1.0 + F0 * (1 - E) / E
            float3 W = 1.0f + SpecularColor * ((1.0f - E_val) / max(E_val, 1e-5f));
            Specular *= W;

            // 2. Diffuse-Specular energy preservation (split-sum)
            float3 F90 = saturate(50.0f * SpecularColor.g);
            float3 E_spec = W * (E_val * SpecularColor + Ef_val * (F90 - SpecularColor));
            
            // Attenuate diffuse by the energy reflected/scattered specularly
            float DiffuseScale = 1.0f - saturate(dot(E_spec, float3(0.2126f, 0.7152f, 0.0722f))); // Luminance
            Kd *= DiffuseScale;
        #else
            Kd *= (1.0f - F); // original ad-hoc conservation
        #endif

        #if USE_BURLEY_DIFFUSE
            float3 Diffuse = Kd * Diffuse_Burley(DiffuseColor, Roughness, NoV, NoL, VoH);
        #else
            float3 Diffuse = (Kd * DiffuseColor) / PI;
        #endif

        float POMShadow = 1.0f;
        if (HasHeightMap) {
            POMShadow = ParallaxSelfShadow(UV, L, input.worldPos, normalize(input.worldNormal), POMDepth, POMdUVdx, POMdUVdy);
        }

        float3 Radiance = LightColor.rgb * LightColor.w * Atten * POMShadow;
        Lighting        = (Diffuse + Specular) * Radiance * NoL;
    }

    // --- Ambient (hemispheric sky simulation) ---
    float  SkyOcclusion = dot(N, float3(0.0f, 1.0f, 0.0f)) * 0.5f + 0.5f;
    float3 Ambient      = float3(0.05f, 0.06f, 0.08f) * BaseColor * SkyOcclusion * AO;

    // --- Emissive ---
    float3 Emissive = EmissiveFactor.rgb * EmissiveStrength;
    if (HasEmissiveMap)
        Emissive *= EmissiveMap.Sample(MaterialSampler, UV).rgb;

    // --- Compose and tonemap ---
    float3 FinalColor = Lighting + Ambient + Emissive;
    FinalColor = FinalColor / (FinalColor + 1.0f); // Reinhard tonemap
    FinalColor = pow(FinalColor, 1.0f / 2.2f);     // Gamma correction

    output.color = float4(FinalColor, 1.0f);

    // --- Pixel Depth Offset (PDO) Calculation ---
    if (HasHeightMap) {
        // Displace the world position into the surface along the geometric normal
        float3 WorldPosDisplaced = input.worldPos - normalize(input.worldNormal) * (POMDepth * HeightScale);
        // Project the displaced world position to homogeneous clip space
        float4 PosClip = mul(float4(WorldPosDisplaced, 1.0f), MVP);
        // Perform perspective divide to get correct NDC depth [0, 1]
        output.depth = PosClip.z / PosClip.w;
    } else {
        // Default geometric depth
        output.depth = input.pos.z;
    }

    return output;
}
