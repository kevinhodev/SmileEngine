// PS do preview de material (Editor de Materiais): forward PBR isolado — sol fixo + IBL do
// HDRI PROPRIO do preview (irradiance t8 / prefiltered t9 / BRDF LUT t10). A avaliacao de
// material (TBN por derivadas, POM, packing MR/Specular, Toksvig) e um porte 1:1 do
// GBuffer.ps — o que o preview mostra e o que a cena desenha. Tonemap ACES + gamma no fim
// (o RT e RGBA8 pra readback direto do editor).

cbuffer PreviewCB : register(b0) {
    row_major float4x4 MVP;
    row_major float4x4 Model;
    float4 CameraPos;
    float4 SunDirIntensity; // xyz = direcao PARA o sol, w = intensidade
    float4 SunColor;
    float4 IBLParams;       // x = intensidade, y = rotacao do env (rad), z = mip max, w = enabled
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
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2);
Texture2D AOMap                : register(t3);
Texture2D EmissiveMap          : register(t4);
Texture2D HeightMap            : register(t5);
Texture2D MetalnessMap         : register(t6);
Texture2D RoughnessMap         : register(t7);

TextureCube IrradianceMap      : register(t8);
TextureCube PrefilteredMap     : register(t9);
Texture2D   BRDFLut            : register(t10);

SamplerState MaterialSampler   : register(s0);
SamplerState IBLSampler        : register(s1);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    bool   frontFace   : SV_IsFrontFace;
};

// ---- Helpers de material (porte do GBuffer.ps) ----
void BuildTangentBasis(float3 N, float3 dPdx, float3 dPdy, float2 dUVdx, float2 dUVdy,
                       out float3 T, out float3 B) {
    T = normalize(dUVdy.y * dPdx - dUVdx.y * dPdy);
    B = normalize(dUVdx.x * dPdy - dUVdy.x * dPdx);
    T = normalize(T - N * dot(N, T));
    B = normalize(cross(N, T));
}

float3 ApplyNormalMap(float3 N, float3 T, float3 B, float3 SampledNormal) {
    float3x3 TBN = float3x3(T, B, N);
    float3 n = SampledNormal * 2.0f - 1.0f;
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

float2 ParallaxOcclusionMapping(float2 uv, float3 Vts, float2 dUVdx, float2 dUVdy, float fade) {
    float numSteps = lerp(ParallaxMaxSteps, ParallaxMinSteps, saturate(Vts.z));
    float stepSize = 1.0f / numSteps;

    float2 maxOffset = -(Vts.xy / max(Vts.z, 1e-4f)) * (HeightScale * fade);
    float2 deltaUV   = maxOffset * stepSize;

    float  rayHeight  = 1.0f;
    float  lastHeight = 1.0f;
    float2 currOffset = 0.0f;
    float2 lastOffset = 0.0f;
    float  currHeight = HeightMap.SampleGrad(MaterialSampler, uv, dUVdx, dUVdy).r;

    [loop]
    for (int i = 0; i < (int)numSteps; ++i) {
        if (currHeight >= rayHeight) break;
        lastOffset = currOffset;  lastHeight = currHeight;
        rayHeight -= stepSize;    currOffset += deltaUV;
        currHeight = HeightMap.SampleGrad(MaterialSampler, uv + currOffset, dUVdx, dUVdy).r;
    }

    float2 finalOffset;
    if (ParallaxRefine) {
        float tLow = 0.0f, tHigh = 1.0f;
        [loop]
        for (int j = 0; j < (int)ParallaxRefineSteps; ++j) {
            float  tMid   = 0.5f * (tLow + tHigh);
            float2 offMid = lerp(lastOffset, currOffset, tMid);
            float  rayMid = rayHeight + stepSize * (1.0f - tMid);
            float  hMid   = HeightMap.SampleGrad(MaterialSampler, uv + offMid, dUVdx, dUVdy).r;
            if (hMid >= rayMid) tHigh = tMid; else tLow = tMid;
        }
        float t     = 0.5f * (tLow + tHigh);
        finalOffset = lerp(lastOffset, currOffset, t);
    } else {
        float afterDepth  = currHeight - rayHeight;
        float beforeDepth = lastHeight - (rayHeight + stepSize);
        float t           = afterDepth / max(afterDepth - beforeDepth, 1e-5f);
        finalOffset       = lerp(currOffset, lastOffset, t);
    }
    return uv + finalOffset;
}

// ---- Helpers de shading (BRDF minimo autocontido) ----
static const float PI = 3.14159265f;

float3 RotateY(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 F_Schlick(float3 F0, float VoH) {
    return F0 + (1.0f - F0) * pow(1.0f - VoH, 5.0f);
}

float3 F_SchlickRoughness(float3 F0, float NoV, float Roughness) {
    return F0 + (max(float3(1.0f - Roughness, 1.0f - Roughness, 1.0f - Roughness), F0) - F0)
              * pow(1.0f - NoV, 5.0f);
}

float D_GGX(float NoH, float a2) {
    float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float V_SmithJointApprox(float NoV, float NoL, float a) {
    float SmithV = NoL * (NoV * (1.0f - a) + a);
    float SmithL = NoV * (NoL * (1.0f - a) + a);
    return 0.5f / max(SmithV + SmithL, 1e-5f);
}

float3 ACESFilm(float3 x) {
    // Narkowicz ACES fit — mesmo espirito do tonemap principal, bom o bastante pro preview.
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

float4 main(PSInput input) : SV_Target {
    float3 GeoN = normalize(input.worldNormal);
    if (!input.frontFace) GeoN = -GeoN;

    float3 dPdx  = ddx(input.worldPos);
    float3 dPdy  = ddy(input.worldPos);
    float2 dUVdx = ddx(input.uv);
    float2 dUVdy = ddy(input.uv);
    float3 T, B;
    BuildTangentBasis(GeoN, dPdx, dPdy, dUVdx, dUVdy, T, B);

    float3 V = normalize(CameraPos.xyz - input.worldPos);

    float2 UV = input.uv;
    if (HasHeightMap) {
        float ParallaxFadeF = ParallaxFade(dUVdx, dUVdy);
        if (ParallaxFadeF > 0.0f) {
            float3 Vts = float3(dot(V, T), dot(V, B), dot(V, GeoN));
            UV = ParallaxOcclusionMapping(input.uv, Vts, dUVdx, dUVdy, ParallaxFadeF);
        }
    }

    float4 AlbedoSample = HasAlbedoMap ? AlbedoMap.Sample(MaterialSampler, UV)
                                       : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 BaseColor    = BaseColorFactor.rgb * AlbedoSample.rgb;
    if (AlphaTest)
        clip(AlbedoSample.a * BaseColorFactor.a - AlphaCutoff);

    float3 N        = GeoN;
    float  ToksvigT = 1.0f;
    if (HasNormalMap) {
        float4 NrmSample = NormalMap.Sample(MaterialSampler, UV);
        float3 Encoded   = NrmSample.rgb;
        if (NormalReconstructZ) {
            float2 xy = NrmSample.rg * 2.0f - 1.0f;
            float  z  = sqrt(saturate(1.0f - dot(xy, xy)));
            Encoded   = float3(NrmSample.rg, z * 0.5f + 0.5f);
        } else {
            ToksvigT = NrmSample.a;
        }
        N = ApplyNormalMap(GeoN, T, B, Encoded);
    }

    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float4 MR = MetallicRoughnessMap.Sample(MaterialSampler, UV);
        if (SpecularPacking) {
            Roughness *= MR.g;
            Metallic  *= MR.b;
        } else {
            Metallic  *= MR.r;
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
        float toksvigVar       = 1.0f - ToksvigT * ToksvigT;
        float aLin             = Roughness * Roughness;
        float a2New            = saturate(aLin * aLin + kernelRoughness2 + toksvigVar);
        Roughness              = sqrt(sqrt(a2New));
    }

    float AO = 1.0f;
    if (SpecularPacking && HasMetallicRoughnessMap)
        AO = lerp(1.0f, MetallicRoughnessMap.Sample(MaterialSampler, UV).r, AOStrength);
    else if (HasAOMap)
        AO = lerp(1.0f, AOMap.Sample(MaterialSampler, UV).r, AOStrength);

    float3 Emissive = EmissiveFactor.rgb * EmissiveStrength;
    if (HasEmissiveMap)
        Emissive *= EmissiveMap.Sample(MaterialSampler, UV).rgb;

    // ---- Shading: sol direto + IBL ----
    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    float3 L   = normalize(SunDirIntensity.xyz);
    float3 H   = normalize(V + L);
    float  NoL = saturate(dot(N, L));
    float  NoV = saturate(abs(dot(N, V)) + 1e-5f);
    float  NoH = saturate(dot(N, H));
    float  VoH = saturate(dot(V, H));
    float  a   = Roughness * Roughness;
    float  a2  = a * a;

    float3 SunRadiance = SunColor.rgb * SunDirIntensity.w;

    float3 Direct = 0.0f;
    if (NoL > 0.0f) {
        float3 F    = F_Schlick(SpecularColor, VoH);
        float  D    = D_GGX(NoH, a2);
        float  Vis  = V_SmithJointApprox(NoV, NoL, a);
        float3 Spec = D * Vis * F;
        Direct = (DiffuseColor / PI + Spec) * SunRadiance * NoL;
    }

    // Folhagem: transmissao wrap simples (luz atravessando a folha) — aproximacao do
    // two-sided do deferred, suficiente pro preview.
    if (ShadingModel == 1) {
        float Wrap  = saturate(dot(-N, L) * 0.6f + 0.4f);
        Direct += DiffuseColor * SubsurfaceColor.rgb * SubsurfaceColor.w
                * SunRadiance * Wrap / PI;
    }

    float3 Ambient = 0.0f;
    if (IBLParams.w > 0.5f) {
        float3 R    = reflect(-V, N);
        float3 RotN = RotateY(N, IBLParams.y);
        float3 RotR = RotateY(R, IBLParams.y);

        float3 F     = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        float3 KdIBL = (1.0f - F) * (1.0f - Metallic);

        float3 Irradiance  = IrradianceMap.SampleLevel(IBLSampler, RotN, 0.0f).rgb;
        float3 DiffuseIBL  = KdIBL * BaseColor * Irradiance;

        float  Mip         = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y);

        Ambient = (DiffuseIBL + SpecularIBL) * AO * IBLParams.x;
    }

    float3 Color = Direct + Ambient + Emissive;
    Color = ACESFilm(Color);
    Color = pow(Color, 1.0f / 2.2f);
    return float4(Color, 1.0f);
}
