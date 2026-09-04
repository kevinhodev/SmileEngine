#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/RayTracing/RTTriangle.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h" // sizeof(Vertex): stride do SRV bindless de VB
#include "Smile/Scene/Scene.h"

#include <cstring>

namespace Smile {
    static_assert(FGpuTableStorage::kFrameSlots == FCommandQueue::kFramesInFlight,
                  "staging versionado por frame em voo");

    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        // Folga de capacidade, pela mesma razao do SceneCapacityFor (Scene.h): o custo de
        // estourar nao e realocar um buffer de dezenas de KB, e sim que a realocacao troca o
        // recurso lido pelas duas filas — e isso exige dreno. Com folga, acrescentar material ou
        // malha numa carga aditiva cabe sem trocar recurso nenhum.
        u32 TableCapacityFor(u32 RowCount) {
            const u32 Slack = RowCount / 8u;
            return RowCount + (Slack > 32u ? Slack : 32u);
        }
    }

    // ================================================================================================
    // Armazenamento
    // ================================================================================================
    void FGpuTableStorage::Create(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _RowBytes,
                                  u32 _Capacity, const char* _Label) {
        RowBytes_ = _RowBytes;
        if (!Buffer_ || _Capacity > Capacity_) {
            Capacity_ = _Capacity;
            // Misc, e nao GI/Geometry: as tabelas sao da CENA, e os consumidores de hoje (RT, GI,
            // mesh lights) vao ser tres entre varios quando o raster passar a le-las na Fase 3.
            // O numero que importa para o gate da fase sai do PersistentBytes(), com rotulo
            // proprio no breakdown por recurso.
            Buffer_ = GpuResources::CreateBuffer(
                _Device, static_cast<u64>(Capacity_) * RowBytes_, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, EVramCategory::Misc, _Label);
            Staging_ = GpuResources::CreateUploadBuffer(
                _Device, static_cast<u64>(Capacity_) * RowBytes_, kFrameSlots,
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
        Desc.Buffer.StructureByteStride = RowBytes_;
        _SRVHeap.CreateSRV(_Device, Buffer_.Get(), Desc, SrvSlot_);
    }

    void FGpuTableStorage::Release(FTextureSRVHeap& _SRVHeap) {
        if (SrvSlot_ != kInvalidSlot) { _SRVHeap.Free(SrvSlot_, 1); SrvSlot_ = kInvalidSlot; }
        Buffer_.Reset();
        Staging_  = {};
        Capacity_ = 0;
        RowBytes_ = 0;
    }

    u32 FGpuTableStorage::RecordUpload(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot,
                                       const void* _Rows, std::span<const u32> _Changed) {
        if (!Buffer_ || !Staging_ || !_CommandList || _Rows == nullptr || _Changed.empty())
            return 0;

        const u32 Slot = _FrameSlot % kFrameSlots;
        u8* const Base = Staging_.Slice(Slot);
        const u8* const Src = static_cast<const u8*>(_Rows);
        for (u32 Row : _Changed) {
            if (Row >= Capacity_) return 0; // linha fora do buffer: nao copia nada
            std::memcpy(Base + static_cast<size_t>(Row) * RowBytes_,
                        Src + static_cast<size_t>(Row) * RowBytes_, RowBytes_);
        }

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Buffer_.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        _CommandList->ResourceBarrier(1, &B);

        // Linhas contiguas viram UMA copia. Sem isto, aplicar um sidecar (que reescreve dezenas de
        // materiais de uma vez) emitiria dezenas de CopyBufferRegion consecutivos que descrevem um
        // unico intervalo. Exige _Changed ORDENADO, que e o que o DiffTableRows produz.
        const u64 SliceBase = static_cast<u64>(Slot) * Staging_.SliceBytes;
        u32 Regions  = 0;
        u32 RunStart = _Changed[0];
        u32 RunEnd   = RunStart; // inclusivo
        auto FlushRun = [&]() {
            const u64 Offset = static_cast<u64>(RunStart) * RowBytes_;
            const u64 Bytes  = static_cast<u64>(RunEnd - RunStart + 1) * RowBytes_;
            _CommandList->CopyBufferRegion(Buffer_.Get(), Offset, Staging_.Get(),
                                           SliceBase + Offset, Bytes);
            ++Regions;
        };
        for (size_t I = 1; I < _Changed.size(); ++I) {
            if (_Changed[I] == RunEnd + 1) { RunEnd = _Changed[I]; continue; }
            FlushRun();
            RunStart = RunEnd = _Changed[I];
        }
        FlushRun();

        B.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        _CommandList->ResourceBarrier(1, &B);
        return Regions;
    }

    // ================================================================================================
    // Materiais
    // ================================================================================================
    u32 FGpuMaterialTable::RowOf(const FMaterial* _Material) const {
        if (!_Material) return kDefaultRow;
        // Mapa, e nao busca linear no vetor de linhas: isto e chamado uma vez por RENDERAVEL ao
        // montar o snapshot, entao linear seria O(renderaveis x materiais) — 2,3 milhoes de
        // comparacoes na Emerald e 3,2 na MEASURE_SEVEN, cada vez que um Ctrl+D no editor
        // remonta o snapshot.
        const auto It = RowByMaterial_.find(_Material);
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
            // O primeiro a chegar vence, e nao o ultimo: um ponteiro repetido na lista seria duas
            // linhas para o mesmo material, e as duas subiriam o mesmo conteudo. Nao acontece hoje
            // (ImportedMaterials guarda unique_ptr), mas o mapa nao deve inventar uma politica se
            // acontecer.
            if (M && RowByMaterial_.emplace(M, static_cast<u32>(RowMaterials_.size())).second)
                RowMaterials_.push_back(M);
        }

        Storage_.Create(_Device, _SRVHeap, kRowBytes,
                        TableCapacityFor(static_cast<u32>(RowMaterials_.size())),
                        "GpuMaterialTable");

        // O conteudo do buffer novo e indefinido; o shadow tem de refletir isso, e nao o que a
        // tabela anterior continha — senao a comparacao do primeiro RecordUpload concluiria que
        // nada mudou e a GPU leria lixo. Limpar o shadow e o que torna TODAS as linhas sujas.
        Shadow_.clear();
        Dirty_             = true;
        LastDirtyRows_     = 0;
        LastCopyRegions_   = 0;
        TotalRowsUploaded_ = 0;
    }

    void FGpuMaterialTable::Release(FTextureSRVHeap& _SRVHeap) {
        Storage_.Release(_SRVHeap);
        RowMaterials_.clear();
        RowByMaterial_.clear();
        Shadow_.clear();
        Fresh_.clear();
        Changed_.clear();
        Dirty_             = false;
        LastDirtyRows_     = 0;
        LastCopyRegions_   = 0;
        TotalRowsUploaded_ = 0;
    }

    void FGpuMaterialTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot) {
        LastDirtyRows_   = 0;
        LastCopyRegions_ = 0;
        if (!Dirty_ || !Storage_.IsBuilt() || RowMaterials_.empty()) return;
        Dirty_ = false;

        const u32 Rows = static_cast<u32>(RowMaterials_.size());
        Fresh_.resize(Rows);
        for (u32 I = 0; I < Rows; ++I)
            Fresh_[I] = FGpuMaterialRecord::FromMaterial(RowMaterials_[I]);

        if (DiffTableRows<FGpuMaterialRecord>(Fresh_, Shadow_, Changed_) == 0) return;

        LastCopyRegions_ = Storage_.RecordUpload(_CommandList, _FrameSlot, Fresh_.data(), Changed_);
        if (LastCopyRegions_ == 0) return;
        LastDirtyRows_ = static_cast<u32>(Changed_.size());
        TotalRowsUploaded_ += LastDirtyRows_;
    }

    // ================================================================================================
    // Meshes
    // ================================================================================================
    u32 FGpuMeshTable::RowOf(const FGpuMesh* _Mesh) const {
        if (!_Mesh) return kMissingRow;
        const auto It = RowByMesh_.find(_Mesh);
        return It == RowByMesh_.end() ? kMissingRow : It->second;
    }

    u32 FGpuMeshTable::GeometrySlotOf(const FGpuMesh* _Mesh) const {
        const u32 Row = RowOf(_Mesh);
        if (Row == kMissingRow || DescriptorBase_ == kInvalidSlot) return 0;
        // A linha 0 nao tem geometria, entao a malha da linha N ocupa o bloco N-1.
        return DescriptorBase_ + (Row - 1u) * kDescriptorsPerMesh;
    }

    void FGpuMeshTable::SetupForScene(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                      const FScene& _Scene) {
        // UMA enumeracao das malhas unicas, na ordem da cena. Antes ela vivia no
        // FRaytracingScene::Build e o FDDGI reconstruia a mesma lista com um criterio "identico,
        // palavra por palavra" — que divergiu. Aqui ela tem dono.
        UniqueMeshes_.clear();
        RowMeshes_.clear();
        RowByMesh_.clear();
        RowMeshes_.push_back(nullptr); // linha 0 = malha ausente
        // Bounds locais por malha. Vem do RENDERAVEL porque e onde eles moram hoje (copiados do
        // SMeshEntry no load); todos os renderaveis da mesma malha carregam os mesmos valores,
        // entao o primeiro que aparecer decide. A duplicacao no renderavel sai na Fase 4.
        std::vector<Vec3> Mins, Maxs;
        for (const FRenderable& R : _Scene.Renderables()) {
            if (!R.Mesh || !R.Mesh->IsValid()) continue;
            if (!RowByMesh_.emplace(R.Mesh, static_cast<u32>(RowMeshes_.size())).second) continue;
            RowMeshes_.push_back(R.Mesh);
            UniqueMeshes_.push_back(R.Mesh);
            Mins.push_back(R.LocalAABBMin);
            Maxs.push_back(R.LocalAABBMax);
        }

        const u32 NumMeshes = static_cast<u32>(UniqueMeshes_.size());

        // Descritores de geometria: TRES por malha unica, [VB][IB][RTTri]. Realocados sempre que a
        // contagem muda — sao contiguos por construcao, entao nao ha free list a manter aqui.
        if (DescriptorBase_ != kInvalidSlot) {
            _SRVHeap.Free(DescriptorBase_, DescriptorCount_);
            DescriptorBase_  = kInvalidSlot;
            DescriptorCount_ = 0;
        }
        if (NumMeshes > 0) {
            DescriptorCount_ = NumMeshes * kDescriptorsPerMesh;
            DescriptorBase_  = _SRVHeap.Allocate(DescriptorCount_);

            D3D12_SHADER_RESOURCE_VIEW_DESC GeoSrv{};
            GeoSrv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            GeoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            GeoSrv.Format                  = DXGI_FORMAT_UNKNOWN;
            for (u32 I = 0; I < NumMeshes; ++I) {
                const FGpuMesh* M      = UniqueMeshes_[I];
                const u32       VbSlot = DescriptorBase_ + I * kDescriptorsPerMesh;

                GeoSrv.Buffer.FirstElement        = M->VertexFirstElement();
                GeoSrv.Buffer.NumElements         = M->VertexCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(Vertex);
                _SRVHeap.CreateSRV(_Device, M->VertexResource(), GeoSrv, VbSlot);

                // O IB e Buffer<uint>: tipado, sem stride.
                GeoSrv.Format                     = DXGI_FORMAT_R32_UINT;
                GeoSrv.Buffer.FirstElement        = M->IndexFirstElement();
                GeoSrv.Buffer.NumElements         = M->GetIndexCount();
                GeoSrv.Buffer.StructureByteStride = 0;
                _SRVHeap.CreateSRV(_Device, M->IndexResource(), GeoSrv, VbSlot + 1);

                GeoSrv.Format                     = DXGI_FORMAT_UNKNOWN;
                GeoSrv.Buffer.FirstElement        = M->RTTriangleFirstElement();
                GeoSrv.Buffer.NumElements         = M->RTTriangleCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(FRTTriangle);
                _SRVHeap.CreateSRV(_Device,
                                   M->RTTriangleCount() > 0 ? M->RTTriangleResource() : nullptr,
                                   GeoSrv, VbSlot + 2);
            }
        }

        Storage_.Create(_Device, _SRVHeap, kRowBytes,
                        TableCapacityFor(static_cast<u32>(RowMeshes_.size())), "GpuMeshTable");

        // As linhas sao funcao pura da malha + dos descritores acabados de escrever, entao dao
        // para ser montadas aqui de uma vez; o RecordUpload do proximo frame as sobe.
        Fresh_.assign(RowMeshes_.size(), FGpuMeshRecord{});
        for (u32 I = 0; I < NumMeshes; ++I) {
            const FGpuMesh* M = UniqueMeshes_[I];
            FGpuMeshRecord& Row = Fresh_[I + 1];
            Row.LocalAABBMin  = Mins[I];
            Row.LocalAABBMax  = Maxs[I];
            Row.VertexSrv     = DescriptorBase_ + I * kDescriptorsPerMesh;
            Row.IndexSrv      = Row.VertexSrv + 1;
            Row.TriangleSrv   = Row.VertexSrv + 2;
            Row.VertexCount   = M->VertexCount();
            Row.IndexCount    = M->GetIndexCount();
            Row.TriangleCount = M->RTTriangleCount();
        }

        Shadow_.clear();
        Dirty_         = true;
        LastDirtyRows_ = 0;
    }

    void FGpuMeshTable::Release(FTextureSRVHeap& _SRVHeap) {
        if (DescriptorBase_ != kInvalidSlot) {
            _SRVHeap.Free(DescriptorBase_, DescriptorCount_);
            DescriptorBase_  = kInvalidSlot;
            DescriptorCount_ = 0;
        }
        Storage_.Release(_SRVHeap);
        UniqueMeshes_.clear();
        RowMeshes_.clear();
        RowByMesh_.clear();
        Shadow_.clear();
        Fresh_.clear();
        Changed_.clear();
        Dirty_         = false;
        LastDirtyRows_ = 0;
    }

    void FGpuMeshTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot) {
        LastDirtyRows_ = 0;
        if (!Dirty_ || !Storage_.IsBuilt() || Fresh_.empty()) return;
        Dirty_ = false;
        // Sem recalculo: ao contrario do material, nada de malha muda sem passar por um Setup —
        // geometria nao e editavel em runtime. As linhas ja estao prontas desde o SetupForScene.
        if (DiffTableRows<FGpuMeshRecord>(Fresh_, Shadow_, Changed_) == 0) return;
        if (Storage_.RecordUpload(_CommandList, _FrameSlot, Fresh_.data(), Changed_) == 0) return;
        LastDirtyRows_ = static_cast<u32>(Changed_.size());
    }
}
