#ifndef SMILE_REGIR_SAMPLING
#define SMILE_REGIR_SAMPLING

#include "ReGIRCommon.hlsli"

// Segunda etapa do ReGIR: escolhe uma celula com jitter estocastico, sorteia reservoirs do pool
// e os resampleia pelo integrando REAL do hit (radiancia incidente * N.L). A visibilidade fica
// fora do pHat e custa um unico shadow ray para a amostra vencedora.
//
// Retorna false apenas quando o ponto nao pode usar o grid (desligado/fora dos limites). Dentro
// dele, zero e uma estimativa valida — cair no full loop quando as 8 propostas falham enviesaria
// o estimador e faria justamente os pixels dificeis pagarem o pior custo.
bool ReGIRSelectPunctual(float3 hitPos, float3 hitN,
                         float3 gridMin, float3 invCellSize, int3 gridCount,
                         uint slotsPerCell, uint shadingCandidates,
                         uint slotsSRV, uint averageSRV, uint frameIndex, uint numLights,
                         out uint selectedLight, out float3 estimate) {
    selectedLight = REGIR_INVALID_LIGHT;
    estimate = 0.0f;

    const float3 baseCoord = (hitPos - gridMin) * invCellSize;
    if (any(baseCoord < 0.0f) || any(baseCoord >= (float3)gridCount)) return false;

    uint rng = ReGIRHash(asuint(hitPos.x) ^ ReGIRHash(asuint(hitPos.y)) ^
                         ReGIRHash(asuint(hitPos.z)) ^ ReGIRHash(frameIndex + 0x6D2B79F5u));
    const float3 jitter = float3(ReGIRRand(rng), ReGIRRand(rng), ReGIRRand(rng)) - 0.5f;
    const float3 jitteredCoord = baseCoord + jitter;
    const int3 cell = clamp((int3)floor(jitteredCoord), int3(0, 0, 0), gridCount - 1);
    const uint cellIndex = (uint)cell.x + (uint)cell.y * (uint)gridCount.x +
                           (uint)cell.z * (uint)(gridCount.x * gridCount.y);

    StructuredBuffer<FReGIRReservoir> GridSlots = ResourceDescriptorHeap[slotsSRV];
    StructuredBuffer<float> GridAverage = ResourceDescriptorHeap[averageSRV];
    const float averageWeight = GridAverage[cellIndex];
    if (averageWeight <= 0.0f || slotsPerCell == 0u || shadingCandidates == 0u) return true;

    float wSum = 0.0f;
    float selectedTarget = 0.0f;
    float3 selectedContribution = 0.0f;
    [loop]
    for (uint i = 0u; i < shadingCandidates; ++i) {
        const uint localSlot = min((uint)(ReGIRRand(rng) * slotsPerCell), slotsPerCell - 1u);
        const FReGIRReservoir candidate = GridSlots[cellIndex * slotsPerCell + localSlot];
        if (candidate.LightIndex == REGIR_INVALID_LIGHT || candidate.LightIndex >= numLights ||
            candidate.SampleTarget <= 0.0f) continue;

        const float sourcePdf = candidate.SampleTarget / averageWeight;
        if (sourcePdf <= 0.0f) continue;

        float3 L; float dist;
        const float3 contribution = PunctualLightIncoming(
            SceneLights[candidate.LightIndex], hitPos, L, dist) * saturate(dot(hitN, L));
        const float target = ReGIRLuminance(contribution);
        // Casa o dominio do caminho de referencia, que descarta individualmente termos abaixo
        // deste limiar antes de emitir shadow ray.
        if (target < 1e-3f) continue;

        const float w = target / sourcePdf;
        wSum += w;
        if (ReGIRRand(rng) * wSum < w) {
            selectedLight = candidate.LightIndex;
            selectedTarget = target;
            selectedContribution = contribution;
        }
    }

    if (selectedLight != REGIR_INVALID_LIGHT) {
        const float resultWeight = (wSum / (float)shadingCandidates) / selectedTarget;
        estimate = selectedContribution * resultWeight;
    }
    return true;
}

#endif
