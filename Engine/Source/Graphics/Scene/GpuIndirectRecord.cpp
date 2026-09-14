#include "Smile/Graphics/Scene/GpuIndirectRecord.h"
#include "Smile/Graphics/Resources/Mesh.h"

namespace Smile {
    namespace {
        // Um pool ganha chamada quando a regiao do seu lote tem capacidade. UMA regra para a porta e
        // para a gravacao: a porta confere as views exatamente dos pools que a gravacao vai amarrar.
        bool GanhaChamada(const FGpuIndirectRegion& _R) { return _R.Capacity != 0u; }
    }

    u32 GpuPoolViewsFor(std::span<const FGpuGeometryPool> _Pools,
                        std::vector<FGpuPoolViews>& _OutViews) {
        std::vector<FGpuPoolViews> Views(_Pools.size());
        for (size_t P = 0; P < _Pools.size(); ++P) {
            const FGpuGeometryPool& Pool = _Pools[P];
            if (!Pool.Vertex) return GpuPoolViewsMissingResource;
            if (!Pool.Index) return GpuPoolViewsMissingResource;
            const D3D12_RESOURCE_DESC Vb = Pool.Vertex->GetDesc();
            const D3D12_RESOURCE_DESC Ib = Pool.Index->GetDesc();
            if (Vb.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return GpuPoolViewsNotBuffer;
            if (Ib.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return GpuPoolViewsNotBuffer;
            u32 VbBytes = 0, IbBytes = 0;
            u32 Motivo = GpuPoolViewBytes(Vb.Width, static_cast<u32>(sizeof(Vertex)), VbBytes);
            if (Motivo != GpuPoolViewsOk) return Motivo;
            Motivo = GpuPoolViewBytes(Ib.Width, static_cast<u32>(sizeof(u32)), IbBytes);
            if (Motivo != GpuPoolViewsOk) return Motivo;

            Views[P].Vertex.BufferLocation = Pool.Vertex->GetGPUVirtualAddress();
            Views[P].Vertex.SizeInBytes    = VbBytes;
            Views[P].Vertex.StrideInBytes  = static_cast<u32>(sizeof(Vertex));
            Views[P].Index.BufferLocation  = Pool.Index->GetGPUVirtualAddress();
            Views[P].Index.SizeInBytes     = IbBytes;
            Views[P].Index.Format          = DXGI_FORMAT_R32_UINT;
        }
        _OutViews = std::move(Views);
        return GpuPoolViewsOk;
    }

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

        // As views de TODO pool que vai ganhar chamada, antes da primeira: conferidas dentro do laco
        // da gravacao, o pool 0 ja estaria gravado quando o pool 1 reprovasse.
        const u32 PoolCount = static_cast<u32>(_Pools.size());
        const std::span<const FGpuIndirectRegion> Regioes = _Layout.Regions();
        for (u32 Pool = 0; Pool < PoolCount; ++Pool) {
            if (!GanhaChamada(Regioes[GpuBatchIndex({ _PipelineClass, Pool }, PoolCount)])) continue;
            const D3D12_VERTEX_BUFFER_VIEW& V = _Pools[Pool].Vertex;
            if (V.BufferLocation == 0) return GpuIndirectRecordBadVertexView;
            if (V.StrideInBytes != sizeof(Vertex)) return GpuIndirectRecordBadVertexView;
            if (V.SizeInBytes < sizeof(Vertex)) return GpuIndirectRecordBadVertexView;
            const D3D12_INDEX_BUFFER_VIEW& I = _Pools[Pool].Index;
            if (I.BufferLocation == 0) return GpuIndirectRecordBadIndexView;
            if (I.Format != DXGI_FORMAT_R32_UINT) return GpuIndirectRecordBadIndexView;
            if (I.SizeInBytes < sizeof(u32)) return GpuIndirectRecordBadIndexView;
        }
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
            if (!GanhaChamada(R)) continue;

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
