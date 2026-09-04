#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"

#include <algorithm>
#include <cstring>

namespace Smile {
    static_assert(FGpuMaterialTable::kFrameSlots == FCommandQueue::kFramesInFlight,
                  "staging versionado por frame em voo");

    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        // Folga de capacidade, pela mesma razao do SceneCapacityFor (Scene.h): o custo de
        // estourar nao e realocar um buffer de dezenas de KB, e sim que a realocacao troca o
        // recurso lido pelas duas filas — e isso exige dreno. Com folga, acrescentar material
        // numa carga aditiva cabe sem trocar recurso nenhum.
        u32 MaterialCapacityFor(u32 RowCount) {
            const u32 Slack = RowCount / 8u;
            return RowCount + (Slack > 32u ? Slack : 32u);
        }
    }

    u32 FGpuMaterialTable::RowOf(const FMaterial* _Material) const {
        if (!_Material) return kDefaultRow;
        // Mapa, e nao busca linear no vetor de linhas: isto e chamado uma vez por RENDERAVEL ao
        // montar o snapshot, entao linear seria O(renderaveis x materiais) — 2,3 milhoes de
        // comparacoes na Emerald e 3,2 na MEASURE_SEVEN, cada vez que um Ctrl+D no editor
        // remonta o snapshot.
        const auto It = RowByMaterial_.find(_Material);
        // Desconhecido acontece de verdade: o proxy do terreno e materiais criados fora do import
        // nao estao na lista de materiais da cena.
        return It == RowByMaterial_.end() ? kDefaultRow : It->second;
    }

    void FGpuMaterialTable::SetupForMaterials(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                              std::span<FMaterial* const> _Materials) {
        // Linha 0 reservada para o material default; os materiais da cena vem a partir de 1.
        RowMaterials_.clear();
        RowByMaterial_.clear();
        RowMaterials_.reserve(_Materials.size() + 1);
        RowMaterials_.push_back(nullptr);
        for (FMaterial* M : _Materials) {
            // O primeiro a chegar vence, e nao o ultimo: um ponteiro repetido na lista seria
            // duas linhas para o mesmo material, e as duas subiriam o mesmo conteudo. Nao
            // acontece hoje (ImportedMaterials guarda unique_ptr), mas o mapa nao deve inventar
            // uma politica se acontecer.
            if (M && RowByMaterial_.emplace(M, static_cast<u32>(RowMaterials_.size())).second)
                RowMaterials_.push_back(M);
        }

        const u32 Rows = static_cast<u32>(RowMaterials_.size());
        const u32 Needed = MaterialCapacityFor(Rows);

        if (!Buffer_ || Needed > Capacity_) {
            Capacity_ = Needed;
            // Misc, e nao GI/Geometry: a tabela e da CENA, e os consumidores dela hoje (RT, GI,
            // mesh lights) vao ser tres entre varios quando o raster passar a le-la na Fase 3.
            // O numero que importa para o gate da fase sai do PersistentBytes(), com rotulo
            // proprio no breakdown por recurso.
            Buffer_ = GpuResources::CreateBuffer(
                _Device, static_cast<u64>(Capacity_) * kRowBytes, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, EVramCategory::Misc,
                "GpuMaterialTable");
            Staging_ = GpuResources::CreateUploadBuffer(
                _Device, static_cast<u64>(Capacity_) * kRowBytes, kFrameSlots,
                /*ForConstantBuffer=*/false);

            if (SrvSlot_ == kInvalidSlot) SrvSlot_ = _SRVHeap.Allocate(1);
        }

        // O SRV e reescrito no MESMO slot mesmo quando o recurso nao mudou: e barato e evita um
        // caminho em que o slot existe apontando para um buffer ja liberado.
        D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
        Desc.Format                     = DXGI_FORMAT_UNKNOWN;
        Desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Desc.Buffer.NumElements         = Capacity_;
        Desc.Buffer.StructureByteStride = kRowBytes;
        _SRVHeap.CreateSRV(_Device, Buffer_.Get(), Desc, SrvSlot_);

        // O conteudo do buffer novo e indefinido; o shadow tem de refletir isso, e nao o que a
        // tabela anterior continha — senao a comparacao do primeiro RecordUpload concluiria que
        // nada mudou e a GPU leria lixo. Limpar o shadow e o que torna TODAS as linhas sujas.
        Shadow_.clear();
        Dirty_ = true;
        LastDirtyRows_ = 0;
        LastCopyRegions_ = 0;
        TotalRowsUploaded_ = 0;
    }

    void FGpuMaterialTable::Release(FTextureSRVHeap& _SRVHeap) {
        if (SrvSlot_ != kInvalidSlot) { _SRVHeap.Free(SrvSlot_, 1); SrvSlot_ = kInvalidSlot; }
        Buffer_.Reset();
        Staging_ = {};
        Capacity_ = 0;
        RowMaterials_.clear();
        RowByMaterial_.clear();
        Shadow_.clear();
        Fresh_.clear();
        Changed_.clear();
        Dirty_ = false;
        LastDirtyRows_ = 0;
        LastCopyRegions_ = 0;
        TotalRowsUploaded_ = 0;
    }

    void FGpuMaterialTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot) {
        LastDirtyRows_   = 0;
        LastCopyRegions_ = 0;
        if (!Dirty_ || !Buffer_ || !Staging_ || !_CommandList || RowMaterials_.empty()) return;
        Dirty_ = false;

        const u32 Rows = static_cast<u32>(RowMaterials_.size());
        Fresh_.resize(Rows);
        for (u32 I = 0; I < Rows; ++I)
            Fresh_[I] = FGpuMaterialRecord::FromMaterial(RowMaterials_[I]);

        if (DiffMaterialRows(Fresh_, Shadow_, Changed_) == 0) return;

        const u32 Slot = _FrameSlot % kFrameSlots;
        u8* const Base = Staging_.Slice(Slot);
        for (u32 Row : Changed_)
            std::memcpy(Base + static_cast<size_t>(Row) * kRowBytes, &Fresh_[Row], kRowBytes);

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Buffer_.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        _CommandList->ResourceBarrier(1, &B);

        // Linhas contiguas viram UMA copia. Sem isto, aplicar um sidecar (que reescreve dezenas
        // de materiais de uma vez) emitiria dezenas de CopyBufferRegion consecutivos que
        // descrevem um unico intervalo.
        const u64 SliceBase = static_cast<u64>(Slot) * Staging_.SliceBytes;
        u32 RunStart = Changed_[0];
        u32 RunEnd   = RunStart; // inclusivo
        auto FlushRun = [&]() {
            const u64 Offset = static_cast<u64>(RunStart) * kRowBytes;
            const u64 Bytes  = static_cast<u64>(RunEnd - RunStart + 1) * kRowBytes;
            _CommandList->CopyBufferRegion(Buffer_.Get(), Offset, Staging_.Get(),
                                           SliceBase + Offset, Bytes);
            ++LastCopyRegions_;
        };
        for (size_t I = 1; I < Changed_.size(); ++I) {
            if (Changed_[I] == RunEnd + 1) { RunEnd = Changed_[I]; continue; }
            FlushRun();
            RunStart = RunEnd = Changed_[I];
        }
        FlushRun();

        B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        _CommandList->ResourceBarrier(1, &B);

        LastDirtyRows_ = static_cast<u32>(Changed_.size());
        TotalRowsUploaded_ += LastDirtyRows_;
    }
}
