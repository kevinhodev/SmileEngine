#include "WaterCommon.hlsli"

// t0 = cubemap especular prefiltrado do FHDREnvironment (reflexao IBL).
TextureCube SpecularCube : register(t0);
Texture2D<float4> AtmosphereSkyView : register(t5);
SamplerState LinearClamp  : register(s1);

static const float WaterAtmoBottomR = 6360.0f;
static const float WaterAtmoViewH   = 6360.5f;

float2 WaterSkyViewParamsToUv(float viewZenithCos, float lightViewCos) {
    float vHorizon = sqrt(max(0.0f, WaterAtmoViewH * WaterAtmoViewH - WaterAtmoBottomR * WaterAtmoBottomR));
    float cosBeta  = vHorizon / max(WaterAtmoViewH, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = 3.14159265f - beta;

    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0f, 1.0f));
    float u, v;
    if (viewZenithAngle < zenithHorizonAngle) {
        float coord = viewZenithAngle / max(zenithHorizonAngle, 1e-4f);
        coord = 1.0f - coord;
        coord = 1.0f - sqrt(max(0.0f, coord));
        v = coord * 0.5f;
    } else {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-4f);
        coord = sqrt(max(0.0f, coord));
        v = coord * 0.5f + 0.5f;
    }

    float coord = -lightViewCos * 0.5f + 0.5f;
    u = sqrt(max(0.0f, coord));
    return float2(u, v);
}

float3 WaterAerialFogColor(float3 viewDir) {
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 fogDir = normalize(float3(viewDir.x, max(viewDir.y, 0.015f), viewDir.z));

    if (AerialFogParams.w > 1.5f) {
        float3 sunDir = normalize(SunDirection.xyz);
        float viewZenithCos = max(dot(fogDir, up), -0.02f);
        float3 viewHoriz = fogDir - up * viewZenithCos;
        float3 sunHoriz  = sunDir - up * dot(sunDir, up);
        float lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));
        return AtmosphereSkyView.SampleLevel(LinearClamp, WaterSkyViewParamsToUv(viewZenithCos, lightViewCos), 0.0f).rgb;
    }

    if (Misc.y > 0.5f) {
        return SpecularCube.SampleLevel(LinearClamp, fogDir, Misc.w).rgb * Misc.z;
    }

    return AnalyticSky(fogDir);
}

float WaterAerialFogAmount(float camDist, float3 viewDir) {
    if (AerialFogParams.w <= 0.5f) return 0.0f;

    float distPastStart = max(camDist - AerialFogParams.x, 0.0f);
    float rangeT = saturate(distPastStart / max(AerialFogParams.y, 1.0f));
    rangeT = rangeT * rangeT * (3.0f - 2.0f * rangeT);

    float earlyT = saturate(distPastStart / max(AerialFogParams.y * 0.72f, 1.0f));
    earlyT = earlyT * earlyT * (3.0f - 2.0f * earlyT);

    float grazing = saturate(1.0f - abs(viewDir.y) * 8.5f);
    grazing = grazing * grazing * (3.0f - 2.0f * grazing);

    float distanceKm = distPastStart * 0.001f;
    float density = max(AerialFogParams.z, 0.0f);
    float expFog = 1.0f - exp2(-density * distanceKm * lerp(0.45f, 2.35f, grazing));
    float horizonFog = earlyT * grazing * 0.38f;

    return saturate(max(max(rangeT * grazing * 0.55f, expFog * earlyT), horizonFog) * 0.78f);
}

// Etapa 0: agua plana. Fresnel mistura cor de fundo <-> reflexao do ceu; + sun specular.
// Saida em HDR LINEAR (o tonemap ACES e um passe final unico — DOCS/ARCHITECTURE).
float4 main(VSOutput IN) : SV_Target {
    int debugMode = (int)floor(DebugParams.x + 0.5f);

    if (debugMode == 1) {
        return float4(0.05f, 0.95f, 1.0f, 1.0f);
    }
    if (debugMode == 2) {
        float tileSize = max(IN.debugData.x, 1.0f);
        float internalLodRaw = clamp(IN.debugData.z, 0.0f, 2.0f);
        float effectiveCellSize = tileSize * exp2(internalLodRaw) / 32.0f;
        float lodT = saturate(1.0f - log2(max(effectiveCellSize, 2.0f) / 2.0f) / 8.0f);
        float3 nearColor = float3(1.0f, 0.76f, 0.12f);
        float3 farColor  = float3(0.06f, 0.32f, 1.0f);
        float3 color = lerp(farColor, nearColor, lodT);

        float internalLod = saturate(internalLodRaw / 2.0f);
        color = lerp(color, float3(0.72f, 0.20f, 0.95f), internalLod * 0.30f);

        float geomorph = saturate(IN.debugData.w);
        color = lerp(color, float3(0.15f, 1.0f, 0.42f), geomorph * 0.22f);

        float hasSkirt = step(0.5f, IN.debugData.y);
        color = lerp(color, float3(1.0f, 0.08f, 0.08f), hasSkirt * 0.25f);
        return float4(color, 1.0f);
    }

    float3 N = normalize(IN.normal);
    float3 V = normalize(IN.vView);
    float  normalToksvigT = 1.0;

    // --- Bump de detalhe (Etapa 2): perturba a normal do FFT com detalhe filtrado ---
    // O ripple fino morre cedo; a baixa frequencia continua no fundo para a agua nao
    // virar um plano liso depois do anti-shimmer.
    float camDist  = length(IN.vView);
    float farBlend = WaterFarBlend(camDist);
    float nearFFT = 1.0 - farBlend;
    float detailFade = saturate(1.0 - camDist / max(BumpParams2.w, 1.0));
    detailFade = detailFade * detailFade * (3.0 - 2.0 * detailFade) * nearFFT;
    float swellFadeRange = max(BumpParams2.w * 16.0, 4500.0);
    float swellFade = saturate(1.0 - (camDist - BumpParams2.w) / swellFadeRange);
    swellFade = swellFade * swellFade * (3.0 - 2.0 * swellFade);
    swellFade = lerp(0.30, 1.0, swellFade) * nearFFT;
    float2 hiDx = ddx(IN.baseTC.zw);
    float2 hiDy = ddy(IN.baseTC.zw);
    float2 loDx = ddx(IN.baseTC.xy);
    float2 loDy = ddy(IN.baseTC.xy);
    if (BumpParams2.z > 0.5 && (detailFade > 0.0 || swellFade > 0.0)) {
        float2 pofs = BumpParallaxOffset(IN.baseTC, V, BumpParams2.y * max(detailFade, swellFade * 0.35));
        float4 sHi  = SampleWaterNormalGrad(IN.baseTC.zw + pofs, hiDx, hiDy); // alta freq
        float4 sLo0 = SampleWaterNormalGrad(IN.baseTC.xy * 0.25 + pofs, loDx * 0.25, loDy * 0.25);
        float4 sLo1 = SampleWaterNormalGrad(IN.baseTC.xy + pofs, loDx, loDy);
        float2 hiSlope = (sHi.xz  / max(sHi.y,  0.05)) * BumpParams.w;
        float2 loSlope = ((sLo0.xz / max(sLo0.y, 0.05)) + (sLo1.xz / max(sLo1.y, 0.05))) *
                         (0.5 * BumpParams.z);
        float layerToksvigT = min(lerp(1.0, sHi.w, detailFade),
                                  lerp(1.0, min(sLo0.w, sLo1.w), swellFade));
        float bumpContribution = saturate(BumpParams2.x * max(detailFade, swellFade * 0.75));
        normalToksvigT = lerp(1.0, layerToksvigT, bumpContribution);
        N.xz += (hiSlope * detailFade + loSlope * swellFade) * BumpParams2.x;
        N = normalize(N);
    }
    if (farBlend > 0.0) {
        N = normalize(lerp(N, WaterFarSwellNormal(IN.worldPos.xz), farBlend));
        normalToksvigT = lerp(normalToksvigT, 1.0, farBlend);
    }

    if (debugMode == 3) {
        float2 tcDebug = IN.worldPos.xz * 0.0125 * OceanParams0.w * 1.25;
        float4 dispDebug = FFTDisplacement.SampleLevel(LinearWrap, tcDebug, 0.0);
        float h = dispDebug.z * 0.01f;
        float pos = saturate(h);
        float neg = saturate(-h);
        return float4(0.42f + pos * 0.58f, 0.48f - saturate(abs(h)) * 0.22f,
                      0.55f + neg * 0.45f, 1.0f);
    }
    if (debugMode == 4) {
        return float4(N * 0.5f + 0.5f, 1.0f);
    }

    float  NoV = saturate(dot(N, V));
    float  horizonGrazing = saturate(1.0f - abs(V.y) * 5.0f);
    horizonGrazing = horizonGrazing * horizonGrazing * (3.0f - 2.0f * horizonGrazing);

    // --- Specular AA para agua: Karis (derivadas de normal) + Toksvig (normal mipped) ---
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    // Karis mais forte: onde a chop varia muito a normal, a reflexao fica mais aspera
    // (mip mais alto) -> o reflexo do ceu/sol nas ripples vira brilho suave, sem blobs.
    float  karisVariance = min(1.6 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.55);
    float  toksvigVar    = 1.0 - normalToksvigT * normalToksvigT;
    float  glossFadeDist = max(BumpParams2.w * 14.0, 3500.0);
    float  distGlossFade = clamp(1.0 - camDist / glossFadeDist, 0.45, 1.0); // mais blur ao longe (mata blocos)
    float  baseRoughness = saturate(1.0 - ShadeParams.x * distGlossFade);
    float  horizonRoughness = horizonGrazing * farBlend * 0.35f;
    float  reflectionRoughness =
        saturate(sqrt(baseRoughness * baseRoughness + karisVariance + toksvigVar) +
                 farBlend * 0.18 + horizonRoughness);
    float reflBump = saturate(RefractionParams.w *
                              lerp(1.0, 0.32, farBlend) *
                              lerp(1.0, 0.55, horizonGrazing * farBlend));
    float3 Nrefl = normalize(lerp(float3(0.0, 1.0, 0.0), N, reflBump));

    // --- Reflexao ---
    float3 R = reflect(-V, Nrefl);
    float3 reflection;
    if (Misc.y > 0.5) {
        // Cubemap IBL: roughness alto -> mip alto. Toksvig/derivadas matam fireflies.
        float lod = reflectionRoughness * Misc.w;
        reflection = SpecularCube.SampleLevel(LinearClamp, R, lod).rgb * Misc.z;
    } else {
        reflection = AnalyticSky(R);
    }
    float horizonReflectionScatter = horizonGrazing * farBlend;
    reflection = lerp(reflection, WaterAerialFogColor(normalize(IN.worldPos - CameraPos.xyz)),
                      horizonReflectionScatter * 0.16f);
    reflection *= ShadeParams.y; // ReflectionScale

    // --- Fresnel (F0 da agua ~0.02) decide reflexao vs corpo ---
    float F = FresnelSchlick(0.02, NoV, ShadeParams.x);
    if (debugMode == 5) {
        return float4(F.xxx, 1.0f);
    }

    // --- Corpo da agua: refracao + absorcao (Etapa 3), porte do WaterPS (Water.cfx:1030+) ---
    float3 body = DeepColorDensity.rgb;
    float  columnDebug = 0.0f;
    float  softDebug   = 0.0f;
    float  fA   = 1.0; // soft-intersection (mundo x camera) — modula o Fresnel nas bordas
    if (DepthParams.z > 0.5) {
        float  waterDepth = IN.screenProj.w;                 // view-Z da agua (clip.w)
        float2 screenUV   = IN.screenProj.xy / IN.screenProj.w;
        float  sceneDepth = SampleSceneDepthLin(screenUV);   // cena atras da agua
        columnDebug = max(sceneDepth - waterDepth, 0.0f);
        float  column     = sceneDepth - waterDepth;         // espessura da coluna d'agua (na vista)

        // borda suave: orla fina (column pequeno) deixa a cena aparecer.
        float softIntersect = saturate(RefractionParams.y * column);
        softDebug = softIntersect;

        // refracao: desloca o UV pela normal; escala por softIntersect e screenProj.z (atenua longe).
        float2 refrOfs = N.xz * RefractionParams.x * softIntersect * IN.screenProj.z;
        float2 refrUV  = screenUV + refrOfs;
        // mascara de profundidade: so refrata o que esta ATRAS da agua (anti-bleed de foreground).
        if (SampleSceneDepthLin(refrUV) < waterDepth) refrUV = screenUV;
        float3 refrColor = SceneColor.SampleLevel(LinearClamp, refrUV, 0).rgb; // ja LINEAR HDR

        // in-scattering estilo Cry (OceanIntoPS) / Single Layer Water: a coluna d'agua
        // ABSORVE a refracao por canal (vermelho some 1o -> azula) e ESPALHA de volta um
        // turquesa que cresce com a profundidade e satura. Independe da espessura, entao
        // aparece mesmo na esfera demo pequena.
        float cosForward = waterDepth / max(length(IN.vView), 1e-3);
        float pathLen    = max(column, 0.0) / max(cosForward, 0.1);
        // A coluna em view-space fica enorme no oceano aberto; escalar pelo fog density
        // evita que o corpo vire uma chapa turquesa saturada em poucos metros.
        float opticalLen = pathLen * max(RefractionParams.z, 0.0f) * 0.08f;
        float3 transmit  = exp2(-AbsorptionColor.rgb * opticalLen);
        float3 inScatter = InScatterColor.rgb * (1.0 - exp2(-InScatterColor.w * opticalLen));
        body = refrColor * transmit + inScatter;

        float cameraSoft = saturate(waterDepth - 0.33);
        fA = softIntersect * cameraSoft;
    }

    float slopeEnergy = saturate(length(N.xz) * 1.45f);
    float surfaceReflection = (0.18f + 0.32f * slopeEnergy) * saturate(1.0f - farBlend * 0.55f);
    float reflectionWeight = saturate(max(F * fA, surfaceReflection * fA) * ShadeParams.y);
    float3 color = lerp(body, reflection, reflectionWeight);
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

    // --- Foam / whitecaps: Jacobiano da deformacao choppy do FFT (Asylum/Cry) ---
    // J<1 = cristas dobrando -> espuma difusa que clareia a agua e abafa o specular.
    // Amostra o .w do FFT na mesma tiling do VS (recomputada de worldPos.xz).
    float  foam    = 0.0;
    float3 foamLit = float3(0.0, 0.0, 0.0);
    float  JDebug  = 1.0;
    if (FoamParams.z > 0.0 || debugMode == 8) {
        float2 tcFoam = IN.worldPos.xz * 0.0125 * OceanParams0.w * 1.25;
        JDebug = FFTDisplacement.SampleLevel(LinearWrap, tcFoam, 0.0).w;
        foam = saturate((FoamParams.x - JDebug) / max(FoamParams.y, 1e-3));
        foam = foam * foam * (3.0 - 2.0 * foam);
        float foamFade = saturate(1.0 - camDist / max(FoamParams.w, 1.0));
        foamFade = foamFade * foamFade * (3.0 - 2.0 * foamFade);
        foam = saturate(foam * FoamParams.z * foamFade * nearFFT);
        // espuma iluminada pelo sol (acompanha a exposicao HDR da cena) + termo ambiente.
        foamLit = FoamColor.rgb * (SunColor.rgb * SunDirection.w * 0.16 + 0.30);
    }
    if (debugMode == 8) {
        return float4(saturate((1.0f - JDebug) * 2.0f), foam, saturate(JDebug), 1.0f);
    }

    // --- Sun specular (2 lobos) ---
    float specAA = saturate(1.0 - reflectionRoughness * 1.25) * lerp(1.0, 0.22, farBlend);
    float sunShininess = lerp(24.0, ShadeParams.z, specAA * specAA);
    float spec = SunSpecularLobes(N, V, normalize(SunDirection.xyz), sunShininess) * specAA;
    float3 sunSpecCol = SunColor.rgb * (spec * SunDirection.w * ShadeParams.w);
    sunSpecCol = min(sunSpecCol, AbsorptionColor.w); // clamp fireflies do sol (patches brancos)
    sunSpecCol *= (1.0 - foam * FoamColor.w);        // espuma abafa o glint do sol
    color += sunSpecCol;

    // Espuma por cima (difusa): sobrepoe reflexo/specular onde as cristas dobram.
    color = lerp(color, foamLit, foam);

    // Aerial perspective de superficie: lava contraste/spec no medio-longe em HDR,
    // escondendo o aliasing residual do projected grid sem apagar o primeiro plano.
    float3 viewDir = normalize(IN.worldPos - CameraPos.xyz);
    float aerialFog = WaterAerialFogAmount(camDist, viewDir);
    if (aerialFog > 0.0) {
        float3 fogColor = lerp(DeepColorDensity.rgb, WaterAerialFogColor(viewDir), 0.82);
        color = lerp(color, fogColor, min(aerialFog, 0.68));
    }

    return float4(color, 1.0);
}
