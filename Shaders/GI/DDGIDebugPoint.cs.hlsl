#include "../GBuffer.hlsli"
#include "DDGICommon.hlsli"

// Diagnostico pontual do DDGI. Uma unica thread reconstrói o ponto clicado a partir do
// depth e roda os mesmos oito taps do gather — literalmente a mesma funcao
// (DDGI_EvaluateTapCheb, em DDGICommon.hlsli), nao uma copia dela. Diagnostico que
// reimplementa o que audita passa a mentir no dia em que o original muda.
//
// Saida:
//   [0] ponto.xyz, valido
//   [1] normal.xyz, soma dos pesos
//   [2 + i*3]     probeIndex (negativo = inativa/ignorada), distancia ao ponto, media, sigma
//   [2 + i*3 + 1] peso trilinear, visibilidade Chebyshev, peso bruto final, peso normalizado
//   [2 + i*3 + 2] irradiancia.rgb para a normal do ponto, risco de leak

cbuffer DDGIPointDebugCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 DistAtlasParams;
    float4 CameraPositionFlags; // xyz = camera; w = GI flags
    float4 PixelParams;         // xy = pixel interno; zw = tamanho interno
    float4 BiasParams;          // x = escala do bias, y = teto em metros (0 = sem teto),
                                // z = largura do fade de borda em celulas (0 = desligado)
};

Texture2D<float4> GBufferB        : register(t0);
Texture2D<float>  SceneDepth      : register(t1);
Texture2D<float4> IrradianceAtlas : register(t2);
Texture2D<float4> DistanceAtlas   : register(t3);
Buffer<float4>    ProbeData       : register(t4);
RWBuffer<float4>  DiagnosticOut   : register(u0);
SamplerState      LinearClamp     : register(s0);

static const uint DDGI_POINT_PROBES = 8u;
// +1 no fim: peso do volume (fade de borda). Acrescentado no FIM de proposito — as linhas por
// probe ficam nos mesmos indices e o parser do editor nao se desloca.
static const uint DDGI_POINT_ROWS   = 3u + DDGI_POINT_PROBES * 3u;
static const uint DDGI_POINT_ROW_VOLUME = 2u + DDGI_POINT_PROBES * 3u;

[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    [unroll] for (uint r = 0u; r < DDGI_POINT_ROWS; ++r)
        DiagnosticOut[r] = 0.0f;

    const uint2 sizePx = max((uint2)PixelParams.zw, uint2(1u, 1u));
    const uint2 px = min((uint2)PixelParams.xy, sizePx - 1u);
    const float depth = SceneDepth.Load(int3(px, 0));
    if (depth <= 0.0f) return; // reverse-Z: zero = fundo

    const float2 uv  = (float2(px) + 0.5f) / float2(sizePx);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldH = mul(float4(ndc, depth, 1.0f), InvViewProj);
    if (abs(worldH.w) < 1e-8f) return;

    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 N = GBuffer_OctDecode(GBufferB.Load(int3(px, 0)).rg);
    const float3 V = normalize(CameraPositionFlags.xyz - worldPos);
    const int flags = (int)CameraPositionFlags.w;
    const bool useChebyshev = (flags & 1) != 0;
    const bool skipInactive = (flags & 2) != 0;
    const bool useFallback  = (flags & 4) != 0;
    const uint skipMode = skipInactive ? (useFallback ? 2u : 1u) : 0u;

    const float3 biasVec = useChebyshev
        ? DDGI_SurfaceBias(N, V, GridMinSpacing.w, BiasParams.x, BiasParams.y) : 0.0f;
    const float3 biasPos = worldPos + biasVec;
    const float3 g = (biasPos - GridMinSpacing.xyz) / GridMinSpacing.w;
    const int3 base = (int3)floor(g);
    const float3 fracPart = saturate(g - (float3)base);
    const int3 count = (int3)GridCount.xyz;

    uint  probeIndices[DDGI_POINT_PROBES];
    bool  skipped[DDGI_POINT_PROBES];
    float pointDistances[DDGI_POINT_PROBES];
    float means[DDGI_POINT_PROBES];
    float deviations[DDGI_POINT_PROBES];
    float trilinearWeights[DDGI_POINT_PROBES];
    float visibilityWeights[DDGI_POINT_PROBES];
    float finalWeights[DDGI_POINT_PROBES];
    float3 irradiances[DDGI_POINT_PROBES];
    float totalWeight = 0.0f;

    [unroll]
    for (uint i = 0u; i < DDGI_POINT_PROBES; ++i) {
        DDGITapCheb tap;
        if (useChebyshev) {
            // A MESMA funcao que o SampleDDGIIrradianceCheb usa p/ pesar cada probe: e o
            // que garante que o numero relatado aqui e o numero que iluminou o pixel.
            tap = DDGI_EvaluateTapCheb(
                (int)i, base, fracPart, biasPos, worldPos, N,
                GridMinSpacing.xyz, GridMinSpacing.w, count,
                DistanceAtlas, LinearClamp, (int)DistAtlasParams.x,
                1.0f / DistAtlasParams.yz, ProbeData, skipMode,
                DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x));
        } else {
            // Com o Chebyshev desligado o consumidor e o SampleDDGIIrradiance: trilinear
            // puro, sem bias, sem relocacao e sem skip de probe inativa. Os momentos ainda
            // sao lidos p/ o painel mostrar o que o teste de visibilidade DIRIA se ligado.
            const int3 off = int3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
            const int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);
            const float3 tri = lerp(1.0f - fracPart, fracPart, (float3)off);
            const float3 probeToPoint =
                biasPos - DDGI_ProbeWorldPos(c, GridMinSpacing.xyz, GridMinSpacing.w);

            tap.Coord       = c;
            tap.Index       = (uint)DDGI_ProbeLinear(c, count);
            tap.Ignored     = false;
            tap.DistToProbe = length(probeToPoint);
            tap.Trilinear   = tri.x * tri.y * tri.z;
            tap.Visibility  = 1.0f;
            tap.Weight      = tap.Trilinear;

            const float2 moments = DDGI_SampleProbeRG(
                DistanceAtlas, LinearClamp,
                DDGI_TileOrigin(c, count, (int)DistAtlasParams.x,
                                DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x)),
                (int)DistAtlasParams.x, 1.0f / DistAtlasParams.yz,
                probeToPoint / max(tap.DistToProbe, 1e-4f));
            tap.Mean  = moments.x;
            tap.Mean2 = moments.y;
        }

        const float3 irr = tap.Ignored ? 0.0f : DDGI_SampleProbe(
            IrradianceAtlas, LinearClamp,
            DDGI_TileOrigin(tap.Coord, count, (int)AtlasParams.x,
                            DDGI_TilesPerRow(AtlasParams.y, (int)AtlasParams.x)),
            (int)AtlasParams.x, 1.0f / AtlasParams.yz, N);

        probeIndices[i]      = tap.Index;
        skipped[i]           = tap.Ignored;
        pointDistances[i]    = tap.DistToProbe;
        means[i]             = tap.Mean;
        deviations[i]        = sqrt(abs(tap.Mean2 - tap.Mean * tap.Mean));
        trilinearWeights[i]  = tap.Trilinear;
        visibilityWeights[i] = tap.Visibility;
        finalWeights[i]      = tap.Weight;
        irradiances[i]       = irr;
        totalWeight         += tap.Weight;
    }

    DiagnosticOut[0] = float4(worldPos, 1.0f);
    DiagnosticOut[1] = float4(N, totalWeight);
    // Peso do volume no ponto: 1 = dentro (gather integral), <1 = mistura com o ambiente de
    // fora, 0 = so ambiente. Os pesos por probe acima NAO mudam com o fade — sem publicar isto,
    // mexer no slider reexecutaria o diagnostico e nada no painel se moveria, exatamente a
    // impressao de "knob morto" que o re-disparo veio corrigir.
    DiagnosticOut[DDGI_POINT_ROW_VOLUME] = float4(
        DDGI_VolumeWeight(worldPos, GridMinSpacing.xyz, GridMinSpacing.w, count, BiasParams.z),
        0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint i = 0u; i < DDGI_POINT_PROBES; ++i) {
        const float normalized = totalWeight > 0.0f
            ? finalWeights[i] / totalWeight : 0.0f;
        const float risk = useChebyshev
            ? normalized * (1.0f - visibilityWeights[i]) : 0.0f;
        const float encodedIndex = skipped[i]
            ? -(float(probeIndices[i]) + 1.0f) : float(probeIndices[i]);
        const uint row = 2u + i * 3u;
        DiagnosticOut[row] = float4(
            encodedIndex, pointDistances[i], means[i], deviations[i]);
        DiagnosticOut[row + 1u] = float4(
            trilinearWeights[i], visibilityWeights[i],
            skipped[i] ? -1.0f : finalWeights[i], normalized);
        DiagnosticOut[row + 2u] = float4(irradiances[i], risk);
    }
}
