#include "Smile/Graphics/Scene/GpuClassify.h"

namespace Smile {
    u32 ClassifyCandidates(const FGpuClassifyInput& _In, std::vector<u32>& _OutBatchCounts,
                           std::vector<u32>& _OutCounters) {
        // Dimensionadas e zeradas aqui. O `assign` e deliberado sobre `resize`: uma chamada anterior
        // deixaria contagens vivas nos slots que esta nao toca, e o teste compararia acumulado da
        // CPU com zerado da GPU.
        _OutCounters.assign(GpuClassifyOutcomeCount, 0u);
        _OutBatchCounts.assign(_In.BatchCapacity, 0u);

        for (const u32 Slot : _In.Slots) {
            FGpuClassifyCommand Cmd{};
            const u32 Outcome = GpuClassifyCandidate(Slot, _In.Meta, _In.Materials, _In.Meshes,
                                                     _In.PoolCount, Cmd);
            if (Outcome != GpuClassifyIndirect) {
                ++_OutCounters[Outcome];
                continue;
            }

            // O indice do lote e limitado por construcao depois do `PoolCount` validado, entao este
            // teste so dispara quando `BatchCapacity` foi dimensionada abaixo de
            // `GpuBatchSlotCount(PoolCount)` — erro de quem alocou, e nao dado de cena. Ele existe
            // porque a capacidade viaja separada do `PoolCount` ate o shader, e la os dois podem
            // discordar sem que nada reclame.
            const u32 Batch = Cmd.Batch;
            if (Batch >= _In.BatchCapacity) {
                ++_OutCounters[GpuClassifyOverflow];
                continue;
            }

            ++_OutBatchCounts[Batch];
            ++_OutCounters[GpuClassifyIndirect];
        }
        return _OutCounters[GpuClassifyIndirect];
    }

    u32 GpuCountRegionCapacities(std::span<const FGpuSceneInstanceMeta> _Meta,
                                 std::span<const FGpuMaterialRecord> _Materials,
                                 std::span<const FGpuMeshRecord> _Meshes, u32 _PoolCount,
                                 std::vector<u64>& _OutCapacities) {
        u32 Slots = 0;
        const u32 Motivo = GpuClassifyBatchBufferSlots(_PoolCount, Slots);
        if (Motivo != GpuClassifyPreflightOk) return Motivo;

        _OutCapacities.assign(Slots, 0ull);
        for (size_t Linha = 0; Linha < _Meta.size(); ++Linha) {
            const FGpuSceneInstanceMeta& M = _Meta[Linha];
            // Exclusao PERMANENTE do raster, e nao estado de frame: o produtor de candidatos nunca poe
            // estas linhas na lista. `Visible` fica de fora deste teste de proposito — ver o header.
            if ((M.Flags & GpuSceneInstanceRaytracingOnly) != 0u) continue;

            // A linha como a GPU a veria se ela chegasse como candidata. Linha vazia, ids e pool
            // invalidos e classes fora do indireto saem pelos mesmos desfechos do dispatch, e nenhum
            // deles e `Indirect`.
            FGpuClassifyCommand Cmd{};
            if (GpuClassifyCandidate(static_cast<u32>(Linha), _Meta, _Materials, _Meshes, _PoolCount,
                                     Cmd) != GpuClassifyIndirect)
                continue;
            ++_OutCapacities[Cmd.Batch];
        }
        return GpuClassifyPreflightOk;
    }

    u32 GpuLayoutRegions(std::span<const u64> _Capacities,
                         std::vector<FGpuIndirectRegion>& _OutRegions, u32& _OutTotal) {
        // Primeiro a soma, inteira e em u64; so depois, e so se couber, qualquer escrita. A soma de
        // u64 nao da a volta aqui: cada parcela e uma contagem de linhas de uma tabela em memoria.
        u64 Total = 0;
        for (const u64 C : _Capacities) Total += C;
        if (Total > UINT32_MAX) return GpuRegionLayoutTooManyCommands;

        // Daqui para baixo toda conversao e segura: cada offset e cada capacidade sao <= Total.
        _OutRegions.assign(_Capacities.size(), FGpuIndirectRegion{});
        u64 Acc = 0;
        for (size_t B = 0; B < _Capacities.size(); ++B) {
            _OutRegions[B].Offset   = static_cast<u32>(Acc);
            _OutRegions[B].Capacity = static_cast<u32>(_Capacities[B]);
            Acc += _Capacities[B];
        }
        _OutTotal = static_cast<u32>(Total);
        return GpuRegionLayoutOk;
    }

    void ScatterIndirectArgs(const FGpuClassifyInput& _In,
                             std::span<const FGpuIndirectRegion> _Regions,
                             std::span<const u32> _BatchCounts, std::vector<u32>& _InOutCursors,
                             std::vector<FGpuIndirectDrawArgs>& _InOutArgs,
                             std::vector<u32>& _OutCounters) {
        _OutCounters.assign(GpuScatterCounterCount, 0u);
        const u32 Capacidade = static_cast<u32>(_InOutArgs.size());

        for (const u32 Slot : _In.Slots) {
            FGpuClassifyCommand Cmd{};
            const u32 Outcome = GpuClassifyCandidate(Slot, _In.Meta, _In.Materials, _In.Meshes,
                                                     _In.PoolCount, Cmd);
            if (Outcome != GpuClassifyIndirect) continue;

            // ---- a partir daqui a posicao esta reservada: falha e REGISTRADA, nunca silenciosa ----
            //
            // Mesma ordem do shader, porque o teste compara os contadores dos dois lados: com duas
            // falhas verdadeiras ao mesmo tempo, a primeira da lista e a que conta.
            const u32 Cursor = _InOutCursors[Cmd.Batch]++;
            if (Cursor >= _BatchCounts[Cmd.Batch]) {
                ++_OutCounters[GpuScatterCounterFaultCursor];
                continue;
            }
            const FGpuIndirectRegion& R = _Regions[Cmd.Batch];
            if (Cursor >= R.Capacity) {
                ++_OutCounters[GpuScatterCounterFaultRegion];
                continue;
            }
            if (static_cast<u64>(R.Offset) + R.Capacity > Capacidade) {
                ++_OutCounters[GpuScatterCounterFaultCapacity];
                continue;
            }
            _InOutArgs[R.Offset + Cursor] = Cmd.Args;
            ++_OutCounters[GpuScatterCounterEmitted];
        }
    }

    u32 FinalizeDrawCounts(std::span<const u32> _BatchCounts,
                           std::span<const FGpuIndirectRegion> _Regions,
                           std::span<const u32> _ScatterCounters, std::span<const u32> _BatchCursors,
                           std::span<const u32> _ClassifyCounters, u32 _CandidateCount,
                           std::vector<u32>& _OutDrawCounts, u32& _OutReason) {
        u32 Motivo = 0;
        if (_ScatterCounters[GpuScatterCounterFaultCursor] != 0u)   Motivo |= GpuFinalizeFaultCursor;
        if (_ScatterCounters[GpuScatterCounterFaultRegion] != 0u)   Motivo |= GpuFinalizeFaultRegion;
        if (_ScatterCounters[GpuScatterCounterFaultCapacity] != 0u) Motivo |= GpuFinalizeFaultCapacity;

        // A ancora: candidatos desta execucao, que nao acumulam. Ver o shader.
        u32 SomaClassify = 0;
        for (u32 K = 0; K < GpuClassifyOutcomeCount; ++K) SomaClassify += _ClassifyCounters[K];
        if (SomaClassify != _CandidateCount) Motivo |= GpuFinalizeClassifyAnchor;

        const u32 Indiretos = _ClassifyCounters[GpuClassifyIndirect];
        u32 SomaLotes = 0;
        for (size_t I = 0; I < _BatchCounts.size(); ++I) {
            SomaLotes += _BatchCounts[I];
            if (_BatchCursors[I] != _BatchCounts[I])    Motivo |= GpuFinalizeCursorMismatch;
            if (_BatchCounts[I] > _Regions[I].Capacity) Motivo |= GpuFinalizeRegionOverflow;
        }
        if (SomaLotes != Indiretos) Motivo |= GpuFinalizeBatchSum;

        const u32 Tratados = _ScatterCounters[GpuScatterCounterEmitted] +
                             _ScatterCounters[GpuScatterCounterFaultCursor] +
                             _ScatterCounters[GpuScatterCounterFaultRegion] +
                             _ScatterCounters[GpuScatterCounterFaultCapacity];
        if (Tratados != Indiretos) Motivo |= GpuFinalizeScatterSum;

        _OutReason = Motivo;
        _OutDrawCounts.assign(_BatchCounts.size(), 0u);
        if (Motivo == 0u)
            for (size_t J = 0; J < _BatchCounts.size(); ++J) _OutDrawCounts[J] = _BatchCounts[J];
        return Motivo == 0u ? 1u : 0u;
    }
}
