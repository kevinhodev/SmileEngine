#include "DDGICommon.hlsli"

// Este passe precisa somente do prefixo agendado e da geometria das cascatas, mas recebe o mesmo
// CBV dos demais. Os 24 float4 iniciais terminam em MiscParams3; o static_assert no C++ prende
// GICascadeParams em 384 bytes. Declarar o prefixo como array evita copiar 24 nomes sem criar um
// segundo layout que pudesse deslocar a cauda em silencio.
cbuffer DDGICompactCB : register(b0) {
    float4 DDGIPrefix[24];
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 GICascadeScrollDelta[4];
};

Buffer<float4> ProbeData : register(t0);
RWBuffer<uint> ActiveProbeIndices : register(u0);
RWBuffer<uint> ActiveProbeCount   : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint probeIdx = DTid.x;
    const uint scheduled = (uint)max((int)GICascadeParams.z, 0);
    if (probeIdx >= scheduled) return;

    const int3 count = (int3)DDGIPrefix[1].xyz; // GridCountRays
    const int cascade = DDGI_CascadeOfProbe((int)probeIdx, count);
    const int localIdx = DDGI_LocalProbeIndex((int)probeIdx, count);
    const int3 scroll = (int3)GICascadeScrollOffset[cascade].xyz;
    const int3 pc = DDGI_GeometricCoord(DDGI_ProbeCoord(localIdx, count), scroll, count);
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);

    // Uma marca inativa pertence ao ponto de mundo ANTIGO do slot. A lamina nova entra sempre;
    // Trace/Update zeram o historico dela e Relocate publica a classificacao nova no fim.
    if (ProbeData[probeIdx].w < 0.0f && !newlyExposed) return;

    uint dst;
    InterlockedAdd(ActiveProbeCount[0], 1u, dst);
    ActiveProbeIndices[dst] = probeIdx;
}
