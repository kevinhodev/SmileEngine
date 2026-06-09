#ifndef SMILE_WATER_COMMON_HLSLI
#define SMILE_WATER_COMMON_HLSLI

// CB do oceano — casa campo-a-campo com FWaterRenderer::WaterConstants (Water.h).
// Matrizes row_major: convencao row-vector da engine (mul(v, M)).
cbuffer WaterCB : register(b0) {
    row_major float4x4 ViewProj;
    row_major float4x4 InvViewProj;
    float4 CameraPos;        // xyz, w=1
    float4 SunDirection;     // xyz=dir TO sun (norm), w=intensity
    float4 SunColor;         // rgb, w=waterClarity(m)
    float4 OceanParams0;     // x=windDir(rad) y=windSpeed z=shoreFoamIntensity w=wavesAmount
    float4 OceanParams1;     // x=wavesSize y=FlowDir.x z=FlowDir.y w=waterLevel
    float4 DeepColorDensity; // rgb=cor de fundo, w=fogDensity (futuro)
    float4 Misc;             // x=time y=iblEnabled z=iblIntensity w=specMaxMip
    float4 WaterFXParams;    // x=sssStrength y=sssPower z=sssHeightScale w=shoreFoamWidth(m)
    float4 ShadeParams;      // x=FresnelGloss y=ReflectionScale z=SunShininess w=SunSpecScale
    float4 OceanFFT;         // x=useFFT y=dispScale z=choppyScale w=normalUp
    float4 OceanFade;        // x=fadeStart(m) y=fadeRange(m)
    float4 BumpParams;       // x=Tilling y=DetailTilling z=NormalsScale w=DetailNormalsScale
    float4 BumpParams2;      // x=bumpStrength y=parallaxHeight z=useBump w=bumpFadeDist
    float4 ScreenParams;     // x=w y=h z=1/w w=1/h
    float4 DepthParams;      // x=near y=far z=hasSceneCopies w=useAtmosphereSky
    float4 RefractionParams; // x=RefractionBumpScale y=SoftIntersectionFactor z=fogDensity w=ReflectionBumpScale
    float4 AerialFogParams;  // x=start(m) y=range(m) z=density w=0 off/1 HDR/2 physical sky
    float4 FarBlendParams;   // x=FFT->swell start(m) y=range(m) z=swell height w=swell normal strength
    float4 DebugParams;      // x=debug mode (0 off/1 wire/2 tiles)
    float4 InScatterColor;   // rgb=cor turquesa do in-scatter, w=densidade
    float4 AbsorptionColor;  // rgb=extincao por canal (Beer-Lambert), w=clamp do sun-spec
    float4 FoamParams;       // x=coverage(limiar J) y=sharpness z=intensidade(0=off) w=fadeDist(m)
    float4 FoamColor;        // rgb=tint da espuma, w=supressao do sun-spec
    float4 QuadTreeParams;   // x=rootX y=rootZ z=leafSize w=rootSize
};

// t1 = displacement do FFT (Dx, Dz, -h, J). s0 = linear wrap.
Texture2D<float4> FFTDisplacement : register(t1);
SamplerState      LinearWrap      : register(s0);

// Mapeamento world-space -> UV base da textura FFT. Fonte UNICA: VS (deslocamento), PS
// (foam/debug) e funcoes de detalhe devem usar a mesma escala para manter alinhamento.
float2 WaterFFTSampleUV(float2 worldXZ) {
    return worldXZ * 0.0125 * OceanParams0.w * 1.25;
}

float4 WaterSampleFFTUv(float2 baseUV) {
    return FFTDisplacement.SampleLevel(LinearWrap, baseUV, 0.0);
}

float4 WaterSampleFFT(float2 worldXZ) {
    return WaterSampleFFTUv(WaterFFTSampleUV(worldXZ));
}

float3 WaterFFTNormalFromDisplacement(float2 worldXZ, float worldStep, float normalUp) {
    float2 e = float2(max(worldStep, 0.05), 0.0);
    float hL = WaterSampleFFT(worldXZ - e.xy).z;
    float hR = WaterSampleFFT(worldXZ + e.xy).z;
    float hD = WaterSampleFFT(worldXZ - e.yx).z;
    float hU = WaterSampleFFT(worldXZ + e.yx).z;
    return normalize(float3(hL - hR, max(normalUp * 0.65, 1.0), hD - hU));
}

// t4 = normal-map dinamico derivado do FFT (com mip chain Toksvig).
// RGB fica em espaco-mundo Y-up assinado; A guarda Toksvig T da mip chain.
Texture2D<float4> WaterNormalTex : register(t4);
SamplerState      AnisoWrap      : register(s2);

// t2/t3 = copias da cena ANTES da agua (refracao/fog). scene-color e HDR LINEAR
// (sem inverse-tonemap, especificidade Smile); scene-depth e a profundidade NDC linearizavel.
Texture2D<float4> SceneColor : register(t2);
Texture2D<float>  SceneDepth : register(t3);

// Profundidade NDC[0,1] (LH) -> view-space Z (linear). Mesma proj de PerspectiveFovLH.
float LinearizeDepth(float ndcZ) {
    float n = DepthParams.x, f = DepthParams.y;
    return n * f / (f - ndcZ * (f - n));
}

// Profundidade linear da cena no UV de tela (point/Load — sem interpolar bordas).
float SampleSceneDepthLin(float2 uv) {
    int2 px = clamp(int2(uv * ScreenParams.xy), int2(0, 0), int2(ScreenParams.xy) - 1);
    return LinearizeDepth(SceneDepth.Load(int3(px, 0)));
}

struct VSOutput {
    float4 pos        : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 vView      : TEXCOORD2; // vetor de visao em mundo (cam - pos), nao-normalizado
    float4 screenProj : TEXCOORD3; // xy=uv de tela, w=clip.w, z=escala de prof. (refracao futura)
    float4 baseTC     : TEXCOORD4; // .xy=bump low-freq, .zw=bump high-freq (com scroll por vento)
    float4 debugData  : TEXCOORD5; // x=tile size y=subset pattern z=internal LOD w=geomorph
    float2 tileUV     : TEXCOORD6; // uv local estavel do tile para debug de quadtree
};

// Fresnel de Schlick. F0 da agua ~ 0.02 (indice ~1.33). FresnelGloss aguca a curva.
float FresnelSchlick(float F0, float cosTheta, float gloss) {
    float f = pow(saturate(1.0 - cosTheta), 5.0);
    // gloss alto -> curva mais nitida (menos Fresnel em angulo raso interno).
    return saturate(F0 + (1.0 - F0) * f * lerp(0.5, 1.0, gloss));
}

// Ceu analitico simples (fallback quando nao ha HDR carregado p/ o cubemap especular).
// Gradiente horizonte->zenite + leve realce na direcao do sol.
float3 AnalyticSky(float3 dir) {
    float up = saturate(dir.y * 0.5 + 0.5);
    float3 horizon = float3(0.52, 0.62, 0.72);
    float3 zenith  = float3(0.09, 0.24, 0.52);
    float3 sky = lerp(horizon, zenith, up);
    float sun = saturate(dot(normalize(dir), normalize(SunDirection.xyz)));
    sky += SunColor.rgb * pow(sun, 64.0) * 0.5;
    return sky;
}

// Normal de detalhe filtrada. A amostragem implicita deixa o hardware escolher mip/anisotropia
// pelo footprint de tela; o alpha carrega Toksvig T para roughness/specular AA.
float4 SampleWaterNormalGrad(float2 uv, float2 dx, float2 dy) {
    float4 n = WaterNormalTex.SampleGrad(AnisoWrap, uv, dx, dy);
    n.xyz = normalize(n.xyz);
    n.w = saturate(n.w);
    return n;
}

float SmoothWaterStep(float x) {
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}

// Fade suavizado por distancia: 1 perto, cai a 0 alem de (start + range). Padrao usado
// por todos os anti-shimmer/anti-aliasing de horizonte (deslocamento, normal, detalhe).
float WaterDistanceFade(float dist, float start, float range) {
    return SmoothWaterStep(saturate(1.0 - (dist - start) / max(range, 1.0)));
}

// Fade da normal de detalhe no horizonte: start/range derivados do OceanFade, com pisos
// para a normal nao tremer no longe mesmo com OceanFade curto.
float WaterNormalFade(float dist) {
    float start = max(OceanFade.x * 2.5, 1000.0);
    float range = max(OceanFade.y * 3.0, 4500.0);
    return WaterDistanceFade(dist, start, range);
}

// Value-noise barato (hash 2D + bilinear suavizado). Usado p/ quebrar a borda da espuma,
// que senao fica chapada e revela o limiar do Jacobiano.
float WaterHash21(float2 p) {
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}
float WaterValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = WaterHash21(i);
    float b = WaterHash21(i + float2(1.0, 0.0));
    float c = WaterHash21(i + float2(0.0, 1.0));
    float d = WaterHash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float WaterFarBlend(float camDist) {
    return SmoothWaterStep((camDist - FarBlendParams.x) / max(FarBlendParams.y, 1.0));
}

float2 SafeWaterFlowDir() {
    float2 flow = OceanParams1.yz;
    float lenSq = max(dot(flow, flow), 1e-4);
    return flow * rsqrt(lenSq);
}

// Asylum-style far-water replacement: a few long, smooth procedural swells.
// It is intentionally low-frequency so the horizon stops carrying FFT/choppy detail.
float WaterFarSwellHeight(float2 worldXZ) {
    float2 flow = SafeWaterFlowDir();
    float2 side = float2(-flow.y, flow.x);
    float windT = Misc.x * max(OceanParams0.y, 0.1);

    float2 d0 = flow;
    float2 d1 = normalize(flow * 0.62 + side * 0.78);
    float2 d2 = normalize(flow * 0.24 - side * 0.97);

    float h =
        sin(dot(worldXZ, d0) * 0.025 + windT * 0.105) * 0.55 +
        sin(dot(worldXZ, d1) * 0.015 + windT * 0.062 + 1.7) * 0.32 +
        sin(dot(worldXZ, d2) * 0.008 + windT * 0.037 + 4.1) * 0.22;

    return h * FarBlendParams.z * OceanParams1.x;
}

float2 WaterFarSwellSlope(float2 worldXZ) {
    const float eps = 3.0;
    float hx0 = WaterFarSwellHeight(worldXZ - float2(eps, 0.0));
    float hx1 = WaterFarSwellHeight(worldXZ + float2(eps, 0.0));
    float hz0 = WaterFarSwellHeight(worldXZ - float2(0.0, eps));
    float hz1 = WaterFarSwellHeight(worldXZ + float2(0.0, eps));
    return float2(hx1 - hx0, hz1 - hz0) / (2.0 * eps);
}

float3 WaterFarSwellNormal(float2 worldXZ) {
    float2 slope = WaterFarSwellSlope(worldXZ) * max(FarBlendParams.w, 0.0);
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// Parallax offset, 2 iteracoes. Desloca as UVs
// do bump pela altura na direcao da visao. dir = V projetado no plano (V.xz / V.y).
float2 BumpParallaxOffset(float4 baseTC, float3 V, float heightScale) {
    if (heightScale <= 0.0) return float2(0.0, 0.0);
    float2 dir = V.xz / max(V.y, 0.2);
    float h1 = WaterSampleFFTUv(baseTC.xy * 0.25).b * heightScale;
    float2 p = dir * h1;
    float h2 = WaterSampleFFTUv(baseTC.zw + p).b * heightScale;
    p += dir * h2;
    return p;
}

// Specular do sol — Blinn-Phong de 2 lobos: lobo largo + lobo estreito, dando o glint
// nitido nas cristas alem do brilho amplo.
float SunSpecularLobes(float3 N, float3 V, float3 L, float shininess) {
    float3 R = reflect(-V, N);
    float LdotR = saturate(dot(L, R));
    float wide   = pow(LdotR, shininess);
    float narrow = pow(LdotR, shininess * 8.0);
    return wide * 0.5 + narrow * 0.5;
}

#endif // SMILE_WATER_COMMON_HLSLI
