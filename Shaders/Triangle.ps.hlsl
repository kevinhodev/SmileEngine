// PBR Pixel Shader — Cook-Torrance BRDF with full PBR texture support.
// Texture slots (t0-t4) must match Material.h kMaterialTextureSlots order.

// --- Constant Buffers ---

cbuffer TransformCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    float4 CameraPosition;
    float4 LightDirection; // xyz = world position of point light
    float4 LightColor;     // rgb = color, w = brightness
    float4 IBLParams;      // x = intensity, y = rotation (rad), z = maxMip (specular), w = enabled (0/1)
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
    uint   NormalFlipY;    // 1 = DirectX convention (G inverted), 0 = OpenGL
};

// --- Texture Slots ---

Texture2D AlbedoMap            : register(t0);
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2); // R=Metallic, G=Roughness (glTF convention)
Texture2D AOMap                : register(t3);
Texture2D EmissiveMap          : register(t4);

TextureCube IrradianceMap      : register(t6); // diffuse irradiance cube
TextureCube PrefilteredMap     : register(t7); // GGX prefiltered specular cube (mip = roughness * maxMip)
Texture2D   BRDFLut            : register(t8); // Karis split-sum LUT (RG16F: F0 scale, F0 bias)

SamplerState MaterialSampler   : register(s0); // anisotropic wrap (materials)
SamplerState IBLSampler        : register(s1); // trilinear clamp   (cube + LUT)

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

void GGXEnergyLookup(float Roughness, float NoV, out float E, out float Ef) {
    float r = Roughness;
    float c = NoV;
    E = 1.0f - saturate(pow(r, c / (r + 1e-5f)) * ((r * c + 0.0266916f) / (0.466495f + c)));
    Ef = pow(1.0f - c, 5.0f) * pow(2.36651f * pow(c, 4.7703f * r) + 0.0387332f, r);
}

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

// Roughness-aware Schlick: clamps Fresnel on rough surfaces (used only for IBL).
float3 F_SchlickRoughness(float3 F0, float cosTheta, float roughness) {
    float r1 = 1.0f - roughness;
    return F0 + (max(float3(r1, r1, r1), F0) - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 RotateY(float3 v, float angle) {
    float s = sin(angle), c = cos(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}


// --- Tangent space from screen-space derivatives ---
float3 PerturbNormal(float3 N, float3 dPdx, float3 dPdy, float2 dUVdx, float2 dUVdy, float3 SampledNormal) {
    float3 T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    float3 B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);
    // Gram-Schmidt orthonormalize against N for stability
    T = normalize(T - N * dot(N, T));
    B = normalize(cross(N, T)); // re-derive B to guarantee orthonormality
    float3x3 TBN = float3x3(T, B, N);

    // Remap [0,1] -> [-1,1], scale XY by NormalStrength
    float3 n = SampledNormal * 2.0f - 1.0f;
    if (NormalFlipY) n.y = -n.y; // DirectX → OpenGL: invert green channel
    n.xy *= NormalStrength;
    return normalize(mul(n, TBN));
}


float4 main(PSInput input) : SV_TARGET {
    float2 UV          = input.uv;
    float3 GeoN        = normalize(input.worldNormal);


    // --- Base Color ---
    float3 BaseColor = BaseColorFactor.rgb;
    if (HasAlbedoMap)
        BaseColor *= AlbedoMap.Sample(MaterialSampler, UV).rgb;

    // --- Normal ---
    float3 N         = GeoN;
    float  ToksvigT  = 1.0f; // Toksvig factor from normal map .a; 1.0 = no per-mip variance
    if (HasNormalMap) {
        float4 NrmSample      = NormalMap.Sample(MaterialSampler, UV);
        float3 SampledNormal  = NrmSample.rgb;
        ToksvigT              = NrmSample.a;
        float3 dPdx = ddx(input.worldPos);
        float3 dPdy = ddy(input.worldPos);
        // Pass ORIGINAL UV derivatives so the TBN is continuous across the surface.
        N = PerturbNormal(GeoN, dPdx, dPdy, ddx(input.uv), ddy(input.uv), SampledNormal);
    }

    // --- Metallic / Roughness ---
    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float2 MR = MetallicRoughnessMap.Sample(MaterialSampler, UV).rg;
        Metallic  *= MR.r;
        Roughness *= MR.g;
    }
    Roughness = max(Roughness, 0.04f); // avoid mirror-perfect surfaces

    // --- Specular Anti-Aliasing (Karis SAA + Toksvig combined in α² space) ---
    // Both contributions accumulate as variances on α² (microfacet variance):
    //   • Karis: per-pixel normal derivative captures screen-space normal change.
    //     0.18 cap prevents over-darkening from high-frequency normal map noise.
    //   • Toksvig: mip-level normal variance encoded as .a during normal map
    //     mipgen. T=1 (mip 0) contributes 0; lower T (high mips, blended via
    //     bilinear) contributes (1 - T²).
    // Final α² = α²_base + kernel_karis + toksvig_var, clamped, then converted
    // back to perceptual roughness via sqrt(sqrt(...)) for the rest of the PS.
    {
        float3 dNdx = ddx(N);
        float3 dNdy = ddy(N);
        float variance         = 0.25f * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        float kernelRoughness2 = min(2.0f * variance, 0.18f);
        float toksvigVar       = 1.0f - ToksvigT * ToksvigT; // 0 when T=1
        float aLin             = Roughness * Roughness;      // perceptual² = α (GGX linear)
        float a2New            = saturate(aLin * aLin + kernelRoughness2 + toksvigVar);
        Roughness              = sqrt(sqrt(a2New));          // back to perceptual
    }

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

    float Atten = 1.0f / (Dist * Dist + 1.0f);

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
        float D = D_GGX(a2, NoH);
        #if USE_EXACT_SMITH
            float Vis = Vis_SmithJointExact(a2, NoV, NoL);
        #else
            float Vis = Vis_SmithJointApprox(a2, NoV, NoL);
        #endif
        float3 F = F_Schlick(SpecularColor, VoH);
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

        float3 Radiance = LightColor.rgb * LightColor.w * Atten;
        Lighting        = (Diffuse + Specular) * Radiance * NoL;
    }

    // --- Ambient (Image-Based Lighting, split-sum) ---
    float3 Ambient = float3(0.0f, 0.0f, 0.0f);
    if (IBLParams.w > 0.5f) {
        float3 RotN = RotateY(N, IBLParams.y);
        float3 R    = reflect(-V, N);
        float3 RotR = RotateY(R, IBLParams.y);

        float3 F        = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        float3 KdIBL    = (1.0f - F) * (1.0f - Metallic);

        float3 Irradiance  = IrradianceMap.SampleLevel(IBLSampler, RotN, 0.0f).rgb;
        float3 DiffuseIBL  = KdIBL * DiffuseColor * Irradiance;

        float  Mip         = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y);

        Ambient = (DiffuseIBL + SpecularIBL) * AO * IBLParams.x;
    }

    // --- Emissive ---
    float3 Emissive = EmissiveFactor.rgb * EmissiveStrength;
    if (HasEmissiveMap)
        Emissive *= EmissiveMap.Sample(MaterialSampler, UV).rgb;

    // --- Compose and tonemap ---
    float3 FinalColor = Lighting + Ambient + Emissive;
    FinalColor = FinalColor / (FinalColor + 1.0f); // Reinhard tonemap
    FinalColor = pow(FinalColor, 1.0f / 2.2f);     // Gamma correction

    return float4(FinalColor, 1.0f);
}
