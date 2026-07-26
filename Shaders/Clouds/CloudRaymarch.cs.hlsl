#include "../Common/DepthConfig.hlsli"

cbuffer CloudCB : register(b0) {
    row_major float4x4 InvViewProjNoTrans;
    float4 CameraPos;        // xyz = camera no frame do planeta (km)
    float4 SunDir;
    float4 SunColor;
    float4 PlanetRadii;
    float4 CloudParams;
    float4 CloudParams2;
    float4 WindParams;
    float4 MarchParams;
    float4 ScreenParams;
    float4 PhaseParams;
    float4 AtmoLink;
    row_major float4x4 PrevVPWorld;      // usado pelo CloudTemporal (mesmo CB)
    float4 TemporalParams;
    float4 SkyAmbientCol;    // xyz = ambient fisico do ceu (F3: integral do SkyView LUT)
    float4 GroundAmbientCol; // xyz = ambient fisico do chao
    row_major float4x4 InvViewProjWorld; // reconstrucao de world pos do depth
    float4 CamWorld;         // xyz = camera em unidades de mundo, w = km/unidade
    float4 CloudMisc;        // xy = resolucao full-res
};

Texture3D<float4>   BaseNoise        : register(t0);
Texture3D<float4>   DetailNoise      : register(t1);
Texture2D<float4>   WeatherMap       : register(t2);
Texture2D<float4>   TransmittanceLUT : register(t3);
Texture2D<float4>   MultiScatterLUT  : register(t4);
Texture2D<float>    SceneDepth       : register(t5);
RWTexture2D<float4> OutClouds        : register(u0);

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);

#include "CloudDensity.hlsli"
#include "CloudLighting.hlsli"

float RaySphereNearest(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}
float RaySphereFar(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + sqrt(disc);
}
bool RaySphere2(float3 ro, float3 rd, float radius, out float t0, out float t1) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    t0 = -1.0f; t1 = -1.0f;
    if (disc < 0.0f) return false;
    float s = sqrt(disc);
    t0 = -b - s; t1 = -b + s;
    return true;
}
float CloudIGN(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

CloudCtx MakeCtx() {
    CloudCtx c;
    c.bottomR = PlanetRadii.x; c.innerR = PlanetRadii.y; c.outerR = PlanetRadii.z;
    c.coverage = CloudParams.x; c.densityScale = CloudParams.y; c.noiseScale = CloudParams.z;
    c.weatherScale = CloudParams2.x; c.erosionStrength = CloudParams2.y;
    c.detailScale = CloudParams2.z; c.cloudTypeBias = CloudParams2.w;
    c.peakStrength = AtmoLink.w;
    c.wind = WindParams.xyz * CloudParams.w;
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)ScreenParams.x;
    uint H = (uint)ScreenParams.y;
    if (id.x >= W || id.y >= H) return;

    float2 uv  = (float2(id.xy) + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldFar = mul(float4(ndc, 1.0f, 1.0f), InvViewProjNoTrans);
    float3 dir = normalize(worldFar.xyz);
    float3 cam = CameraPos.xyz;

    // Intervalo do raio na casca [innerR, outerR] p/ camera abaixo/dentro/acima da camada.
    float i0, i1, o0, o1;
    bool hitI = RaySphere2(cam, dir, PlanetRadii.y, i0, i1);
    bool hitO = RaySphere2(cam, dir, PlanetRadii.z, o0, o1);
    float camR = length(cam);

    float tStart, tEnd;
    if (camR < PlanetRadii.y) {          // abaixo da camada: entra ao sair da esfera interna
        tStart = hitI ? i1 : -1.0f;
        tEnd   = hitO ? o1 : -1.0f;
    } else if (camR < PlanetRadii.z) {   // dentro da camada
        tStart = 0.0f;
        tEnd   = (hitI && i0 > 0.0f) ? i0 : (hitO ? o1 : -1.0f);
    } else {                             // acima da camada
        tStart = (hitO && o0 > 0.0f) ? o0 : -1.0f;
        tEnd   = (hitI && i0 > 0.0f) ? i0 : (hitO ? o1 : -1.0f);
    }

    // Chao (planeta) e geometria da cena limitam o fim da marcha — assim ha nuvem ENTRE a
    // camera e o chao/predios quando se olha de cima, e a geometria oclui o que esta atras.
    float planetHit = RaySphereNearest(cam, dir, PlanetRadii.x);
    if (planetHit > 0.0f) tEnd = min(tEnd, planetHit);

    {
        int2 fullPx = min(int2(uv * CloudMisc.xy), int2(CloudMisc.xy) - int2(1, 1));
        float sd = SceneDepth.Load(int3(fullPx, 0));
        if (!SmileIsSky(sd)) {
            float4 wp = mul(float4(ndc, sd, 1.0f), InvViewProjWorld);
            wp.xyz /= max(wp.w, 1e-6f);
            float sceneDist = length(wp.xyz - CamWorld.xyz) * CamWorld.w; // km
            tEnd = min(tEnd, sceneDist);
        }
    }

    tStart = max(tStart, 0.0f);
    if (!hitO || tEnd <= tStart) {
        OutClouds[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // Clamp ABSOLUTO, coerente com o fade absoluto (decisao de arte: horizonte limpo).
    // Tambem poupa a marcha na faixa rasante onde o fade ja zeraria tudo.
    float maxDist = MarchParams.w;
    if (maxDist > 0.0f) tEnd = min(tEnd, maxDist);
    if (tEnd <= tStart) {
        OutClouds[id.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    int   numSteps   = max(16, (int)MarchParams.x);
    int   lightSteps = max(1,  (int)MarchParams.y);

    float segLen = tEnd - tStart;
    float dt     = segLen / (float)numSteps;

    float jitter = CloudIGN(float2(id.xy) + 5.588238f * WindParams.w);
    float t      = tStart + jitter * dt;

    CloudCtx ctx = MakeCtx();

    float g1 = PhaseParams.x, g2 = PhaseParams.y, blend = PhaseParams.z, powderStrength = PhaseParams.w;
    float atmoTopR    = AtmoLink.x;
    int   msOctaves   = max(1, (int)AtmoLink.y);
    float ambientScale = AtmoLink.z;
    float cosTheta  = dot(dir, SunDir.xyz);
    float basePhase = DualLobeHG(cosTheta, g1, g2, blend);

    float3 L = float3(0.0f, 0.0f, 0.0f);
    float  transmittance = 1.0f;

    [loop]
    for (int i = 0; i < numSteps; ++i) {
        if (t > tEnd) break;
        float3 p = cam + t * dir;

        float hf;
        float baseD = CloudBaseDensity(p, BaseNoise, WeatherMap, LinearWrapSampler, ctx, hf);
        // DECISAO DE ARTE (nao bug): fade pela distancia ABSOLUTA da camera. Isso apaga a
        // faixa rasante do horizonte (tStart 50-150 km) de proposito — tentamos preenche-la
        // com fade relativo + anti-tiling + wash analitico e o visual foi rejeitado
        // (weather map esticado/repetido em perspectiva). Horizonte limpo > paredao feio.
        if (maxDist > 0.0f)
            baseD *= saturate(1.0f - (t - 0.5f * maxDist) / (0.5f * maxDist));

        if (baseD <= 0.001f) { t += dt * 2.0f; continue; }

        float d = CloudErode(baseD, p, hf, DetailNoise, LinearWrapSampler, ctx);
        if (d > 0.001f) {
            int   lSteps = (transmittance < 0.3f) ? max(2, lightSteps / 2) : lightSteps;
            // Caminho de luz REAL: distancia ate sair da camada na direcao do sol, nao a
            // espessura vertical — com sol baixo o percurso rasante e muito maior e a
            // espessura fixa deixava a nuvem clara demais contra o sol. Cap de 15 km
            // (default ShadowTracingDistance da UE).
            float lightRange = RaySphereFar(p, SunDir.xyz, ctx.outerR);
            lightRange = clamp(lightRange, 0.1f, 15.0f);
            float lBase  = lightRange / (float)(lSteps * lSteps);
            float lightOD   = 0.0f;
            float lightDist = 0.0f;
            [loop]
            for (int j = 0; j < lSteps; ++j) {
                float stepLen = lBase * (2.0f * j + 1.0f); 
                lightDist += stepLen;
                float3 lp = p + SunDir.xyz * lightDist;
                float  lhf;
                lightOD += CloudBaseDensity(lp, BaseNoise, WeatherMap, LinearWrapSampler, ctx, lhf)
                         * ctx.densityScale * stepLen;
            }

            float  height = length(p);
            float  sunZen = dot(p / max(height, 1e-4f), SunDir.xyz);
            float3 atmoT  = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                    height, sunZen, ctx.bottomR, atmoTopR);
            float3 sunRadiance = SunColor.rgb * SunDir.w * atmoT;

            // Ambient fisico (F3): integral cos-weighted do SkyView LUT, ja em unidades de
            // radiancia da cena — segue crepusculo, transmitancia e luar sozinho.
            // A base da nuvem e iluminada pela faixa clara do horizonte, NAO pelo chao
            // (a integral do chao vai a ~preto no crepusculo e pintava a barriga de preto);
            // estilo UE SkyLightCloudBottomVisibility (default 0.5). O chao so contribui
            // quando for mais claro que meia-luz do ceu (ex.: bounce ao meio-dia).
            float3 bottomAmb  = max(GroundAmbientCol.rgb, SkyAmbientCol.rgb * 0.5f);
            float3 skyAmbient = lerp(bottomAmb, SkyAmbientCol.rgb, hf) * ambientScale;

            float sAtten = 1.0f, eAtten = 1.0f, pAtten = 1.0f;
            float sunScatter = 0.0f;
            [loop]
            for (int n = 0; n < msOctaves; ++n) {
                float ph = lerp(0.0795f, basePhase, pAtten); 
                sunScatter += sAtten * ph * exp(-lightOD * eAtten);
                sAtten *= 0.5f; eAtten *= 0.5f; pAtten *= 0.5f;
            }
            // Powder so em back-scatter: aplicado uniforme ele escurece a silhueta
            // iluminada por tras quando se olha na direcao do sol (Nubis/Schneider).
            float powderFade = saturate(cosTheta * 0.5f + 0.5f);
            float powder     = lerp(PowderTerm(d, powderStrength), 1.0f, powderFade);
            float3 sunContribution = sunRadiance * sunScatter * powder;

            float3 inLum   = sunContribution + skyAmbient;
            float  ext     = d;
            float  sampleT = exp(-ext * dt);
            float3 S       = inLum * d;
            float3 Sint    = (S - S * sampleT) / max(ext, 1e-6f);
            L += transmittance * Sint;
            transmittance *= sampleT;

            if (transmittance < 0.01f) break;
        }
        t += dt;
    }

    if (transmittance < 0.999f) {
        float3 entryPos    = cam + tStart * dir;
        float  camHeight   = length(cam);
        float  entryHeight = length(entryPos);
        float  muCam   = dot(cam / camHeight, dir);
        float  muEntry = dot(entryPos / entryHeight, dir);

        float3 Tcam   = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                camHeight, muCam, ctx.bottomR, atmoTopR);
        float3 Tentry = SampleAtmoTransmittance(TransmittanceLUT, LinearClampSampler,
                                                entryHeight, muEntry, ctx.bottomR, atmoTopR);
        float3 viewTrans = saturate(Tcam / max(Tentry, 1e-3f));

        float3 haze = SampleAtmoMultiScatter(MultiScatterLUT, LinearClampSampler, camHeight,
                                             dot(cam / camHeight, SunDir.xyz), ctx.bottomR, atmoTopR)
                    * SunDir.w * ambientScale;

        float cloudOpacity = 1.0f - transmittance;
        L = L * viewTrans + haze * (1.0f - viewTrans) * cloudOpacity;
    }

    OutClouds[id.xy] = float4(L, transmittance);
}
