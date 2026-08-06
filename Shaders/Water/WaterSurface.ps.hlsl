#include "WaterCommon.hlsli"
#include "../Shadow/CSMCommon.hlsli"
#include "../GBuffer.hlsli"

TextureCube SpecularCube : register(t0);
SamplerState LinearClamp  : register(s1);

struct PSOutput {
    float4 Color       : SV_Target0;
    float2 Velocity    : SV_Target1; // mesmo contrato do GBuffer.ps: curUV - prevUV
    float4 GBufferA    : SV_Target2;
    float4 GBufferB    : SV_Target3;
    float4 GBufferC    : SV_Target4;
    float  Reactive    : SV_Target5;
    float  Composition : SV_Target6;
    float  SpecHitDist : SV_Target7;
};

struct PSOutputBase {
    float4 Color    : SV_Target0;
    float2 Velocity : SV_Target1;
};

struct PSOutputMasks {
    float4 Color       : SV_Target0;
    float2 Velocity    : SV_Target1;
    float  Reactive    : SV_Target2;
    float  Composition : SV_Target3;
};

struct PSOutputReflection {
    float4 Color       : SV_Target0;
    float2 Velocity    : SV_Target1;
    float4 GBufferA    : SV_Target2;
    float4 GBufferB    : SV_Target3;
    float4 GBufferC    : SV_Target4;
    float  Reactive    : SV_Target5;
    float  Composition : SV_Target6;
};

float4 ShadeWater(VSOutput IN, out float3 GuideNormal, out float GuideRoughness,
                    out float2 GuideRoughnessAxes, out float ReactiveMask, out float GuideFoam,
                    out float GuideReflectionCoverage) {
    GuideNormal = normalize(IN.normal);
    GuideRoughness = max(1.0f - ShadeParams.x, 0.04f);
    GuideRoughnessAxes = GuideRoughness.xx;
    ReactiveMask = 0.75f;
    GuideFoam = 0.0f;
    GuideReflectionCoverage = 1.0f;
    int debugMode = (int)floor(DebugParams.x + 0.5f);

    if (debugMode == 1) {
        return float4(0.05f, 0.95f, 1.0f, 1.0f);
    }
    if (debugMode == 2) {
        float tileSize = max(IN.debugData.x, 1.0f);
        float internalLodRaw = clamp(IN.debugData.z, 0.0f, 2.0f);
        float scaleLevel = log2(tileSize / max(QuadTreeParams.z, 1.0f));
        float scaleT = saturate(scaleLevel / 9.0f);

        float3 lod0 = float3(0.10f, 0.78f, 0.96f);
        float3 lod1 = float3(1.00f, 0.72f, 0.18f);
        float3 lod2 = float3(0.86f, 0.20f, 0.92f);
        float3 color = (internalLodRaw < 0.5f) ? lod0 : ((internalLodRaw < 1.5f) ? lod1 : lod2);
        color = lerp(color, float3(0.04f, 0.10f, 0.18f), scaleT * 0.35f);

        float2 tileUV = saturate(IN.tileUV);
        float2 edgeDist = min(tileUV, 1.0f - tileUV);
        float gridWidth = max(max(fwidth(tileUV.x), fwidth(tileUV.y)) * 1.15f, 0.0015f);
        float gridLine = 1.0f - smoothstep(0.0f, gridWidth, min(edgeDist.x, edgeDist.y));

        float geomorph = saturate(IN.debugData.w);
        color = lerp(color, float3(0.15f, 1.0f, 0.42f), geomorph * 0.25f);

        uint subsetPattern = (uint)floor(IN.debugData.y + 0.5f);
        uint leftPattern = subsetPattern % 3u; subsetPattern /= 3u;
        uint rightPattern = subsetPattern % 3u; subsetPattern /= 3u;
        uint bottomPattern = subsetPattern % 3u; subsetPattern /= 3u;
        uint topPattern = subsetPattern % 3u;

        float stitchWidth = max(gridWidth * 2.0f, 0.003f);
        float stitchLine = 0.0f;
        stitchLine = max(stitchLine, (leftPattern > 0u) ? 1.0f - smoothstep(0.0f, stitchWidth, tileUV.x) : 0.0f);
        stitchLine = max(stitchLine, (rightPattern > 0u) ? 1.0f - smoothstep(0.0f, stitchWidth, 1.0f - tileUV.x) : 0.0f);
        stitchLine = max(stitchLine, (bottomPattern > 0u) ? 1.0f - smoothstep(0.0f, stitchWidth, tileUV.y) : 0.0f);
        stitchLine = max(stitchLine, (topPattern > 0u) ? 1.0f - smoothstep(0.0f, stitchWidth, 1.0f - tileUV.y) : 0.0f);

        color = lerp(color, float3(0.86f, 0.96f, 1.0f), gridLine * 0.18f);
        color = lerp(color, float3(1.0f, 0.08f, 0.04f), stitchLine * 0.78f);
        return float4(color, 1.0f);
    }

    float3 N = normalize(IN.normal);
    float3 V = normalize(IN.vView);
    float2 unresolvedSlopeVariance = 0.0f;

    float camDist  = length(IN.vView);
    float detailFade = WaterDistanceFade(camDist, 0.0, BumpParams2.w);
    float swellFade = WaterDistanceFade(camDist, BumpParams2.w, max(BumpParams2.w * 16.0, 4500.0));
    swellFade = lerp(0.30, 1.0, swellFade);
    // Camadas de normal = cascatas REAIS: hi = cascata 0 (detalhe), lo = cascatas 1/2.
    float2 uv2  = WaterCascadeUV(2, IN.parametricXZ);
    float2 hiDx = ddx(IN.baseTC.zw);
    float2 hiDy = ddy(IN.baseTC.zw);
    float2 loDx = ddx(IN.baseTC.xy);
    float2 loDy = ddy(IN.baseTC.xy);
    float2 lo2Dx = ddx(uv2);
    float2 lo2Dy = ddy(uv2);
    if (BumpParams2.z > 0.5 && (detailFade > 0.0 || swellFade > 0.0)) {
        float2 pofs = BumpParallaxOffset(IN.baseTC, V, BumpParams2.y * max(detailFade, swellFade * 0.35));
        float4 mHi  = SampleWaterSlopeMomentsCascade(0, IN.baseTC.zw + pofs, hiDx, hiDy);
        float4 mLo0 = SampleWaterSlopeMomentsCascade(1, IN.baseTC.xy + pofs, loDx, loDy);
        float4 mLo1 = SampleWaterSlopeMomentsCascade(2, uv2 + pofs, lo2Dx, lo2Dy);
        float4 sHi  = WaterDecodeSlopeMoments(mHi);
        float4 sLo0 = WaterDecodeSlopeMoments(mLo0);
        float4 sLo1 = WaterDecodeSlopeMoments(mLo1);
        float2 hiSlope = (sHi.xz  / max(sHi.y,  0.05)) * BumpParams.w;
        // Camada de RIPPLE fino (glitter granulado estilo AC4): cascata 0 re-amostrada
        // a 6x com deriva pelo vento — devolve a alta frequência que o tiling fake
        // antigo dava e as UVs físicas perderam. Fade curto (~120 m).
        float ripFade = WaterDistanceFade(camDist, 0.0, 120.0);
        float4 sRip = float4(0.0, 1.0, 0.0, 1.0);
        float4 mRip = 0.0f;
        if (ripFade > 0.0) {
            // A FFT positiva com k centrado resolve a crista em +wind. Textura
            // procedural f(uv-vt) precisa do sinal oposto para viajar junto dela.
            float2 uvRip = IN.baseTC.zw * 6.0 - OceanParams1.yz * (Misc.x * 0.02);
            mRip = SampleWaterSlopeMomentsCascade(0, uvRip, hiDx * 6.0, hiDy * 6.0);
            sRip = WaterDecodeSlopeMoments(mRip);
            hiSlope += (sRip.xz / max(sRip.y, 0.05)) * (BumpParams.w * 0.7 * ripFade);
        }
        float2 loSlope = ((sLo0.xz / max(sLo0.y, 0.05)) + (sLo1.xz / max(sLo1.y, 0.05))) *
                         (0.5 * BumpParams.z);
        float bumpContribution = saturate(BumpParams2.x * max(detailFade, swellFade * 0.75));
        const float hiWeight = BumpParams.w * detailFade;
        const float rippleWeight = BumpParams.w * 0.7f * ripFade;
        const float loWeight = 0.5f * BumpParams.z * swellFade;
        unresolvedSlopeVariance = WaterSlopeVariance(mHi) * (hiWeight * hiWeight) +
                                  WaterSlopeVariance(mRip) * (rippleWeight * rippleWeight) +
                                  (WaterSlopeVariance(mLo0) + WaterSlopeVariance(mLo1)) *
                                      (loWeight * loWeight);
        unresolvedSlopeVariance *= bumpContribution * bumpContribution;
        // NormalMip represents the same FFT field already used by the displaced
        // geometry. Adding its slope to IN.normal double-counted that spectrum
        // (about 1.5x at the default strength). Blend between the geometric and
        // filtered per-pixel estimators instead; the advected ripple above remains
        // an intentional shading-only detail inside the filtered estimator.
        const float2 filteredSlope = hiSlope * detailFade + loSlope * swellFade;
        const float3 filteredNormal = normalize(float3(filteredSlope.x, 1.0f, filteredSlope.y));
        N = normalize(lerp(N, filteredNormal, bumpContribution));
    }
    if (debugMode == 3) {
        float4 dispDebug = WaterSampleFFT(IN.parametricXZ);
        float h = dispDebug.z * 0.01f;
        float pos = saturate(h);
        float neg = saturate(-h);
        return float4(0.42f + pos * 0.58f, 0.48f - saturate(abs(h)) * 0.22f,
                      0.55f + neg * 0.45f, 1.0f);
    }
    if (debugMode == 4) {
        return float4(N * 0.5f + 0.5f, 1.0f);
    }

    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);

    float  karisVariance = min(1.6 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.55);
    float  glossFadeDist = max(BumpParams2.w * 14.0, 3500.0);
    float  distGlossFade = clamp(1.0 - camDist / glossFadeDist, 0.45, 1.0); 
    float  baseRoughness = saturate(1.0 - ShadeParams.x * distGlossFade);
    // Cada eixo recebe apenas a energia de slope que deixou de caber na normal filtrada.
    // O termo de Karis e isotropico; a energia dos mips permanece wind/cross-wind.
    const float2 alphaAxes = saturate(baseRoughness * baseRoughness +
                                      unresolvedSlopeVariance + 0.5f * karisVariance);
    const float2 reflectionRoughnessAxes = sqrt(max(alphaAxes, 0.0016f.xx));
    const float alphaEffective = sqrt(reflectionRoughnessAxes.x * reflectionRoughnessAxes.x *
                                      reflectionRoughnessAxes.y * reflectionRoughnessAxes.y);
    float reflectionRoughness = sqrt(alphaEffective);
    GuideRoughness = max(reflectionRoughness, 0.04f);
    GuideRoughnessAxes = max(reflectionRoughnessAxes, 0.04f.xx);
    float reflBump = saturate(RefractionParams.w);
    float3 Nrefl = normalize(lerp(float3(0.0, 1.0, 0.0), N, reflBump));
    // Sol, ambiente e o trace dedicado devem descrever a MESMA superficie especular.
    // Antes o cubemap usava Nrefl, enquanto Fresnel/Sol/guides usavam N integral.
    GuideNormal = Nrefl;
    float NoV = saturate(dot(Nrefl, V));
    const bool dedicatedReflections = DebugParams.y > 0.5f;
    
    float3 R = reflect(-V, Nrefl);
    float3 reflection = 0.0f;
    if (!dedicatedReflections) {
        if (Misc.y > 0.5) {
            float lod = reflectionRoughness * Misc.w;
            reflection = SpecularCube.SampleLevel(LinearClamp, R, lod).rgb * Misc.z;
        } else {
            reflection = AnalyticSky(R);
        }
        reflection *= ShadeParams.y;
    }
    
    float F = FresnelSchlick(0.02, NoV);
    if (debugMode == 5) {
        return float4(F.xxx, 1.0f);
    }

    // Sombra do sol (CSM) na água: gate dos termos solares (in-scatter/SSS/spec/espuma).
    // A normal geométrica é o plano d'água — offset anti-acne na vertical.
    float sunVis = SampleCSM(IN.worldPos, float3(0.0, 1.0, 0.0), IN.pos.xy);

    // Luz que ilumina o corpo d'água (estilo UE SingleLayerWater: sol + ambiente do céu
    // iluminam o espalhamento). Antes o in-scatter era uma COR CONSTANTE — água turquesa
    // de meio-dia mesmo no pôr-do-sol = o look "leitoso" (A/B 2026-07-16).
    float3 bodyLight = WaterAmbient.rgb +
                       SunColor.rgb * (SunDirection.w * saturate(SunDirection.y) * 0.35 * sunVis);

    float3 body = DeepColorDensity.rgb * bodyLight;
    float  columnDebug = 0.0f;
    float  softDebug   = 0.0f;
    float  fA   = 1.0;
    if (DepthParams.z > 0.5) {
        float  waterDepth = IN.screenProj.w;                 
        float2 screenUV   = IN.screenProj.xy / IN.screenProj.w;
        float  sceneDepth = SampleSceneDepthLin(screenUV);   
        columnDebug = max(sceneDepth - waterDepth, 0.0f);
        float  column     = sceneDepth - waterDepth;         

        float softIntersect = saturate(RefractionParams.y * column);
        softDebug = softIntersect;

        float2 refrOfs = N.xz * RefractionParams.x * softIntersect * IN.screenProj.z;
        float2 refrUV  = clamp(screenUV + refrOfs, 0.5f * ScreenParams.zw,
                               1.0f - 0.5f * ScreenParams.zw);
        const int2 refrPx = clamp(int2(refrUV * ScreenParams.xy), int2(0, 0),
                                  int2(ScreenParams.xy) - 1);
        const float refrDeviceZ = SceneDepth.Load(int3(refrPx, 0));
        const float refrSceneDepth = LinearizeDepth(refrDeviceZ);
        bool invalidRefraction = refrDeviceZ <= 0.0f || refrSceneDepth < waterDepth;
        if (!invalidRefraction) {
            const float3 refrSceneWorld = WaterSceneWorldFromDepth(refrUV, refrDeviceZ);
            // Depth sozinho so garante que a amostra esta atras da interface. Um offset que cai
            // na fachada ACIMA da agua tambem satisfaz isso e duplica o predio como uma falsa
            // reflexao deslocada. Transmissao valida deve terminar abaixo da superficie local.
            const float interfaceY = max(IN.worldPos.y, OceanParams1.w) + 0.25f;
            invalidRefraction = refrSceneWorld.y > interfaceY;
        }
        if (invalidRefraction) refrUV = screenUV;
        float3 refrColor = SceneColor.SampleLevel(LinearClamp, refrUV, 0).rgb; 

        float cosForward = waterDepth / max(length(IN.vView), 1e-3);
        float pathLen    = max(column, 0.0) / max(cosForward, 0.1);

        float opticalLen = pathLen * max(RefractionParams.z, 0.0f) * 0.08f;
        float3 transmit  = exp2(-AbsorptionColor.rgb * opticalLen);
        float3 inScatter = InScatterColor.rgb * bodyLight *
                           (1.0 - exp2(-InScatterColor.w * opticalLen));
        body = refrColor * transmit + inScatter;

        float cameraSoft = saturate(waterDepth - 0.33);
        fA = softIntersect * cameraSoft;
    }

    // SSS de crista (porte do Watercfx da Cry): luz vazando pela onda — ambiente do céu
    // + lobo forward-scatter do sol, pesados pela altura da crista e pela inclinação.
    if (WaterFXParams.x > 0.0) {
        float waveHeight = saturate((IN.worldPos.y - OceanParams1.w) * WaterFXParams.z);
        // Clamp do peso + termo de ambiente ×0.25: sem isso o céu HDR inteiro somava no
        // corpo d'água e lavava o mar de branco (A/B 2026-07-16, look "chantilly").
        float whf = saturate(waveHeight * 0.5 + NoV * (1.0 - saturate(N.y)));
        float3 sssTint = InScatterColor.rgb * WaterFXParams.x;
        float3 sssEnv = (Misc.y > 0.5)
            ? SpecularCube.SampleLevel(LinearClamp, -V, clamp(whf, 0.2, 0.7) * Misc.w).rgb * Misc.z
            : AnalyticSky(-V);
        float EdotL = saturate(dot(-SunDirection.xyz, V));
        body += sssTint * sssEnv * whf * fA * 0.25;
        body += sssTint * pow(EdotL, WaterFXParams.y) *
                (SunColor.rgb * SunDirection.w) * whf * fA * sunVis;
    }

    float reflectionWeight = saturate(F * fA);
    GuideReflectionCoverage = saturate(fA);
    // No caminho dedicado a radiancia refletida chega num passe posterior. O corpo ja sai
    // atenuado pelo Fresnel para manter a composicao aproximadamente conservativa.
    float3 color = dedicatedReflections
        ? body * (1.0f - reflectionWeight)
        : lerp(body, reflection, reflectionWeight);
    if (debugMode == 6) {
        return float4(body / (body + 1.0f), 1.0f);
    }
    if (debugMode == 7) {
        return float4(reflection / (reflection + 1.0f), 1.0f);
    }
    if (debugMode == 9) {
        float d = saturate(columnDebug * 0.05f);
        return float4(d, softDebug, 1.0f - d, 1.0f);
    }

    float  foam    = 0.0;
    float3 foamLit = float3(0.0, 0.0, 0.0);
    float  JDebug  = 1.0;
    if (FoamParams.z > 0.0 || debugMode == 8) {
        // First-order combined determinant. Each per-cascade J is now metric and
        // equally weighted; exact cross terms would require the full derivative tensors.
        uint numFoamC = (uint)CascadeParams.w;
        const float2 jUv0 = WaterCascadeUV(0, IN.parametricXZ);
        const float2 jUv1 = WaterCascadeUV(1, IN.parametricXZ);
        const float2 jUv2 = WaterCascadeUV(2, IN.parametricXZ);
        JDebug = WaterSampleFFTCascadeUvGrad(0, jUv0, ddx(jUv0), ddy(jUv0)).w;
        if (numFoamC > 1)
            JDebug += WaterSampleFFTCascadeUvGrad(1, jUv1, ddx(jUv1), ddy(jUv1)).w - 1.0;
        if (numFoamC > 2)
            JDebug += WaterSampleFFTCascadeUvGrad(2, jUv2, ddx(jUv2), ddy(jUv2)).w - 1.0;
        foam = saturate((FoamParams.x - JDebug) / max(FoamParams.y, 1e-3));
        foam = foam * foam * (3.0 - 2.0 * foam);
        // Breakup procedural (estilo FoamTex da Cry, sem asset): 2 oitavas de value
        // noise advectadas pelo fluxo do vento — quebra o "adesivo chapado".
        float2 flow   = -OceanParams1.yz * Misc.x;
        float2 foamUV = IN.parametricXZ * 0.35 + flow * 0.05;
        float  pat = WaterValueNoise(foamUV) * 0.65 +
                     WaterValueNoise(foamUV * 3.7 - flow * 0.11) * 0.35;
        foam *= saturate(0.35 + 1.55 * pat * pat);
        foam = saturate(foam * FoamParams.z * WaterDistanceFade(camDist, 0.0, FoamParams.w));
        // Espuma iluminada estilo Cry (sol×NdotL×sombra + céu): o piso constante 0.30
        // brilhava no escuro — agora é o ambiente físico do céu.
        foamLit = FoamColor.rgb * (SunColor.rgb * SunDirection.w * 0.16 * sunVis +
                                   WaterAmbient.rgb * 0.8);
    }
    if (debugMode == 8) {
        return float4(saturate((1.0f - JDebug) * 2.0f), foam, saturate(JDebug), 1.0f);
    }

    float spec = WaterSunSpecularGGX(Nrefl, V, normalize(SunDirection.xyz),
                                     GuideRoughnessAxes, WaterWindDirection());
    float3 sunSpecCol = SunColor.rgb * (spec * SunDirection.w * ShadeParams.w) * sunVis;
    sunSpecCol *= (1.0 - foam * FoamColor.w);
    color += sunSpecCol;

    color = lerp(color, foamLit, foam);
    ReactiveMask = saturate(max(ReactiveMask, foam));
    GuideFoam = foam;

    return float4(color, 1.0);
}

PSOutput EvaluateWaterSurface(VSOutput IN) {
    PSOutput o;
    float3 GuideNormal;
    float GuideRoughness;
    float2 GuideRoughnessAxes;
    float ReactiveMask;
    float GuideFoam;
    float GuideReflectionCoverage;
    o.Color = ShadeWater(IN, GuideNormal, GuideRoughness, GuideRoughnessAxes,
                         ReactiveMask, GuideFoam, GuideReflectionCoverage);

    float2 curNDC  = IN.curClip.xy  / IN.curClip.w;
    float2 prevNDC = IN.prevClip.xy / IN.prevClip.w;
    float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    // A agua nao volta ao deferred; estes MRTs existem para o pass de guides do RR enxergar o
    // material que corresponde ao depth/velocity da superficie, em vez do opaco atras dela.
    o.GBufferA = float4(saturate(InScatterColor.rgb), 1.0f);
    o.GBufferB = float4(GBuffer_OctEncode(normalize(GuideNormal)),
                        GuideRoughness, GBuffer_EncodeShadingModel(SMILE_SHADINGMODEL_WATER));
    // Canal privado: supressao da espuma, cobertura optica e roughness wind/cross-wind.
    o.GBufferC = float4(GuideFoam * FoamColor.w, GuideReflectionCoverage,
                        GuideRoughnessAxes);
    o.Reactive = ReactiveMask;
    o.Composition = 1.0f;
    // A reflexao da agua e resolvida no proprio shader (sky/refraction), nao pelo hit da superficie
    // opaca anterior. Zerar impede o RR de herdar o spec-hit do fundo.
    o.SpecHitDist = 0.0f;
    return o;
}

PSOutput main(VSOutput IN) {
    return EvaluateWaterSurface(IN);
}

PSOutputBase mainBase(VSOutput IN) {
    const PSOutput Full = EvaluateWaterSurface(IN);
    PSOutputBase o;
    o.Color = Full.Color;
    o.Velocity = Full.Velocity;
    return o;
}

PSOutputMasks mainMasks(VSOutput IN) {
    const PSOutput Full = EvaluateWaterSurface(IN);
    PSOutputMasks o;
    o.Color = Full.Color;
    o.Velocity = Full.Velocity;
    o.Reactive = Full.Reactive;
    o.Composition = Full.Composition;
    return o;
}

PSOutputReflection mainReflection(VSOutput IN) {
    const PSOutput Full = EvaluateWaterSurface(IN);
    PSOutputReflection o;
    o.Color = Full.Color;
    o.Velocity = Full.Velocity;
    o.GBufferA = Full.GBufferA;
    o.GBufferB = Full.GBufferB;
    o.GBufferC = Full.GBufferC;
    o.Reactive = Full.Reactive;
    o.Composition = Full.Composition;
    return o;
}
