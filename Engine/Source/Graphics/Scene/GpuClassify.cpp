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

    u32 PrefixBatchCounts(std::span<const u32> _Counts, std::vector<u32>& _OutOffsets,
                          u32& _OutOverflow) {
        _OutOffsets.assign(_Counts.size(), 0u);
        _OutOverflow = 0u;

        u32 Acc = 0;
        for (size_t I = 0; I < _Counts.size(); ++I) {
            // A checagem vem ANTES da escrita, e nao depois: assim a entrada que falhou recebe o
            // poison junto das seguintes. Publicar o inicio de um lote que nao cabe inteiro seria
            // publicar um comeco sem fim.
            if (Acc > UINT32_MAX - _Counts[I]) {
                _OutOverflow = 1u;
                for (size_t J = I; J < _Counts.size(); ++J)
                    _OutOffsets[J] = kGpuBatchOffsetPoison;
                break;
            }
            _OutOffsets[I] = Acc;
            Acc += _Counts[I];
        }
        return Acc;
    }

    void ScatterIndirectArgs(const FGpuClassifyInput& _In, std::span<const u32> _BatchOffsets,
                             std::span<const u32> _BatchCounts, u32 _PrefixOverflow,
                             std::vector<u32>& _InOutCursors,
                             std::vector<FGpuIndirectDrawArgs>& _InOutArgs,
                             std::vector<u32>& _OutCounters) {
        _OutCounters.assign(GpuScatterCounterCount, 0u);
        const u32 Capacidade = static_cast<u32>(_InOutArgs.size());

        for (const u32 Slot : _In.Slots) {
            // Mesma abstencao do shader, e pelo mesmo motivo: offset envenenado da a volta em u32.
            if (_PrefixOverflow != 0u) {
                ++_OutCounters[GpuScatterCounterSuppressed];
                continue;
            }
            FGpuClassifyCommand Cmd{};
            const u32 Outcome = GpuClassifyCandidate(Slot, _In.Meta, _In.Materials, _In.Meshes,
                                                     _In.PoolCount, Cmd);
            if (Outcome != GpuClassifyIndirect) continue;

            // ---- a partir daqui a posicao esta reservada: falha e REGISTRADA, nunca silenciosa ----
            const u32 Cursor = _InOutCursors[Cmd.Batch]++;
            if (Cursor >= _BatchCounts[Cmd.Batch]) {
                ++_OutCounters[GpuScatterCounterFaultCursor];
                continue;
            }
            const u32 Position = _BatchOffsets[Cmd.Batch] + Cursor;
            if (Position >= Capacidade) {
                ++_OutCounters[GpuScatterCounterFaultCapacity];
                continue;
            }
            _InOutArgs[Position] = Cmd.Args;
            ++_OutCounters[GpuScatterCounterEmitted];
        }
    }

    u32 FinalizeDrawCounts(std::span<const u32> _BatchCounts, std::span<const u32> _PrefixCounters,
                           std::span<const u32> _ScatterCounters, std::span<const u32> _BatchCursors,
                           std::span<const u32> _ClassifyCounters, u32 _CandidateCount,
                           std::vector<u32>& _OutDrawCounts, u32& _OutReason) {
        u32 Motivo = 0;
        if (_PrefixCounters[GpuPrefixCounterOverflow] != 0u)       Motivo |= GpuFinalizePrefixOverflow;
        if (_ScatterCounters[GpuScatterCounterFaultCursor] != 0u)   Motivo |= GpuFinalizeFaultCursor;
        if (_ScatterCounters[GpuScatterCounterFaultCapacity] != 0u) Motivo |= GpuFinalizeFaultCapacity;

        // A ancora: candidatos desta execucao, que nao acumulam. Ver o shader.
        u32 SomaClassify = 0;
        for (u32 K = 0; K < GpuClassifyOutcomeCount; ++K) SomaClassify += _ClassifyCounters[K];
        if (SomaClassify != _CandidateCount) Motivo |= GpuFinalizeClassifyAnchor;

        const u32 Indiretos = _ClassifyCounters[GpuClassifyIndirect];
        u32 SomaLotes = 0;
        for (size_t I = 0; I < _BatchCounts.size(); ++I) {
            SomaLotes += _BatchCounts[I];
            if (_BatchCursors[I] != _BatchCounts[I]) Motivo |= GpuFinalizeCursorMismatch;
        }
        if (SomaLotes != Indiretos) Motivo |= GpuFinalizeBatchSum;

        const u32 Tratados = _ScatterCounters[GpuScatterCounterEmitted] +
                             _ScatterCounters[GpuScatterCounterFaultCursor] +
                             _ScatterCounters[GpuScatterCounterFaultCapacity];
        if (Tratados != Indiretos) Motivo |= GpuFinalizeScatterSum;

        _OutReason = Motivo;
        _OutDrawCounts.assign(_BatchCounts.size(), 0u);
        if (Motivo == 0u)
            for (size_t J = 0; J < _BatchCounts.size(); ++J) _OutDrawCounts[J] = _BatchCounts[J];
        return Motivo == 0u ? 1u : 0u;
    }
}
