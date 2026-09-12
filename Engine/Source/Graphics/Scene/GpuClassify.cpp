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
            FGpuBatchKey Key{};
            const u32 Outcome = GpuClassifyCandidate(Slot, _In.Meta, _In.Materials, _In.Meshes,
                                                     _In.PoolCount, Key);
            if (Outcome != GpuClassifyIndirect) {
                ++_OutCounters[Outcome];
                continue;
            }

            // O indice do lote e limitado por construcao depois do `PoolCount` validado, entao este
            // teste so dispara quando `BatchCapacity` foi dimensionada abaixo de
            // `GpuBatchSlotCount(PoolCount)` — erro de quem alocou, e nao dado de cena. Ele existe
            // porque a capacidade viaja separada do `PoolCount` ate o shader, e la os dois podem
            // discordar sem que nada reclame.
            const u32 Batch = GpuBatchIndex(Key, _In.PoolCount);
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
                          std::vector<u32>& _OutDrawCounts, u32& _OutOverflow) {
        _OutOffsets.assign(_Counts.size(), 0u);
        _OutDrawCounts.assign(_Counts.size(), 0u);
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
                // `_OutDrawCounts` ja esta TODO zerado pelo assign, e e assim que fica: count buffer
                // zerado faz o ExecuteIndirect executar zero comandos. Nao e uma otimizacao de
                // escrita — e o contrato.
                return Acc;
            }
            _OutOffsets[I] = Acc;
            Acc += _Counts[I];
        }
        // Sem estouro: o count buffer e copia das contagens. A copia acontece depois do laco, e nao
        // dentro dele, porque so aqui se sabe que o prefixo fechou inteiro.
        for (size_t I = 0; I < _Counts.size(); ++I) _OutDrawCounts[I] = _Counts[I];
        return Acc;
    }
}
