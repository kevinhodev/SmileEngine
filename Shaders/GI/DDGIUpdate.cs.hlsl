// DDGI — passe de update dos probes (SM 6.0).
// 1 grupo por probe; 6x6 threads = 1 texel octaedrico cada. Cada texel integra os raios do
// ProbesTrace ponderados por cos(angulo entre a direcao do texel e a do raio) -> media =
// irradiancia/PI naquela direcao. Blend temporal in-place com o atlas (hysteresis): le o
// valor anterior do mesmo texel e converge devagar (estabilidade). Sem borda/Chebyshev
// (Fase 2). O trace JA rodou e leu o atlas do frame anterior antes desta sobrescrita.

#include "DDGICommon.hlsli"

#define DDGI_RAYS 64 // teto; o LOD adaptativo marca os nao-tracados com sentinela (pulados)
#define DDGI_TILE 6

cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;  // xyz = grid origin, w = spacing
    float4 GridCountRays;   // xyz = probe counts, w = rays per probe
    float4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
    float4 SunDirIntensity; // xyz = dir TO sun, w = sun intensity
    float4 SunColorHyst;    // rgb = sun color, w = hysteresis (blend temporal)
    float4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = normalBias
};

Texture2D<float4>   ProbesTrace : register(t0);
RWTexture2D<float4> IrradAtlas  : register(u0);

[numthreads(DDGI_TILE, DDGI_TILE, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int probeIdx  = (int)Gid.x;
    int numProbes = (int)AtlasParams.w;
    if (probeIdx >= numProbes) return;

    int3 count = (int3)GridCountRays.xyz;
    int3 pc    = DDGI_ProbeCoord(probeIdx, count);
    int  tile  = (int)AtlasParams.x; // = DDGI_TILE
    int2 tileOrigin = DDGI_TileOrigin(pc, count, tile);
    int2 local      = int2(GTid.xy);

    // Direcao deste texel octaedrico (centro do texel).
    float2 octUV = ((float2)local + 0.5f) / (float)tile; // [0,1]
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    uint   frame = (uint)TraceParams.x;
    float3 sum   = float3(0.0f, 0.0f, 0.0f);
    float  wsum  = 0.0f;

    // Backface-skip (port Flax): raios que atingiram a superficie POR TRAS (.a < 0, marcado pelo
    // trace) sao ignorados na integracao; se >10% sao backface o probe esta enterrado -> irradiancia
    // 0 (mata leak/blotch de probe dentro de parede, sem custo, sem precisar de classificacao).
    // Limite 35% (nao 10%): so probe REALMENTE enterrado vira 0. 10% disparava em probes meio-
    // embutidos em parede/canto -> com hysteresis alto, pulso lento. Folhagem ja nao emite backface.
    int  backfaceCount = 0;
    int  realCount     = 0; // raios efetivamente tracados (LOD adaptativo pode pular alguns)

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float4 tr = ProbesTrace[int2(r, probeIdx)];
        if (tr.a < -1e8f) continue; // raio nao tracado (sentinela do LOD adaptativo)
        ++realCount;
        if (tr.a < 0.0f) { ++backfaceCount; continue; } // backface
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame);
        float  w    = max(0.0f, dot(texelDir, rdir));
        if (w <= 0.0f) continue;
        sum  += tr.rgb * w;
        wsum += w;
    }

    // Enterrado se >35% dos raios REAIS forem backface (limite escalado pelo nº de raios).
    bool occluded = (realCount > 0) && (backfaceCount > (int)(realCount * 0.35f));
    // Media ponderada por cosseno = irradiancia/PI na direcao do texel.
    float3 result = (occluded || wsum <= 0.0f) ? float3(0.0f, 0.0f, 0.0f) : (sum / wsum);
    // Encode perceptual (blend em espaco gamma reduz flicker nas faixas escuras).
    result = pow(max(result, 0.0f), 1.0f / DDGI_IRRADIANCE_GAMMA);

    int2   texel = tileOrigin + local;
    float3 prev  = IrradAtlas[texel].rgb; // ja em gamma

    // Blend temporal (hysteresis) com aceleracao adaptativa: SO numa mudanca GRANDE de luz (sol
    // mexendo) reduz a hysteresis p/ reagir; conservador (limite 0.5 em espaco gamma, fator 0.6)
    // p/ NAO disparar no ruido per-frame de folhagem/probes barulhentos -> senao acelera o flicker.
    float  hyst     = SunColorHyst.w;
    float  deltaMax = max(abs(result.r - prev.r), max(abs(result.g - prev.g), abs(result.b - prev.b)));
    if (deltaMax > 0.5f) hyst *= 0.6f;

    float3 blended = lerp(result, prev, hyst);
    IrradAtlas[texel] = float4(blended, 1.0f);
}
