#include "ReGIRCommon.hlsli"

cbuffer ReGIRCB : register(b0) {
    float4 GridMinSlots;
    float4 CellSizeHistory;
    float4 GridCountLights;
    float4 BuildParams;
};

StructuredBuffer<FReGIRReservoir> Slots : register(t0);
RWStructuredBuffer<float> CellAverage : register(u0);

groupshared float SharedWeight[64];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint slotsPerCell = (uint)GridMinSlots.w;
    SharedWeight[tid.x] = (tid.x < slotsPerCell)
        ? Slots[gid.x * slotsPerCell + tid.x].AverageWeight : 0.0f;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (tid.x < stride) SharedWeight[tid.x] += SharedWeight[tid.x + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid.x == 0u)
        CellAverage[gid.x] = SharedWeight[0] / max((float)slotsPerCell, 1.0f);
}
