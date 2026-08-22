#include "../GBuffer.hlsli"
#include "DDGICommon.hlsli"

// Diagnostico pontual usando DDGI_EvaluateTapCheb, a mesma funcao do gather.
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
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
};

Texture2D<float4> GBufferB        : register(t0);
Texture2D<float>  SceneDepth      : register(t1);
Texture2D<float4> IrradianceAtlas : register(t2);
Texture2D<float4> DistanceAtlas   : register(t3);
Buffer<float4>    ProbeData       : register(t4);
RWBuffer<float4>  DiagnosticOut   : register(u0);
SamplerState      LinearClamp     : register(s0);

// Duas paginas de oito: cascata primaria e proxima durante o blend.
static const uint DDGI_POINT_PROBES = 16u;
static const uint DDGI_POINT_ROWS   = 3u + DDGI_POINT_PROBES * 3u;
static const uint DDGI_POINT_ROW_VOLUME = 2u + DDGI_POINT_PROBES * 3u;

[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    [unroll] for (uint r = 0u; r < DDGI_POINT_ROWS; ++r)
        DiagnosticOut[r] = 0.0f;

    const uint2 sizePx = max((uint2)PixelParams.zw, uint2(1u, 1u));
    const uint2 px = min((uint2)PixelParams.xy, sizePx - 1u);
    const float depth = SceneDepth.Load(int3(px, 0));
    if (depth <= 0.0f) return; // reverse-Z

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

    const int3 count = (int3)GridCount.xyz;

    const DDGICascadeChoice choice =
        DDGI_SelectCascade(worldPos, GICascadeGridMinSpacing, (int)GICascadeParams.x, count);
    const bool hasBlend = (choice.Next != choice.Primary) && (choice.PrimaryWeight < 0.999f);

    uint  probeIndices[DDGI_POINT_PROBES];
    bool  skipped[DDGI_POINT_PROBES];
    float pointDistances[DDGI_POINT_PROBES];
    float means[DDGI_POINT_PROBES];
    float deviations[DDGI_POINT_PROBES];
    float trilinearWeights[DDGI_POINT_PROBES];
    float visibilityWeights[DDGI_POINT_PROBES];
    float finalWeights[DDGI_POINT_PROBES];
    float3 irradiances[DDGI_POINT_PROBES];
    // Cada cascata normaliza seus oito taps antes do blend.
    float pageWeightSum[2] = { 0.0f, 0.0f };
    float totalWeight = 0.0f;

    [unroll]
    for (uint page = 0u; page < 2u; ++page) {
    const int    casc     = (page == 0u) ? choice.Primary : choice.Next;
    const bool   pageLive = (page == 0u) || hasBlend;
    const float4 cg       = GICascadeGridMinSpacing[casc];
    const int3   cscroll  = (int3)GICascadeScrollOffset[casc].xyz;
    const float3 biasVec = useChebyshev
        ? DDGI_SurfaceBias(N, V, cg.w, BiasParams.x, BiasParams.y) : 0.0f;
    const float3 biasPos  = worldPos + biasVec;
    const float3 g        = (biasPos - cg.xyz) / cg.w;
    const int3   base     = (int3)floor(g);
    const float3 fracPart = saturate(g - (float3)base);

    [unroll]
    for (uint k = 0u; k < 8u; ++k) {
        const uint i = page * 8u + k;
        DDGITapCheb tap;
        if (useChebyshev) {
            tap = DDGI_EvaluateTapCheb(
                (int)k, base, fracPart, biasPos, worldPos, N,
                cg.xyz, cg.w, count,
                DistanceAtlas, LinearClamp, (int)DistAtlasParams.x,
                1.0f / DistAtlasParams.yz, ProbeData, skipMode,
                DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x), casc, cscroll);
        } else {
            // Sem Chebyshev, reproduz o caminho trilinear puro do gather.
            const int3 off = int3(k & 1u, (k >> 1u) & 1u, (k >> 2u) & 1u);
            const int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);
            const float3 tri = lerp(1.0f - fracPart, fracPart, (float3)off);
            const float3 probeToPoint =
                biasPos - DDGI_ProbeWorldPos(c, cg.xyz, cg.w);

            tap.Coord       = c;
            tap.Index       = (uint)DDGI_GlobalProbeFromGeo(c, cscroll, casc, count);
            tap.Ignored     = false;
            tap.DistToProbe = length(probeToPoint);
            tap.Trilinear   = tri.x * tri.y * tri.z;
            tap.Visibility  = 1.0f;
            tap.Weight      = tap.Trilinear;

            const float2 moments = DDGI_SampleProbeRG(
                DistanceAtlas, LinearClamp,
                DDGI_TileOrigin(c, cscroll, count, (int)DistAtlasParams.x,
                                DDGI_TilesPerRow(DistAtlasParams.y, (int)DistAtlasParams.x), casc),
                (int)DistAtlasParams.x, 1.0f / DistAtlasParams.yz,
                probeToPoint / max(tap.DistToProbe, 1e-4f));
            tap.Mean  = moments.x;
            tap.Mean2 = moments.y;
        }

        const float3 irr = tap.Ignored ? 0.0f : DDGI_SampleProbe(
            IrradianceAtlas, LinearClamp,
            DDGI_TileOrigin(tap.Coord, cscroll, count, (int)AtlasParams.x,
                            DDGI_TilesPerRow(AtlasParams.y, (int)AtlasParams.x), casc),
            (int)AtlasParams.x, 1.0f / AtlasParams.yz, N);

        probeIndices[i]      = tap.Index;
        skipped[i]           = tap.Ignored || !pageLive;
        pointDistances[i]    = tap.DistToProbe;
        means[i]             = tap.Mean;
        deviations[i]        = sqrt(abs(tap.Mean2 - tap.Mean * tap.Mean));
        trilinearWeights[i]  = tap.Trilinear;
        visibilityWeights[i] = tap.Visibility;
        finalWeights[i]      = pageLive ? tap.Weight : 0.0f;
        irradiances[i]       = irr;
        pageWeightSum[page] += finalWeights[i];
        if (page == 0u) totalWeight += tap.Weight;
    }
    }

    DiagnosticOut[0] = float4(worldPos, 1.0f);
    DiagnosticOut[1] = float4(N, totalWeight);
    // Sem blend, a primaria tem escala 1 como no fast path do gather.
    const float primaryScale = hasBlend ? choice.PrimaryWeight : 1.0f;
    const float pageScale[2] = { primaryScale,
                                 hasBlend ? (1.0f - choice.PrimaryWeight) : 0.0f };
    // x = peso do volume; yzw = escolha e peso efetivo da cascata.
    DiagnosticOut[DDGI_POINT_ROW_VOLUME] = float4(
        DDGI_VolumeWeight(worldPos, GridMinSpacing.xyz, GridMinSpacing.w, count, BiasParams.z),
        (float)choice.Primary, (float)(hasBlend ? choice.Next : choice.Primary),
        primaryScale);
    [unroll]
    for (uint i = 0u; i < DDGI_POINT_PROBES; ++i) {
        const uint  pg  = i / 8u;
        const float den = pageWeightSum[pg];
        const float normalized = den > 0.0f ? pageScale[pg] * finalWeights[i] / den : 0.0f;
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
