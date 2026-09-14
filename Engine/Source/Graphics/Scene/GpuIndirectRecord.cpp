#include "Smile/Graphics/Scene/GpuIndirectRecord.h"

namespace Smile {
    u32 GpuIndirectRecordPreflight(const FGpuIndirectLayout& _Layout, u32 _PipelineClass,
                                   std::span<const FGpuPoolViews> _Pools,
                                   const FGpuIndirectDrawTargets& _Targets) {
        if (!_Targets.Signature || !_Targets.Args || !_Targets.DrawCounts)
            return GpuIndirectRecordMissingTarget;
        if (_Layout.Version() == 0u) return GpuIndirectRecordInvalidLayout;
        if (_PipelineClass >= kGpuPipelineClassCount) return GpuIndirectRecordInvalidClass;

        // O numero de pools so vira u32 depois de caber, e a conta de lotes e a MESMA porta do
        // classify: duas copias da aritmetica aprovariam por criterios diferentes.
        if (_Pools.size() > UINT32_MAX) return GpuIndirectRecordBadPoolCount;
        u32 Slots = 0;
        if (GpuClassifyBatchBufferSlots(static_cast<u32>(_Pools.size()), Slots) !=
            GpuClassifyPreflightOk)
            return GpuIndirectRecordBadPoolCount;
        if (_Layout.Regions().size() < Slots) return GpuIndirectRecordSmallRegionTable;

        // Os tamanhos em u64. O layout e denso e imutavel, entao toda regiao termina no maximo em
        // `TotalCommands()`: cobrir o total cobre toda chamada.
        //
        // A dimensao ANTES de cada tamanho: o `Width` so e bytes num buffer.
        const D3D12_RESOURCE_DESC Args = _Targets.Args->GetDesc();
        if (Args.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return GpuIndirectRecordArgsNotBuffer;
        if (Args.Width < GpuScatterArgsBytes(_Layout.TotalCommands()))
            return GpuIndirectRecordSmallArgsBuffer;
        const D3D12_RESOURCE_DESC Counts = _Targets.DrawCounts->GetDesc();
        if (Counts.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return GpuIndirectRecordCountNotBuffer;
        if (Counts.Width < static_cast<u64>(Slots) * sizeof(u32))
            return GpuIndirectRecordSmallCountBuffer;
        return GpuIndirectRecordOk;
    }

    u32 GpuRecordIndirectDraws(ID3D12GraphicsCommandList* _List, const FGpuIndirectLayout& _Layout,
                               u32 _PipelineClass, std::span<const FGpuPoolViews> _Pools,
                               const FGpuIndirectDrawTargets& _Targets, u32& _OutCalls) {
        _OutCalls = 0u;
        const u32 Motivo = GpuIndirectRecordPreflight(_Layout, _PipelineClass, _Pools, _Targets);
        if (Motivo != GpuIndirectRecordOk) return Motivo;

        const u32 PoolCount = static_cast<u32>(_Pools.size());
        const std::span<const FGpuIndirectRegion> Regioes = _Layout.Regions();
        for (u32 Pool = 0; Pool < PoolCount; ++Pool) {
            // Sob a porta aprovada o indice cabe: e menor que o total de slots, que ja passou por u32.
            const u32 Lote = GpuBatchIndex({ _PipelineClass, Pool }, PoolCount);
            const FGpuIndirectRegion& R = Regioes[Lote];
            if (R.Capacity == 0u) continue;

            _List->IASetVertexBuffers(0, 1, &_Pools[Pool].Vertex);
            _List->IASetIndexBuffer(&_Pools[Pool].Index);
            _List->ExecuteIndirect(_Targets.Signature, R.Capacity, _Targets.Args,
                                   GpuRegionArgumentOffset(R), _Targets.DrawCounts,
                                   GpuRegionCountOffset(Lote));
            ++_OutCalls;
        }
        return GpuIndirectRecordOk;
    }
}
