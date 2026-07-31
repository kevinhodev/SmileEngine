#include "ReGIRCommon.hlsli"

cbuffer ReGIRCB : register(b0) {
    float4 GridMinSlots;
    float4 CellSizeHistory;
    float4 GridCountLights;
    float4 BuildParams;
};

StructuredBuffer<FReGIRReservoir> PrevSlots : register(t0);
StructuredBuffer<FPunctualLight>   Lights    : register(t1);
RWStructuredBuffer<FReGIRReservoir> OutSlots : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint3 gridCount = (uint3)GridCountLights.xyz;
    const uint slotsPerCell = (uint)GridMinSlots.w;
    const uint cellCount = gridCount.x * gridCount.y * gridCount.z;
    const uint slotCount = cellCount * slotsPerCell;
    const uint slotIndex = dtid.x;
    if (slotIndex >= slotCount) return;

    const uint cellIndex = slotIndex / slotsPerCell;
    const uint z = cellIndex / (gridCount.x * gridCount.y);
    const uint rem = cellIndex - z * gridCount.x * gridCount.y;
    const uint y = rem / gridCount.x;
    const uint x = rem - y * gridCount.x;
    const float3 cellCenter = GridMinSlots.xyz + (float3(x, y, z) + 0.5f) * CellSizeHistory.xyz;
    const float3 halfCell = CellSizeHistory.xyz * 0.5f;
    const float minDistanceSquared = dot(halfCell, halfCell);

    const uint numLights = (uint)GridCountLights.w;
    const uint initialCandidates = (uint)BuildParams.z;
    uint rng = ReGIRHash(slotIndex ^ ReGIRHash((uint)BuildParams.x + 0xA511E9B3u));

    FReGIRReservoir fresh;
    fresh.LightIndex = REGIR_INVALID_LIGHT;
    fresh.AverageWeight = 0.0f;
    fresh.SampleTarget = 0.0f;
    fresh.HistoryLength = 1u;

    float wSum = 0.0f;
    [loop]
    for (uint i = 0; i < initialCandidates && numLights > 0u; ++i) {
        const uint lightIndex = min((uint)(ReGIRRand(rng) * numLights), numLights - 1u);
        const float target = ReGIRGridTarget(Lights[lightIndex], cellCenter, minDistanceSquared);
        // Proposta uniforme 1/N. O N aparece no peso, como exige o RIS.
        const float w = target * numLights;
        wSum += w;
        if (w > 0.0f && ReGIRRand(rng) * wSum < w) {
            fresh.LightIndex = lightIndex;
            fresh.SampleTarget = target;
        }
    }
    fresh.AverageWeight = wSum / max((float)initialCandidates, 1.0f);

    FReGIRReservoir result = fresh;
    if (BuildParams.y > 0.5f) {
        FReGIRReservoir prev = PrevSlots[slotIndex];
        if (prev.LightIndex < numLights && prev.SampleTarget > 0.0f && prev.AverageWeight > 0.0f) {
            // Luzes podem mover/mudar energia. Reponderar e mais caro que reaproveitar o peso
            // velho, mas evita a instabilidade explicitamente apontada no capitulo.
            const float newPrevTarget = ReGIRGridTarget(
                Lights[prev.LightIndex], cellCenter, minDistanceSquared);
            const float reweightedPrevAverage = prev.AverageWeight *
                                                (newPrevTarget / prev.SampleTarget);
            const uint prevM = min(prev.HistoryLength, (uint)CellSizeHistory.w - 1u);
            const float freshStreamWeight = fresh.AverageWeight;
            const float prevStreamWeight = reweightedPrevAverage * prevM;
            const float combinedWeight = freshStreamWeight + prevStreamWeight;
            const uint combinedM = 1u + prevM;
            if (prevStreamWeight > 0.0f && ReGIRRand(rng) * combinedWeight < prevStreamWeight) {
                result.LightIndex = prev.LightIndex;
                result.SampleTarget = newPrevTarget;
            }
            result.AverageWeight = combinedWeight / max((float)combinedM, 1.0f);
            result.HistoryLength = combinedM;
        }
    }
    OutSlots[slotIndex] = result;
}
