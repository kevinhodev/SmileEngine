// Shadow map das nuvens (estilo UE CloudShadowmap / Cry cloud shadows): grade XZ em mundo
// centrada na camera; cada texel parte da BASE da camada e marcha na direcao da key light
// acumulando densidade -> transmitancia [0..1]. Consumido no DeferredLighting (t4) p/
// atenuar o sol/lua no chao. Curvatura do planeta desprezada na grade (~30 m em 20 km).
cbuffer CloudCB : register(b0) {
    row_major float4x4 InvViewProjNoTrans;
    float4 CameraPos;
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
    row_major float4x4 PrevVPWorld;
    float4 TemporalParams;
    float4 SkyAmbientCol;
    float4 GroundAmbientCol;
    row_major float4x4 InvViewProjWorld;
    float4 CamWorld;
    float4 CloudMisc;
    float4 CloudShadowCB;    // xy = centro XZ (km), z = extent (km), w = resolucao
};

Texture3D<float4>   BaseNoise        : register(t0);
Texture3D<float4>   DetailNoise      : register(t1);
Texture2D<float4>   WeatherMap       : register(t2);
Texture2D<float4>   TransmittanceLUT : register(t3); // nao usado; mantem a tabela t0-t4
Texture2D<float4>   MultiScatterLUT  : register(t4); // idem
RWTexture2D<float>  OutShadow        : register(u0);

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);

#include "CloudDensity.hlsli"

float ShadowRaySphereFar(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + sqrt(disc);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint Res = (uint)CloudShadowCB.w;
    if (id.x >= Res || id.y >= Res) return;

    CloudCtx ctx;
    ctx.bottomR = PlanetRadii.x; ctx.innerR = PlanetRadii.y; ctx.outerR = PlanetRadii.z;
    ctx.coverage = CloudParams.x; ctx.densityScale = CloudParams.y; ctx.noiseScale = CloudParams.z;
    ctx.weatherScale = CloudParams2.x; ctx.erosionStrength = CloudParams2.y;
    ctx.detailScale = CloudParams2.z; ctx.cloudTypeBias = CloudParams2.w;
    ctx.peakStrength = AtmoLink.w;
    ctx.wind = WindParams.xyz * CloudParams.w;

    // Key light abaixo do horizonte: sem sombra direcional (CPU ja zera a forca, isto e
    // so um guarda-corpo contra lixo no primeiro frame).
    if (SunDir.y <= 0.01f) { OutShadow[id.xy] = 1.0f; return; }

    float2 uv    = (float2(id.xy) + 0.5f) / (float)Res;
    float2 posKm = CloudShadowCB.xy + (uv - 0.5f) * CloudShadowCB.z;
    float3 p     = float3(posKm.x, PlanetRadii.y, posKm.y); // base da camada

    // Mesma marcha quadratica do light march do raymarch (sem erosao, cap 15 km).
    float lightRange = ShadowRaySphereFar(p, SunDir.xyz, ctx.outerR);
    lightRange = clamp(lightRange, 0.1f, 15.0f);

    const int Steps = 8;
    float lBase = lightRange / (float)(Steps * Steps);
    float od = 0.0f, dist = 0.0f;
    [unroll]
    for (int j = 0; j < Steps; ++j) {
        float stepLen = lBase * (2.0f * j + 1.0f);
        dist += stepLen;
        float3 lp = p + SunDir.xyz * dist;
        float  hf;
        od += CloudBaseDensity(lp, BaseNoise, WeatherMap, LinearWrapSampler, ctx, hf)
            * ctx.densityScale * stepLen;
    }

    OutShadow[id.xy] = exp(-od);
}
