#include "../GBuffer.hlsli"
#include "DDGICommon.hlsli"

// Diagnostico pontual do DDGI. Uma unica thread reconstrói o ponto clicado a partir do
// depth e repete os mesmos oito taps/pesos usados por SampleDDGIIrradianceCheb.
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
};

Texture2D<float4> GBufferB        : register(t0);
Texture2D<float>  SceneDepth      : register(t1);
Texture2D<float4> IrradianceAtlas : register(t2);
Texture2D<float4> DistanceAtlas   : register(t3);
Buffer<float4>    ProbeData       : register(t4);
RWBuffer<float4>  DiagnosticOut   : register(u0);
SamplerState      LinearClamp     : register(s0);

static const uint DDGI_POINT_PROBES = 8u;
static const uint DDGI_POINT_ROWS   = 2u + DDGI_POINT_PROBES * 3u;

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
        ? DDGI_SurfaceBias(N, V, GridMinSpacing.w) : 0.0f;
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
        const int3 off = int3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
        int3 c = clamp(base + off, int3(0, 0, 0), count - 1);
        uint probeIndex = (uint)DDGI_ProbeLinear(c, count);
        bool ignored = false;

        if (useChebyshev && skipMode != 0u && ProbeData[probeIndex].w < 0.0f) {
            if (skipMode >= 2u) {
                const int3 axisMask[3] = {
                    int3(1, 0, 0), int3(0, 1, 0), int3(0, 0, 1)
                };
                bool found = false;
                [loop] for (int sd = 1; sd < 3 && !found; ++sd) {
                    [unroll] for (int ax = 0; ax < 3; ++ax) {
                        const int dir = (off[ax] != 0) ? 1 : -1;
                        const int3 sc = clamp(
                            c + axisMask[ax] * (dir * sd), int3(0, 0, 0), count - 1);
                        const uint candidate = (uint)DDGI_ProbeLinear(sc, count);
                        if (ProbeData[candidate].w >= 0.0f) {
                            c = sc;
                            probeIndex = candidate;
                            found = true;
                            break;
                        }
                    }
                }
                ignored = !found;
            } else {
                ignored = true;
            }
        }

        const float3 tri = lerp(1.0f - fracPart, fracPart, (float3)off);
        const float triWeight = useChebyshev
            ? max(tri.x * tri.y * tri.z, 0.001f)
            : tri.x * tri.y * tri.z;
        const float3 baseProbePos =
            DDGI_ProbeWorldPos(c, GridMinSpacing.xyz, GridMinSpacing.w);
        const float3 probePos = baseProbePos +
            (useChebyshev ? ProbeData[probeIndex].xyz : 0.0f);
        const float3 probeToPoint = biasPos - probePos;
        const float distToPoint = length(probeToPoint);
        const float3 dirPP = probeToPoint / max(distToPoint, 1e-4f);

        const int2 distOrigin =
            DDGI_TileOrigin(c, count, (int)DistAtlasParams.x);
        const float2 moments = DDGI_SampleProbeRG(
            DistanceAtlas, LinearClamp, distOrigin, (int)DistAtlasParams.x,
            1.0f / DistAtlasParams.yz, dirPP);
        const float mean = moments.x;
        const float sigma = sqrt(abs(moments.y - mean * mean));

        float visibility = 1.0f;
        float weight = triWeight;
        if (useChebyshev) {
            const float backface = dot(-dirPP, N) * 0.5f + 0.5f;
            weight = backface * backface + 0.05f;
            if (distToPoint > mean) {
                const float variance = abs(mean * mean - moments.y);
                const float delta = distToPoint - mean;
                const float cheb = variance / max(variance + delta * delta, 1e-8f);
                visibility = max(cheb * cheb * cheb, 0.05f);
                weight *= visibility;
            }
            weight = max(weight, 1e-6f);
            const float minWeightThreshold = 0.2f;
            if (weight < minWeightThreshold)
                weight *= (weight * weight) /
                          (minWeightThreshold * minWeightThreshold);
            weight *= triWeight;
        }
        if (ignored) weight = 0.0f;

        const int2 irrOrigin =
            DDGI_TileOrigin(c, count, (int)AtlasParams.x);
        const float3 irr = ignored ? 0.0f : DDGI_SampleProbe(
            IrradianceAtlas, LinearClamp, irrOrigin, (int)AtlasParams.x,
            1.0f / AtlasParams.yz, N);

        probeIndices[i]      = probeIndex;
        skipped[i]           = ignored;
        pointDistances[i]    = distToPoint;
        means[i]             = mean;
        deviations[i]        = sigma;
        trilinearWeights[i]  = triWeight;
        visibilityWeights[i] = visibility;
        finalWeights[i]      = weight;
        irradiances[i]       = irr;
        totalWeight         += weight;
    }

    DiagnosticOut[0] = float4(worldPos, 1.0f);
    DiagnosticOut[1] = float4(N, totalWeight);
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
