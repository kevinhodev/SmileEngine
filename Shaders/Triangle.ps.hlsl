#include "Shadow/CSMCommon.hlsli" // CSM do sol: CSMCB (b3), SunShadowMap (t11), ShadowCmp (s2)
#include "GI/DDGICommon.hlsli"     // DDGI: SampleDDGIIrradiance (GI difusa indireta, atlas t12)

// --- Constant Buffers ---

cbuffer FrameCB : register(b0) {
    float4 CameraPosition;
    float4 IBLParams;      // x = intensity, y = rotation (rad), z = maxMip (specular), w = enabled (0/1)
    float4 Time;           // x = elapsed sec, y = delta sec, z = frameIndex
    float4 SunDirection;   // xyz = direction TO the sun (normalized), w = intensity
    float4 SunColor;       // rgb = color, w = unused
    float4 SkyAmbientColor;    // rgb = sky (zenith) ambient, w = enabled (0/1)
    float4 GroundAmbientColor; // rgb = ground (nadir) ambient, w = intensity
    float4 DDGIGridMin;        // xyz = origem do grid (mundo), w = espacamento
    float4 DDGIGridCount;      // xyz = nº de probes por eixo, w = enabled (0=off,1=on,2=debug)
    float4 DDGIParams;         // x = intensity, y = tileSize, z = atlasW, w = atlasH
    float4 DDGIDistParams;     // x = distTile, y = distAtlasW, z = distAtlasH, w = chebyshev (0/1)
    // Specular GI (reflexoes RT): quando ligado, o forward exporta o G-buffer (SV_Target1) e
    // reduz o specular ambiente por (1-combineAlpha) (o RT o substitui via composite).
    float4 ReflectionParams;   // x = maxRoughnessToTrace, y = roughnessFadeLength, z = enabled (0/1), w = -
    float4 MoonDirection;      // xyz = direction TO moon (normalized), w = moonlight intensity (0 = off)
    float4 MoonColor;          // rgb = cor do luar (fria, tingida pela transmitancia), w = -
};

struct PSOutput {
    float4 color   : SV_Target0;
    float4 gbuffer : SV_Target1;
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
    uint   NormalFlipY;    // 0 = normal map OpenGL/GL (default, verde invertido p/ D3D); 1 = DirectX

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

    // Separate metalness/roughness maps (t6/t7), as an alternative to the combined MR (t2)
    uint   HasMetalnessMap;
    uint   HasRoughnessMap;

    // --- Import (Bistro/ORCA convention) ---
    uint   SpecularPacking;    // 1 = MR map (t2) is a "Specular" map: R=AO, G=Roughness, B=Metalness
    uint   AlphaTest;          // 1 = clip by BaseColor opacity (foliage/awnings)
    float  AlphaCutoff;
    uint   NormalReconstructZ; // 1 = BC5 normal map (RG only) → reconstruct Z; Toksvig forced to 1

    // Shading model: 0 = DefaultLit, 1 = Foliage (transmissao two-sided estilo Unreal).
    uint   ShadingModel;
    // Transmissao da folha: rgb = tint (1,1,1 => usa o albedo), a = forca (0 = off).
    float4 SubsurfaceColor;
};

#define SHADINGMODEL_DEFAULTLIT 0u
#define SHADINGMODEL_FOLIAGE    1u

// --- Texture Slots ---

Texture2D AlbedoMap            : register(t0);
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2); // R=Metallic, G=Roughness (glTF convention)
Texture2D AOMap                : register(t3);
Texture2D EmissiveMap          : register(t4);
Texture2D HeightMap            : register(t5); // R = height [0,1] (1 = surface, 0 = deepest)
Texture2D MetalnessMap         : register(t6); // R = metalness (separate from combined MR)
Texture2D RoughnessMap         : register(t7); // R = roughness  (separate from combined MR)

TextureCube IrradianceMap      : register(t8);  // diffuse irradiance cube
TextureCube PrefilteredMap     : register(t9);  // GGX prefiltered specular cube (mip = roughness * maxMip)
Texture2D   BRDFLut            : register(t10); // Karis split-sum LUT (RG16F: F0 scale, F0 bias)
// t11 = SunShadowMap (CSMCommon.hlsli)
Texture2D<float4> DDGIIrradianceAtlas : register(t12); // DDGI: atlas octaedrico de irradiancia
Texture2D<float4> DDGIDistanceAtlas   : register(t13); // DDGI: atlas de distancia (Chebyshev, Fase 2)
Buffer<float4>    DDGIProbeData        : register(t15); // DDGI: xyz=offset relocacao, w=state (0/1)
Texture2D<float>  SceneAO             : register(t14); // GTAO: oclusao de contato (screen-space)

SamplerState MaterialSampler   : register(s0); // anisotropic wrap (materials)
SamplerState IBLSampler        : register(s1); // trilinear clamp   (cube + LUT)

// --- Input ---

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    // Gerado pela rasterizacao: true na face da frente, false na de tras. So varia em
    // geometria two-sided (folhagem/toldos do Bistro), onde a face de tras precisa ter a
    // normal invertida — senao metade das folhas fica com NdotL negativo = preta/chapada.
    bool   frontFace   : SV_IsFrontFace;
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
    // Engine D3D (origem da textura no topo-esquerda) + tangente derivada de ∂P/∂V:
    // nessa combinação o verde "+Y" de um normal map GL (padrão glTF/AmbientCG) aponta
    // pro topo da imagem, oposto ao bitangent (que segue +V = base). Logo o GL PRECISA
    // do flip do verde, e o DirectX NÃO. Por isso o flip é o DEFAULT e só desliga quando
    // o material marca que o map é DirectX (NormalFlipY=1).
    if (NormalFlipY == 0) n.y = -n.y;
    n.xy *= NormalStrength;
    return normalize(mul(n, TBN));
}

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

float3 FoliageTransmission(float3 N, float3 V, float3 L, float3 Radiance, float3 TransColor) {
    const float Wrap    = 0.5f;
    float  WrapNoL = saturate((-dot(N, L) + Wrap) / ((1.0f + Wrap) * (1.0f + Wrap)));
    float  VoL     = dot(V, L);
    float  Scatter = D_GGX(0.6f * 0.6f, saturate(-VoL)); // pico ao olhar pro sol pela folha
    return Radiance * (WrapNoL * Scatter) * TransColor;
}

float3 BRDF_Direct(float3 N, float3 V, float3 L, float3 Radiance,
                   float3 DiffuseColor, float3 SpecularColor,
                   float Metallic, float Roughness, float a2, float3 TransColor) {
    float3 Result = float3(0.0f, 0.0f, 0.0f);
    
    float NoL = saturate(dot(N, L));
    if (NoL > 0.0f) {
        float3 H   = normalize(L + V);
        float  NoV = saturate(dot(N, V));
        float  NoH = saturate(dot(N, H));
        float  VoH = saturate(dot(V, H));

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
            float3 W = 1.0f + SpecularColor * ((1.0f - E_val) / max(E_val, 1e-5f));
            Specular *= W;

            // 2. Diffuse-Specular energy preservation (split-sum)
            float3 F90    = saturate(50.0f * SpecularColor.g);
            float3 E_spec = W * (E_val * SpecularColor + Ef_val * (F90 - SpecularColor));
            float  DiffuseScale = 1.0f - saturate(dot(E_spec, float3(0.2126f, 0.7152f, 0.0722f)));
            Kd *= DiffuseScale;
        #else
            Kd *= (1.0f - F);
        #endif

        #if USE_BURLEY_DIFFUSE
            float3 Diffuse = Kd * Diffuse_Burley(DiffuseColor, Roughness, NoV, NoL, VoH);
        #else
            float3 Diffuse = (Kd * DiffuseColor) / PI;
        #endif

        Result += (Diffuse + Specular) * Radiance * NoL;
    }

    // --- Transmissao de folhagem (luz atravessando por trás) ---
    if (any(TransColor > 0.0f))
        Result += FoliageTransmission(N, V, L, Radiance, TransColor);

    return Result;
}


PSOutput main(PSInput input) {
    float3 GeoN = normalize(input.worldNormal);

    if (!input.frontFace) GeoN = -GeoN;

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

    // --- Base Color (+ alpha test for masked materials: BaseColor.a = opacity) ---
    float4 AlbedoSample = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, UV) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 BaseColor    = BaseColorFactor.rgb * AlbedoSample.rgb;
    if (AlphaTest)
        clip(AlbedoSample.a * BaseColorFactor.a - AlphaCutoff);

    // --- Normal ---
    float3 N         = GeoN;
    float  ToksvigT  = 1.0f; // Toksvig factor from normal map .a; 1.0 = no per-mip variance
    if (HasNormalMap) {
        float4 NrmSample = NormalMap.Sample(MaterialSampler, UV);
        float3 Encoded   = NrmSample.rgb;
        if (NormalReconstructZ) {
            // BC5 stores only X,Y in RG. Decode to [-1,1], rebuild Z, re-encode to [0,1]
            // so ApplyNormalMap (which does *2-1 internally) gets a consistent input.
            float2 xy = NrmSample.rg * 2.0f - 1.0f;
            float  z  = sqrt(saturate(1.0f - dot(xy, xy)));
            Encoded   = float3(NrmSample.rg, z * 0.5f + 0.5f);
            ToksvigT  = 1.0f; // BC5 has no alpha → no Toksvig variance
        } else {
            ToksvigT = NrmSample.a;
        }
        N = ApplyNormalMap(GeoN, T, B, Encoded);
    }

    // --- Metallic / Roughness ---
    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float4 MR = MetallicRoughnessMap.Sample(MaterialSampler, UV);
        if (SpecularPacking) {
            // Bistro "Specular": R=AO (handled in AO section), G=Roughness, B=Metalness
            Roughness *= MR.g;
            Metallic  *= MR.b;
        } else {
            Metallic  *= MR.r; // glTF convention: R=Metallic, G=Roughness
            Roughness *= MR.g;
        }
    }

    if (HasMetalnessMap) Metallic  *= MetalnessMap.Sample(MaterialSampler, UV).r;
    if (HasRoughnessMap) Roughness *= RoughnessMap.Sample(MaterialSampler, UV).r;
    Roughness = max(Roughness, 0.04f); 

    {
        float3 dNdx = ddx(N);
        float3 dNdy = ddy(N);
        float variance         = 0.25f * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        float kernelRoughness2 = min(2.0f * variance, 0.25f);
        float toksvigVar       = 1.0f - ToksvigT * ToksvigT; // 0 when T=1
        float aLin             = Roughness * Roughness;      // perceptual² = α (GGX linear)
        float a2New            = saturate(aLin * aLin + kernelRoughness2 + toksvigVar);
        Roughness              = sqrt(sqrt(a2New));          // back to perceptual
    }

    // --- Ambient Occlusion ---
    float AO = 1.0f;
    if (SpecularPacking && HasMetallicRoughnessMap)
        AO = lerp(1.0f, MetallicRoughnessMap.Sample(MaterialSampler, UV).r, AOStrength); // Bistro: AO em R
    else if (HasAOMap)
        AO = lerp(1.0f, AOMap.Sample(MaterialSampler, UV).r, AOStrength);

    float ssao  = (AlphaTest != 0u) ? 1.0f : SceneAO.Load(int3((int2)input.pos.xy, 0));
    float AOAll = AO * ssao;

    if (Time.w > 0.5f) { PSOutput dbg; dbg.color = float4(ssao, ssao, ssao, 1.0f); dbg.gbuffer = float4(0, 0, 1, 0); return dbg; }

    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    float3 TransColor = (ShadingModel == SHADINGMODEL_FOLIAGE)
        ? BaseColor * SubsurfaceColor.rgb * SubsurfaceColor.a
        : float3(0.0f, 0.0f, 0.0f);

    float a   = Roughness * Roughness;
    float a2  = a * a;
    float NoV = saturate(dot(N, V)); // shared with the ambient/IBL block below

    float ReflectCombine = ReflectionParams.z > 0.5f
        ? saturate((ReflectionParams.x - Roughness) / max(ReflectionParams.y, 1e-4f))
        : 0.0f;
    float SpecAmbientScale = 1.0f - ReflectCombine;

    float3 Lighting = float3(0.0f, 0.0f, 0.0f);
    {
        float3 Lsun        = normalize(SunDirection.xyz);
        float3 SunRadiance = SunColor.rgb * SunDirection.w; // w = intensity
        float3 SunLit      = BRDF_Direct(N, V, Lsun, SunRadiance,
                                         DiffuseColor, SpecularColor, Metallic, Roughness, a2, TransColor);
        // POM soft self-shadow toward the sun.
        if (ParallaxSelfShadow && HasHeightMap && ParallaxFadeF > 0.0f && dot(GeoN, Lsun) > 0.0f) {
            float3 Lts = float3(dot(Lsun, T), dot(Lsun, B), dot(Lsun, GeoN));
            SunLit    *= TraceParallaxSelfShadow(UV, SurfaceHeight, Lts, dUVdx, dUVdy, ParallaxFadeF);
        }

        float3 MoonLit = float3(0.0f, 0.0f, 0.0f);
        if (MoonDirection.w > 0.0f) {
            float3 Lmoon        = normalize(MoonDirection.xyz);
            float3 MoonRadiance = MoonColor.rgb * MoonDirection.w;
            MoonLit = BRDF_Direct(N, V, Lmoon, MoonRadiance,
                                  DiffuseColor, SpecularColor, Metallic, Roughness, a2, TransColor);
        }

        Lighting += (SunLit + MoonLit) * SampleCSM(input.worldPos, GeoN, input.pos.xy);
    }

    // --- Ambient ---
    float3 Ambient = float3(0.0f, 0.0f, 0.0f);

    bool UseGI = DDGIGridCount.w > 0.5f;

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

        float3 F        = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        float3 KdIBL    = (1.0f - F) * (1.0f - Metallic);

        float3 Irradiance  = IrradianceMap.SampleLevel(IBLSampler, RotN, 0.0f).rgb;
        float3 DiffuseIBL  = KdIBL * DiffuseColor * Irradiance;

        float  Mip         = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y) * SpecAmbientScale;

        float3 IBLContrib = (UseGI || UseAtmoAmbient) ? SpecularIBL : (DiffuseIBL + SpecularIBL);
        Ambient += IBLContrib * AOAll * IBLParams.x;
    }

    // --- DDGI (difusa indireta) ---
    float3 DbgGI = float3(0.0f, 0.0f, 0.0f);
    if (UseGI) {
        float2 atlasInvSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
        // DDGIDistParams.w empacota flags: bit0 = Chebyshev, bit1 = pular inativas, bit2 = fallback.
        int  giFlags      = (int)DDGIDistParams.w;
        bool useChebyshev = (giFlags & 1) != 0;
        bool skip         = (giFlags & 2) != 0;
        bool fallback     = (giFlags & 4) != 0;
        uint skipMode     = skip ? (fallback ? 2u : 1u) : 0u;
        float3 gi;
        if (useChebyshev) {
            // Fase 2: Chebyshev (usa o atlas de distancia p/ matar o light-leak).
            float2 distInvSize = float2(1.0f / DDGIDistParams.y, 1.0f / DDGIDistParams.z);
            gi = SampleDDGIIrradianceCheb(DDGIIrradianceAtlas, DDGIDistanceAtlas, IBLSampler,
                     input.worldPos, N, DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                     (int)DDGIParams.y, atlasInvSize, (int)DDGIDistParams.x, distInvSize, 0.25f,
                     DDGIProbeData, skipMode);
        } else {
            gi = SampleDDGIIrradiance(DDGIIrradianceAtlas, IBLSampler, input.worldPos, N,
                     DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                     (int)DDGIParams.y, atlasInvSize);
        }
        DbgGI = gi; 
        
        float3 KdGI = (1.0f - Metallic);
        Ambient += KdGI * DiffuseColor * gi * DDGIParams.x * AOAll;

        if (IBLParams.w <= 0.5f) {
            float3 Fa = F_SchlickRoughness(SpecularColor, NoV, Roughness);
            Ambient += Fa * gi * DDGIParams.x * AOAll * SpecAmbientScale;
        }
    }

    // --- Emissive ---
    float3 Emissive = EmissiveFactor.rgb * EmissiveStrength;
    if (HasEmissiveMap)
        Emissive *= EmissiveMap.Sample(MaterialSampler, UV).rgb;

    // --- Compose and tonemap ---
    float3 FinalColor = Lighting + Ambient + Emissive;

    // Debug DDGI: mostra a irradiancia dos probes direto (DDGIGridCount.w == 2). Preto =
    // atlas vazio (trace/update); colorido = atlas com conteudo (bug na injecao/magnitude).
    if (DDGIGridCount.w > 1.5f) {
        PSOutput dbg; dbg.color = float4(DbgGI, 1.0f); dbg.gbuffer = float4(0, 0, 1, 0); return dbg;
    }

    // Debug: overlay de cor chapada por cascata (estilo Unreal/Cry) — independente do brilho
    // da cena, pra enxergar as faixas mesmo no escuro. Fora de todas as cascatas: sem tint.
    if (CSM_DebugEnabled()) {
        int ci = CSM_SelectCascade(input.worldPos, GeoN);
        if (ci >= 0)
            FinalColor = lerp(FinalColor, CSM_CascadeColor(ci), 0.45f);
    }

    // MRT: cor HDR + G-buffer de reflexao (normal de shading octa-encoded, roughness, metallic).
    PSOutput o;
    o.color   = float4(FinalColor, 1.0f);
    o.gbuffer = float4(DDGI_OctEncode(N) * 0.5f + 0.5f, Roughness, Metallic);
    return o;
}
