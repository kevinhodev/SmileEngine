#include "WaterCommon.hlsli"
#include "../Shadow/CSMCommon.hlsli"

TextureCube SpecularCube : register(t0);
SamplerState LinearClamp  : register(s1);

struct PSOutput {
    float4 Color    : SV_Target0;
    float2 Velocity : SV_Target1; // mesmo contrato do GBuffer.ps: curUV - prevUV
};

float4 ShadeWater(VSOutput IN) {
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
    float  normalToksvigT = 1.0;

    float camDist  = length(IN.vView);
    float detailFade = WaterDistanceFade(camDist, 0.0, BumpParams2.w);
    float swellFade = WaterDistanceFade(camDist, BumpParams2.w, max(BumpParams2.w * 16.0, 4500.0));
    swellFade = lerp(0.30, 1.0, swellFade);
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
    if (debugMode == 3) {
        float4 dispDebug = WaterSampleFFT(IN.worldPos.xz);
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

    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);

    float  karisVariance = min(1.6 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.55);
    float  toksvigVar    = 1.0 - normalToksvigT * normalToksvigT;
    float  glossFadeDist = max(BumpParams2.w * 14.0, 3500.0);
    float  distGlossFade = clamp(1.0 - camDist / glossFadeDist, 0.45, 1.0); 
    float  baseRoughness = saturate(1.0 - ShadeParams.x * distGlossFade);
    float  reflectionRoughness =
        saturate(sqrt(baseRoughness * baseRoughness + karisVariance + toksvigVar));
    float reflBump = saturate(RefractionParams.w);
    float3 Nrefl = normalize(lerp(float3(0.0, 1.0, 0.0), N, reflBump));
    
    float3 R = reflect(-V, Nrefl);
    float3 reflection;
    if (Misc.y > 0.5) {
        float lod = reflectionRoughness * Misc.w;
        reflection = SpecularCube.SampleLevel(LinearClamp, R, lod).rgb * Misc.z;
    } else {
        reflection = AnalyticSky(R);
    }
    reflection *= ShadeParams.y; 
    
    float F = FresnelSchlick(0.02, NoV, ShadeParams.x);
    if (debugMode == 5) {
        return float4(F.xxx, 1.0f);
    }

    float3 body = DeepColorDensity.rgb;
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
        float2 refrUV  = screenUV + refrOfs;
        if (SampleSceneDepthLin(refrUV) < waterDepth) refrUV = screenUV;
        float3 refrColor = SceneColor.SampleLevel(LinearClamp, refrUV, 0).rgb; 

        float cosForward = waterDepth / max(length(IN.vView), 1e-3);
        float pathLen    = max(column, 0.0) / max(cosForward, 0.1);

        float opticalLen = pathLen * max(RefractionParams.z, 0.0f) * 0.08f;
        float3 transmit  = exp2(-AbsorptionColor.rgb * opticalLen);
        float3 inScatter = InScatterColor.rgb * (1.0 - exp2(-InScatterColor.w * opticalLen));
        body = refrColor * transmit + inScatter;

        float cameraSoft = saturate(waterDepth - 0.33);
        fA = softIntersect * cameraSoft;
    }

    // Sombra do sol (CSM) na água: gate dos termos solares (SSS/spec/espuma). A normal
    // geométrica é o plano d'água — offset anti-acne na vertical.
    float sunVis = SampleCSM(IN.worldPos, float3(0.0, 1.0, 0.0), IN.pos.xy);

    // SSS de crista (porte do Watercfx da Cry): luz vazando pela onda — ambiente do céu
    // + lobo forward-scatter do sol, pesados pela altura da crista e pela inclinação.
    if (WaterFXParams.x > 0.0) {
        float waveHeight = saturate((IN.worldPos.y - OceanParams1.w) * WaterFXParams.z);
        float whf = waveHeight * 0.5 + NoV * (1.0 - saturate(N.y));
        float3 sssTint = InScatterColor.rgb * WaterFXParams.x;
        float3 sssEnv = (Misc.y > 0.5)
            ? SpecularCube.SampleLevel(LinearClamp, -V, clamp(whf, 0.2, 0.7) * Misc.w).rgb * Misc.z
            : AnalyticSky(-V);
        float EdotL = saturate(dot(-SunDirection.xyz, V));
        body += sssTint * sssEnv * whf * fA;
        body += sssTint * pow(EdotL, WaterFXParams.y) *
                (SunColor.rgb * SunDirection.w) * whf * fA * sunVis;
    }

    float reflectionWeight = saturate(F * fA);
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

    float  foam    = 0.0;
    float3 foamLit = float3(0.0, 0.0, 0.0);
    float  JDebug  = 1.0;
    if (FoamParams.z > 0.0 || debugMode == 8) {
        JDebug = WaterSampleFFT(IN.worldPos.xz).w;
        foam = saturate((FoamParams.x - JDebug) / max(FoamParams.y, 1e-3));
        foam = foam * foam * (3.0 - 2.0 * foam);
        foam = saturate(foam * FoamParams.z * WaterDistanceFade(camDist, 0.0, FoamParams.w));
        // Espuma iluminada estilo Cry: parte solar gateada pela sombra + piso de céu.
        foamLit = FoamColor.rgb * (SunColor.rgb * SunDirection.w * 0.16 * sunVis + 0.30);
    }
    if (debugMode == 8) {
        return float4(saturate((1.0f - JDebug) * 2.0f), foam, saturate(JDebug), 1.0f);
    }

    float specAA = saturate(1.0 - reflectionRoughness * 1.25);
    float sunShininess = lerp(24.0, ShadeParams.z, specAA * specAA);
    float spec = SunSpecularLobes(N, V, normalize(SunDirection.xyz), sunShininess) * specAA;
    float3 sunSpecCol = SunColor.rgb * (spec * SunDirection.w * ShadeParams.w) * sunVis;
    sunSpecCol *= (1.0 - foam * FoamColor.w);
    color += sunSpecCol;

    color = lerp(color, foamLit, foam);

    return float4(color, 1.0);
}

PSOutput main(VSOutput IN) {
    PSOutput o;
    o.Color = ShadeWater(IN);

    float2 curNDC  = IN.curClip.xy  / IN.curClip.w;
    float2 prevNDC = IN.prevClip.xy / IN.prevClip.w;
    float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    return o;
}
