// PBR Pixel Shader — Cook-Torrance BRDF with full PBR texture support.
// Texture slots (t0-t5) must match Material.h kMaterialTextureSlots order.

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

    // --- Parallax Occlusion Mapping (height map at t5) ---
    uint   HasHeightMap;
    float  HeightScale;        // max UV displacement at depth 1 (≈ 0.02..0.10)
    float  ParallaxMinSteps;   // ray-march samples when viewed head-on
    float  ParallaxMaxSteps;   // ray-march samples at grazing angles
    uint   ParallaxSelfShadow; // 1 = trace soft self-shadow toward the light
    float  ParallaxShadowSteps;
    float  ParallaxFadeStart;  // height-map mip where POM starts fading to flat
    float  ParallaxFadeRange;  // mips over which POM fades out (kills distant/grazing swimming)
    uint   ParallaxRefine;     // 1 = binary-search refine the intersection (sharper hit)
    uint   ParallaxRefineSteps; // binary-search iterations when refine is enabled
};

// --- Texture Slots ---

Texture2D AlbedoMap            : register(t0);
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2); // R=Metallic, G=Roughness (glTF convention)
Texture2D AOMap                : register(t3);
Texture2D EmissiveMap          : register(t4);
Texture2D HeightMap            : register(t5); // R = height [0,1] (1 = surface, 0 = deepest)

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
// Built once per pixel and shared by POM (view→tangent) and normal mapping.
void BuildTangentBasis(float3 N, float3 dPdx, float3 dPdy, float2 dUVdx, float2 dUVdy,
                       out float3 T, out float3 B) {
    T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);
    // Gram-Schmidt orthonormalize against N for stability
    T = normalize(T - N * dot(N, T));
    B = normalize(cross(N, T)); // re-derive B to guarantee orthonormality
}

float3 ApplyNormalMap(float3 N, float3 T, float3 B, float3 SampledNormal) {
    float3x3 TBN = float3x3(T, B, N);
    // Remap [0,1] -> [-1,1], scale XY by NormalStrength
    float3 n = SampledNormal * 2.0f - 1.0f;
    if (NormalFlipY) n.y = -n.y; // DirectX → OpenGL: invert green channel
    n.xy *= NormalStrength;
    return normalize(mul(n, TBN));
}


// --- Parallax Occlusion Mapping ---------------------------------------------
// Synthesis of the three engines studied:
//   • Flax  — linear ray-march + single secant intersection, tangent space,
//             SampleGrad for correct mips (MaterialGenerator.Textures.cpp:255).
//   • Unreal— view-dependent step count and the soft self-shadow loop
//             (ParallaxOcclusionMapping material function).
//   • Cry   — quality scaling idea (HasHeightMap gate ≈ off/on tier) and the
//             grazing-angle step ramp (shadeLib.txt:1136 / CommonZPasscfi:289).
// Convention: height map R in [0,1], 1 = surface top, 0 = deepest. All vectors
// in tangent space; Vts = surface→camera dir (Vts.z = N·V).
//
// 'fade' (0..1) scales displacement toward 0 at distance/grazing (see ParallaxFade)
// so the height field melts into the plain normal map instead of swimming.

// Mip-derived fade factor: 1 = full POM, 0 = flat. High mip (far away OR grazing,
// where the UV derivatives stretch) fades out — the trick Unreal/Cry use to hide
// the swimming/streaking artifacts at shallow angles and distance.
float ParallaxFade(float2 dUVdx, float2 dUVdy) {
    float2 texSize;
    HeightMap.GetDimensions(texSize.x, texSize.y);
    float2 dx  = dUVdx * texSize;
    float2 dy  = dUVdy * texSize;
    float  mip = 0.5f * log2(max(dot(dx, dx), dot(dy, dy)));
    return saturate(1.0f - (mip - ParallaxFadeStart) / max(ParallaxFadeRange, 1e-4f));
}

float2 ParallaxOcclusionMapping(float2 uv, float3 Vts, float2 dUVdx, float2 dUVdy,
                                float fade, out float outSurfaceHeight) {
    // More samples at grazing angles (Vts.z→0), fewer head-on (Vts.z→1).
    float numSteps = lerp(ParallaxMaxSteps, ParallaxMinSteps, saturate(Vts.z));
    float stepSize = 1.0f / numSteps;

    // Total UV travel along the view ray (heightmap convention → offset opposes Vts.xy).
    float2 maxOffset = -(Vts.xy / max(Vts.z, 1e-4f)) * (HeightScale * fade);
    float2 deltaUV   = maxOffset * stepSize;

    float  rayHeight  = 1.0f;
    float  lastHeight = 1.0f;
    float2 currOffset = 0.0f;
    float2 lastOffset = 0.0f;
    float  currHeight = HeightMap.SampleGrad(MaterialSampler, uv, dUVdx, dUVdy).r;

    [loop]
    for (int i = 0; i < (int)numSteps; ++i) {
        if (currHeight >= rayHeight) break;     // ray dipped below the surface
        lastOffset = currOffset;  lastHeight = currHeight;
        rayHeight -= stepSize;    currOffset += deltaUV;
        currHeight = HeightMap.SampleGrad(MaterialSampler, uv + currOffset, dUVdx, dUVdy).r;
    }

    // --- Resolve the exact hit inside the bracketing interval ---
    // [lastOffset,lastHeight] = ray still above surface; [currOffset,currHeight] = below.
    float2 finalOffset;
    if (ParallaxRefine) {
        // Binary search (CryEngine-style regula-falsi): a few iterations refine the
        // crossing, removing the stair-stepping the single secant leaves up close.
        // t parametrizes the interval: 0 = lastOffset (above), 1 = currOffset (below).
        float tLow = 0.0f, tHigh = 1.0f;
        [loop]
        for (int j = 0; j < (int)ParallaxRefineSteps; ++j) {
            float  tMid   = 0.5f * (tLow + tHigh);
            float2 offMid = lerp(lastOffset, currOffset, tMid);
            float  rayMid = rayHeight + stepSize * (1.0f - tMid);
            float  hMid   = HeightMap.SampleGrad(MaterialSampler, uv + offMid, dUVdx, dUVdy).r;
            if (hMid >= rayMid) tHigh = tMid; else tLow = tMid;
        }
        float t          = 0.5f * (tLow + tHigh);
        finalOffset      = lerp(lastOffset, currOffset, t);
        outSurfaceHeight = rayHeight + stepSize * (1.0f - t);
    } else {
        // Single secant interpolation between the two bracketing samples (Flax/UE).
        float afterDepth  = currHeight - rayHeight;
        float beforeDepth = lastHeight - (rayHeight + stepSize);
        float t           = afterDepth / max(afterDepth - beforeDepth, 1e-5f); // weight → last
        finalOffset       = lerp(currOffset, lastOffset, t);
        outSurfaceHeight  = rayHeight + stepSize * t;
    }
    return uv + finalOffset;
}

// Soft self-shadow: march from the hit point toward the light and accumulate the
// largest occluder penetration as a faked penumbra (adapted from Unreal's loop).
// Returns 1 = fully lit, 0 = fully shadowed.
float TraceParallaxSelfShadow(float2 uv, float surfaceHeight, float3 Lts,
                              float2 dUVdx, float2 dUVdy, float fade) {
    if (Lts.z <= 0.0f) return 0.0f;              // light below the local horizon
    float  stepSize = 1.0f / ParallaxShadowSteps;
    float2 deltaUV  = (Lts.xy / Lts.z) * (HeightScale * fade) * stepSize;

    float  shadow    = 0.0f;
    float  rayHeight = surfaceHeight;
    float2 offset    = 0.0f;
    [loop]
    for (int i = 0; i < (int)ParallaxShadowSteps; ++i) {
        rayHeight += Lts.z * stepSize;
        offset    += deltaUV;
        if (rayHeight >= 1.0f) break;            // climbed above the height field
        float h = HeightMap.SampleGrad(MaterialSampler, uv + offset, dUVdx, dUVdy).r;
        if (h > rayHeight)                       // occluded by taller relief
            shadow = max(shadow, (h - rayHeight) * 8.0f * (1.0f - (float)i / ParallaxShadowSteps));
    }
    return 1.0f - saturate(shadow);
}


float4 main(PSInput input) : SV_TARGET {
    float3 GeoN = normalize(input.worldNormal);

    // --- Tangent basis (built once, shared by POM + normal mapping) ---
    float3 dPdx  = ddx(input.worldPos);
    float3 dPdy  = ddy(input.worldPos);
    float2 dUVdx = ddx(input.uv);   // ORIGINAL UV derivatives → TBN continuous across surface
    float2 dUVdy = ddy(input.uv);
    float3 T, B;
    BuildTangentBasis(GeoN, dPdx, dPdy, dUVdx, dUVdy, T, B);

    float3 V = normalize(CameraPosition.xyz - input.worldPos);

    // --- Parallax Occlusion Mapping (offsets the UV used by every map below) ---
    float2 UV            = input.uv;
    float  SurfaceHeight = 1.0f;
    float  ParallaxFadeF = 0.0f;
    if (HasHeightMap) {
        ParallaxFadeF = ParallaxFade(dUVdx, dUVdy);    // 0 far/grazing .. 1 close
        if (ParallaxFadeF > 0.0f) {
            float3 Vts = float3(dot(V, T), dot(V, B), dot(V, GeoN)); // surface→camera in tangent space
            UV = ParallaxOcclusionMapping(input.uv, Vts, dUVdx, dUVdy, ParallaxFadeF, SurfaceHeight);
        }
    }

    // --- Base Color ---
    float3 BaseColor = BaseColorFactor.rgb;
    if (HasAlbedoMap)
        BaseColor *= AlbedoMap.Sample(MaterialSampler, UV).rgb;

    // --- Normal ---
    float3 N         = GeoN;
    float  ToksvigT  = 1.0f; // Toksvig factor from normal map .a; 1.0 = no per-mip variance
    if (HasNormalMap) {
        float4 NrmSample = NormalMap.Sample(MaterialSampler, UV);
        N        = ApplyNormalMap(GeoN, T, B, NrmSample.rgb);
        ToksvigT = NrmSample.a;
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

    // --- Lighting vectors --- (V built above, before POM)
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

        // POM soft self-shadowing toward the light (tangent-space march)
        if (ParallaxSelfShadow && HasHeightMap && ParallaxFadeF > 0.0f) {
            float3 Lts = float3(dot(L, T), dot(L, B), dot(L, GeoN));
            Lighting  *= TraceParallaxSelfShadow(UV, SurfaceHeight, Lts, dUVdx, dUVdy, ParallaxFadeF);
        }
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
