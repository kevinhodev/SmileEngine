#include "Smile/Graphics/Scene/GpuClassify.h"

#include <utility>

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

    u32 GpuBuildIndirectLayout(std::span<const u64> _Capacities, u32 _Version,
                               FGpuIndirectLayout& _OutLayout) {
        // Primeiro as portas, inteiras; so depois, e so se couber, qualquer escrita.
        if (_Version == 0u) return GpuRegionLayoutInvalidVersion;

        // A porta vem antes de CADA parcela, e nao depois da soma: somar tudo e so entao comparar deixa
        // a propria soma em u64 dar a volta — `{UINT64_MAX, 1}` vira zero e passa como layout vazio. A
        // versao anterior justificava a falta da checagem pela ORIGEM das capacidades (contagens de
        // linhas de uma tabela em memoria), mas a origem nao faz parte da API, e o teste chama esta
        // funcao direto.
        u64 Total = 0;
        for (const u64 C : _Capacities) {
            // `C > UINT32_MAX` primeiro: e ele que garante que a subtracao a direita nao da a volta.
            if (C > UINT32_MAX || Total > UINT32_MAX - C) return GpuRegionLayoutTooManyCommands;
            Total += C;
        }

        // Daqui para baixo toda conversao e segura: cada offset e cada capacidade sao <= Total.
        //
        // As regioes vao direto para o layout de saida, sem vetor temporario: um vetor por montagem
        // alocaria a cada recontagem, e ela roda com a lista do frame aberta. O layout nao fica pela
        // metade mesmo assim — as portas ja passaram, e o `resize` e a unica coisa que ainda pode
        // falhar; falhando, falha antes de mudar qualquer campo. Com a capacidade reservada, nao aloca.
        _OutLayout.Regions_.resize(_Capacities.size());
        u64 Acc = 0;
        for (size_t B = 0; B < _Capacities.size(); ++B) {
            FGpuIndirectRegion& R = _OutLayout.Regions_[B];
            R.Offset   = static_cast<u32>(Acc);
            R.Capacity = static_cast<u32>(_Capacities[B]);
            R.Version  = _Version;
            Acc += _Capacities[B];
        }
        _OutLayout.Total_   = static_cast<u32>(Total);
        _OutLayout.Version_ = _Version;
        return GpuRegionLayoutOk;
    }

    void ScatterIndirectArgs(const FGpuClassifyInput& _In,
                             std::span<const FGpuIndirectRegion> _Regions, u32 _ExpectedVersion,
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
            if (_ExpectedVersion == 0u || R.Version != _ExpectedVersion) {
                ++_OutCounters[GpuScatterCounterFaultLayoutVersion];
                continue;
            }
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
                           std::span<const u32> _ClassifyCounters,
                           std::span<const u32> _ExpandCounters, u32 _CandidateCount,
                           std::vector<u32>& _OutDrawCounts, u32& _OutReason) {
        auto SomaAte = [](u32& _Soma, u32 _Valor, u32 _Limite) {
            if (_Valor > _Limite || _Soma > _Limite - _Valor) return false;
            _Soma += _Valor;
            return true;
        };
        u32 Motivo = 0;
        if (_ScatterCounters[GpuScatterCounterFaultCursor] != 0u)   Motivo |= GpuFinalizeFaultCursor;
        if (_ScatterCounters[GpuScatterCounterFaultLayoutVersion] != 0u)
            Motivo |= GpuFinalizeFaultLayoutVersion;
        if (_ScatterCounters[GpuScatterCounterFaultRegion] != 0u)   Motivo |= GpuFinalizeFaultRegion;
        if (_ScatterCounters[GpuScatterCounterFaultCapacity] != 0u) Motivo |= GpuFinalizeFaultCapacity;

        // As ancoras: candidatos desta execucao, que nao acumulam. Ver o shader.
        u32 SomaExpand = 0;
        bool SomaExpandValida = true;
        for (u32 E = 0; E < GpuExpandCounterCount; ++E)
            SomaExpandValida = SomaAte(SomaExpand, _ExpandCounters[E], _CandidateCount) &&
                               SomaExpandValida;
        if (!SomaExpandValida || SomaExpand != _CandidateCount) Motivo |= GpuFinalizeExpandAnchor;

        u32 SomaClassify = 0;
        bool SomaClassifyValida = true;
        for (u32 K = 0; K < GpuClassifyOutcomeCount; ++K)
            SomaClassifyValida = SomaAte(SomaClassify, _ClassifyCounters[K], _CandidateCount) &&
                                 SomaClassifyValida;
        if (!SomaClassifyValida || SomaClassify != _CandidateCount)
            Motivo |= GpuFinalizeClassifyAnchor;

        // Os canarios. Pela funcao da lista fechada, e nao por uma soma de indices escrita aqui: o shader
        // subtrai a politica da soma, e os dois so concordam se a lista for a mesma.
        for (u32 K = 0; K < GpuClassifyOutcomeCount; ++K)
            if (GpuClassifyOutcomeIsCanary(K) && _ClassifyCounters[K] != 0u)
                Motivo |= GpuFinalizeClassifyCanary;
        if (_ExpandCounters[GpuExpandCounterInvalidSlot] != 0u ||
            _ExpandCounters[GpuExpandCounterRtOnly] != 0u ||
            _ExpandCounters[GpuExpandCounterOverflow] != 0u)
            Motivo |= GpuFinalizeExpandCanary;

        const u32 Indiretos = _ClassifyCounters[GpuClassifyIndirect];
        u32 SomaLotes = 0;
        bool SomaLotesValida = true;
        for (size_t I = 0; I < _BatchCounts.size(); ++I) {
            SomaLotesValida = SomaAte(SomaLotes, _BatchCounts[I], Indiretos) && SomaLotesValida;
            if (_BatchCursors[I] != _BatchCounts[I])    Motivo |= GpuFinalizeCursorMismatch;
            if (_BatchCounts[I] > _Regions[I].Capacity) Motivo |= GpuFinalizeRegionOverflow;
        }
        if (!SomaLotesValida || SomaLotes != Indiretos) Motivo |= GpuFinalizeBatchSum;

        u32 Tratados = 0;
        bool TratadosValidos = true;
        for (u32 S = 0; S < GpuScatterCounterCount; ++S)
            TratadosValidos = SomaAte(Tratados, _ScatterCounters[S], Indiretos) && TratadosValidos;
        if (!TratadosValidos || Tratados != Indiretos) Motivo |= GpuFinalizeScatterSum;

        _OutReason = Motivo;
        _OutDrawCounts.assign(_BatchCounts.size(), 0u);
        if (Motivo == 0u)
            for (size_t J = 0; J < _BatchCounts.size(); ++J) _OutDrawCounts[J] = _BatchCounts[J];
        return Motivo == 0u ? 1u : 0u;
    }
}
