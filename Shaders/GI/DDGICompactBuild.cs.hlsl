#include "DDGICommon.hlsli"

// Os 24 float4 preservam o offset de GICascadeParams no DDGICB compartilhado.
cbuffer DDGICompactCB : register(b0) {
    float4 DDGIPrefix[24];
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 GICascadeScrollDelta[4];
    float4 ProbeCompactionParams;
};

// Root constants b1. A lista e construida em fases disjuntas, mantendo o contador de raios
// como autoridade: probe nova, regiao invalidada, follow-up, quota por cascata e
// round-robin restante.
cbuffer DDGISelectionPassCB : register(b1) {
    uint SelectionMode;           // 0=nova, 1=follow-up, 2=quota, 3=restante, 4=legado
    uint SelectionRayBudget;      // zero = sem teto (scheduler legado)
    uint SelectionRegularRayCost;
    uint SelectionNewRayCost;
    uint SelectionGlobalStart;
    uint SelectionAllowInactive;
    uint SelectionRegularWindow;
    uint SelectionPadding;
    uint4 SelectionCascadeCursor;
    uint4 SelectionCascadeQuota;
};

Buffer<float4> ProbeData : register(t0);
RWBuffer<uint> ActiveProbeIndices : register(u0);
// [count, rays, novas, regionais, follow-up, quota, round-robin]
RWBuffer<uint> ActiveProbeCount   : register(u1);

bool DDGI_InCascadeQuota(uint localIdx, uint cascade, uint probesPerCascade) {
    const uint quota = min(SelectionCascadeQuota[cascade], probesPerCascade);
    if (quota == 0u || probesPerCascade == 0u) return false;
    const uint cursor = SelectionCascadeCursor[cascade] % probesPerCascade;
    const uint relative = (localIdx + probesPerCascade - cursor) % probesPerCascade;
    return relative < quota;
}

bool DDGI_ReserveSelection(uint rayCost, uint categoryCounter, uint capacity,
                           out uint destination) {
    if (SelectionRayBudget != 0u) {
        [allow_uav_condition]
        while (true) {
            const uint used = ActiveProbeCount[1];
            if (used > SelectionRayBudget || rayCost > SelectionRayBudget - used) return false;
            uint observed;
            InterlockedCompareExchange(ActiveProbeCount[1], used, used + rayCost, observed);
            if (observed == used) break;
        }
    } else {
        InterlockedAdd(ActiveProbeCount[1], rayCost);
    }

    InterlockedAdd(ActiveProbeCount[0], 1u, destination);
    if (destination >= capacity) return false;
    InterlockedAdd(ActiveProbeCount[categoryCounter], 1u);
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint candidates = (uint)max((int)GICascadeParams.z, 0);
    if (candidates == 0u) return;
    const int3 count = (int3)DDGIPrefix[1].xyz; // GridCountRays
    const uint probesPerCascade = (uint)(count.x * count.y * count.z);

    uint probeIdx;
    if (SelectionMode == 3u) {
        // A quota e uma lista pequena e contigua nas root constants; mapeia diretamente
        // para o cursor local de cada cascata em vez de varrer o volume inteiro.
        uint ordinal = DTid.x;
        uint quotaCascade = 0u;
        const uint cascadeCount = min((uint)max((int)GICascadeParams.x, 1), 4u);
        [unroll]
        for (uint c = 0u; c < 4u; ++c) {
            if (c >= cascadeCount) return;
            const uint quota = min(SelectionCascadeQuota[c], probesPerCascade);
            if (ordinal < quota) { quotaCascade = c; break; }
            ordinal -= quota;
            if (c + 1u == cascadeCount) return;
        }
        const uint local = (SelectionCascadeCursor[quotaCascade] + ordinal) %
                           probesPerCascade;
        probeIdx = quotaCascade * probesPerCascade + local;
    } else {
        if (DTid.x >= candidates) return;
        probeIdx = (DTid.x + SelectionGlobalStart) % candidates;
    }
    const uint globalProbeIdx = probeIdx;

    const int cascade = DDGI_CascadeOfProbe((int)globalProbeIdx, count);
    const int localIdx = DDGI_LocalProbeIndex((int)globalProbeIdx, count);
    const int3 scroll = (int3)GICascadeScrollOffset[cascade].xyz;
    const int3 pc = DDGI_GeometricCoord(DDGI_ProbeCoord(localIdx, count), scroll, count);
    const bool newlyExposed =
        DDGI_NewlyExposed(pc, (int3)GICascadeScrollDelta[cascade].xyz, count);
    const float4 invalidateMin = DDGIPrefix[18];
    const float4 invalidateMax = DDGIPrefix[19];
    const bool invalidationActive = invalidateMin.w > 0.5f || DDGIPrefix[23].y > 0.5f;
    const float3 probePos = DDGI_ProbeWorldPos(
        pc, GICascadeGridMinSpacing[cascade].xyz,
        GICascadeGridMinSpacing[cascade].w) + ProbeData[globalProbeIdx].xyz;
    const bool regionallyInvalidated = invalidationActive &&
        all(probePos >= invalidateMin.xyz) && all(probePos <= invalidateMax.xyz);
    const bool followUp = ProbeData[globalProbeIdx].w >= 1.0f;
    const bool inQuota = DDGI_InCascadeQuota((uint)localIdx, (uint)cascade,
                                             probesPerCascade);

    bool selected = false;
    uint rayCost = SelectionRegularRayCost;
    uint categoryCounter = 6u;
    if (SelectionMode == 0u) {
        selected = newlyExposed;
        rayCost = SelectionNewRayCost;
        categoryCounter = 2u;
    } else if (SelectionMode == 1u) {
        selected = !newlyExposed && regionallyInvalidated;
        categoryCounter = 3u;
    } else if (SelectionMode == 2u) {
        selected = !newlyExposed && !regionallyInvalidated && followUp;
        categoryCounter = 4u;
    } else if (SelectionMode == 3u) {
        selected = !newlyExposed && !regionallyInvalidated && !followUp && inQuota;
        categoryCounter = 5u;
    } else if (SelectionMode == 4u) {
        const uint relative = (probeIdx + candidates -
                               (SelectionGlobalStart % candidates)) % candidates;
        selected = !newlyExposed && !regionallyInvalidated && !followUp && !inQuota &&
                   relative < min(SelectionRegularWindow, candidates);
    } else {
        selected = true; // compactacao legada: todos os candidatos ativos
    }
    if (!selected) return;

    // Lamina nova entra mesmo se o slot antigo estava inativo.
    if (ProbeData[globalProbeIdx].w < 0.0f && !newlyExposed && SelectionAllowInactive == 0u)
        return;

    uint dst;
    if (!DDGI_ReserveSelection(rayCost, categoryCounter, candidates, dst)) return;
    ActiveProbeIndices[dst] = globalProbeIdx;
}
