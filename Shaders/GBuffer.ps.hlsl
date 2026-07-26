#include "GBuffer.hlsli"

cbuffer FrameCB : register(b0) {
    float4 CameraPosition : packoffset(c0);
    // x = mip bias global (FSR upscale: log2(render/display) - 1; 0 sem upscale).
    // c18 = logo apos o InvViewProj do FrameConstants (ver Renderer.h).
    float4 RenderParams   : packoffset(c18);
};

#include "MaterialCB.hlsli"

Texture2D AlbedoMap            : register(t0);
Texture2D NormalMap            : register(t1);
Texture2D MetallicRoughnessMap : register(t2);
Texture2D AOMap                : register(t3);
Texture2D EmissiveMap          : register(t4);
Texture2D HeightMap            : register(t5);
Texture2D MetalnessMap         : register(t6);
Texture2D RoughnessMap         : register(t7);

SamplerState MaterialSampler   : register(s0);

struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    float4 curClip     : TEXCOORD3; 
    float4 prevClip    : TEXCOORD4; 
    bool   frontFace   : SV_IsFrontFace;
};

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

GBufferOutput main(PSInput input) {
    float3 GeoN = normalize(input.worldNormal);
    if (!input.frontFace) GeoN = -GeoN;

    float3 dPdx  = ddx(input.worldPos);
    float3 dPdy  = ddy(input.worldPos);
    float2 dUVdx = ddx(input.uv);
    float2 dUVdy = ddy(input.uv);
    float3 T, B;
    BuildTangentBasis(GeoN, dPdx, dPdy, dUVdx, dUVdy, T, B);

    float3 V = normalize(CameraPosition.xyz - input.worldPos);

    float2 UV = input.uv;
    if (HasHeightMap) {
        float ParallaxFadeF = ParallaxFade(dUVdx, dUVdy);
        if (ParallaxFadeF > 0.0f) {
            float3 Vts = float3(dot(V, T), dot(V, B), dot(V, GeoN));
            UV = ParallaxOcclusionMapping(input.uv, Vts, dUVdx, dUVdy, ParallaxFadeF);
        }
    }

    // Bias negativo de mip nas texturas de material quando o FSR upscala (recupera o detalhe
    // que a render res menor perderia); 0 em nativo/SSAA. Nao se aplica ao parallax (SampleGrad).
    float MipBias = RenderParams.x;

    float4 AlbedoSample = HasAlbedoMap ? AlbedoMap.SampleBias(MaterialSampler, UV, MipBias) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 BaseColor    = BaseColorFactor.rgb * AlbedoSample.rgb;
    if (AlphaTest) {
        // A cobertura do recorte (alfa) NAO usa o MipBias negativo do FSR: puxar o alfa p/ um
        // mip mais nitido deixa a borda da folhagem alta-frequencia e a cobertura instavel entre
        // frames com jitter -> cintilacao no upscale (Qualidade+). O bias fica so no RGB (detalhe);
        // o alfa e amostrado no mip da render res (bias 0), mantendo a silhueta estavel p/ o FSR.
        // Sem upscale (MipBias==0) reaproveita AlbedoSample.a — sem fetch extra.
        float ClipAlpha = (MipBias < 0.0f && HasAlbedoMap)
                        ? AlbedoMap.SampleBias(MaterialSampler, UV, 0.0f).a
                        : AlbedoSample.a;
        MaterialAlphaClip(ClipAlpha);
    }

    float3 N        = GeoN;
    float  ToksvigT = 1.0f;
    if (HasNormalMap) {
        float4 NrmSample = NormalMap.SampleBias(MaterialSampler, UV, MipBias);
        float3 Encoded   = NrmSample.rgb;
        if (NormalReconstructZ) {
            float2 xy = NrmSample.rg * 2.0f - 1.0f;
            float  z  = sqrt(saturate(1.0f - dot(xy, xy)));
            Encoded   = float3(NrmSample.rg, z * 0.5f + 0.5f);
            ToksvigT  = 1.0f;
        } else {
            ToksvigT = NrmSample.a;
        }
        N = ApplyNormalMap(GeoN, T, B, Encoded);
    }

    float Metallic  = MetallicFactor;
    float Roughness = RoughnessFactor;
    if (HasMetallicRoughnessMap) {
        float4 MR = MetallicRoughnessMap.SampleBias(MaterialSampler, UV, MipBias);
        if (SpecularPacking) {
            Roughness *= MR.g;
            Metallic  *= MR.b;
        } else {
            Metallic  *= MR.r;
            Roughness *= MR.g;
        }
    }
    if (HasMetalnessMap) Metallic  *= MetalnessMap.SampleBias(MaterialSampler, UV, MipBias).r;
    if (HasRoughnessMap) Roughness *= RoughnessMap.SampleBias(MaterialSampler, UV, MipBias).r;
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
        AO = lerp(1.0f, MetallicRoughnessMap.SampleBias(MaterialSampler, UV, MipBias).r, AOStrength);
    else if (HasAOMap)
        AO = lerp(1.0f, AOMap.SampleBias(MaterialSampler, UV, MipBias).r, AOStrength);

    float3 Emissive = EmissiveFactor.rgb * EmissiveStrength;
    if (HasEmissiveMap)
        Emissive *= EmissiveMap.SampleBias(MaterialSampler, UV, MipBias).rgb;

    // Subsurface premultiplicado (tint x intensidade) — vai no C.gba; o deferred faz
    // TransColor = BaseColor * isto. Default do loader p/ folhagem = (1,1,1)x0.6.
    float3 Subsurface = SubsurfaceColor.rgb * SubsurfaceColor.a;

    GBufferOutput o = EncodeGBuffer(BaseColor, AO, N, Roughness, Metallic, Emissive, ShadingModel,
                                    Subsurface);

    float2 curNDC  = input.curClip.xy  / input.curClip.w;
    float2 prevNDC = input.prevClip.xy / input.prevClip.w;
    float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    return o;
}
