#include "CloudNoiseCommon.hlsli"

cbuffer WeatherParams : register(b0) {
    uint Resolution;
    uint Seed;
    uint CellMult; // multiplicador INTEIRO de celulas (mantem o wrap da textura sem emenda)
    uint _Pad0;
};

RWTexture2D<float4> OutWeather : register(u0);

// Weather map procedural: R = cobertura, G = tipo de nuvem (0=stratus..1=cumulonimbus),
// B = altura de topo por regiao (peak height, estilo Nubis). Uma textura autorada pode
// substituir este bake — mesmos canais.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Resolution || id.y >= Resolution) return;

    float2 uv = (float2(id.xy) + 0.5f) / (float)Resolution;
    float  s  = (float)Seed * 0.131f;
    float2 su = uv * (float)max(CellMult, 1u);

    float coverage = PerlinFBM(float3(su, 0.3f + s), 3.0f);
    coverage = saturate(Remap(coverage, 0.35f, 0.85f, 0.0f, 1.0f));

    float cloudType = WorleyFBM(float3(su, 0.7f + s), 2.0f);
    cloudType = saturate(cloudType * 1.2f);

    // Topo varia em frequencia mais baixa que a cobertura: bancos inteiros sobem/descem.
    float peak = PerlinFBM(float3(su, 1.7f + s), 2.0f);
    peak = saturate(Remap(peak, 0.30f, 0.80f, 0.0f, 1.0f));

    OutWeather[id.xy] = float4(coverage, cloudType, peak, 1.0f);
}
