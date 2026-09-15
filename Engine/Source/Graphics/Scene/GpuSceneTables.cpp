#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/RayTracing/RTTriangle.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h" // sizeof(Vertex): stride do SRV bindless de VB
#include "Smile/Scene/Scene.h"

#include "Smile/Core/Logger.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

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
        if (!Buffer_ || _Capacity > Capacity_ || _RowBytes != RowBytes_) {
            // ATOMICO. Tudo que pode lancar — os dois recursos e o descritor — vai para locais, e o
            // armazenamento so muda depois de os tres existirem. Atribuir a capacidade antes, ou o
            // buffer antes do staging, deixava uma falha no meio com a capacidade nova sobre o
            // buffer antigo: a porta aceitaria linhas alem dele, e a copia e o memcpy do staging
            // passariam do fim.
            //
            // O pico de memoria e o de antes: o recurso antigo ja convivia com o novo durante a
            // criacao, porque so era solto na atribuicao.
            //
            // Misc, e nao GI/Geometry: as tabelas sao da CENA, e os consumidores de hoje (RT, GI,
            // mesh lights) vao ser tres entre varios quando o raster passar a le-las na Fase 3.
            // O numero que importa para o gate da fase sai do PersistentBytes(), com rotulo
            // proprio no breakdown por recurso.
            const u64 Bytes = static_cast<u64>(_Capacity) * _RowBytes;
            Microsoft::WRL::ComPtr<ID3D12Resource> Buffer = GpuResources::CreateBuffer(
                _Device, Bytes, D3D12_RESOURCE_FLAG_NONE, kShaderReadState, EVramCategory::Misc,
                _Label);
            GpuResources::FUploadBuffer Staging = GpuResources::CreateUploadBuffer(
                _Device, Bytes, kFrameSlots, /*ForConstantBuffer=*/false);
            const u32 Slot = SrvSlot_ != kInvalidSlot ? SrvSlot_ : _SRVHeap.Allocate(1);

            // Daqui em diante nada lanca.
            Buffer_   = std::move(Buffer);
            Staging_  = std::move(Staging);
            Capacity_ = _Capacity;
            RowBytes_ = _RowBytes;
            SrvSlot_  = Slot;
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

    void FGpuTableStorage::Release(FTextureSRVHeap& _SRVHeap) noexcept {
        // Nada aqui aloca: o Free do heap devolve o slot na capacidade que o Initialize reservou, e
        // soltar ComPtr e mapeamento nao pede memoria. O slot sai do membro antes do Free para uma
        // segunda liberacao nunca devolve-lo duas vezes.
        const u32 Slot = SrvSlot_;
        SrvSlot_ = kInvalidSlot;
        if (Slot != kInvalidSlot) _SRVHeap.Free(Slot, 1);
        Buffer_.Reset();
        Staging_  = {};
        Capacity_ = 0;
        RowBytes_ = 0;
    }

    bool FGpuTableStorage::AcceptsUpload(ID3D12GraphicsCommandList* _CommandList, const void* _Rows,
                                         std::span<const u32> _Changed) const noexcept {
        if (_Changed.empty()) return true;
        if (!Buffer_ || !Staging_ || !_CommandList || _Rows == nullptr) return false;
        for (u32 Row : _Changed)
            if (Row >= Capacity_) return false;
        return true;
    }

    u32 FGpuTableStorage::RecordUpload(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot,
                                       const void* _Rows, std::span<const u32> _Changed) noexcept {
        // A porta INTEIRA antes de qualquer escrita, inclusive no staging.
        if (_Changed.empty() || !AcceptsUpload(_CommandList, _Rows, _Changed)) return 0;

        const u32 Slot = _FrameSlot % kFrameSlots;
        u8* const Base = Staging_.Slice(Slot);
        const u8* const Src = static_cast<const u8*>(_Rows);
        for (u32 Row : _Changed) {
            std::memcpy(Base + static_cast<size_t>(Row) * RowBytes_,
                        Src + static_cast<size_t>(Row) * RowBytes_, RowBytes_);
        }

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Buffer_.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = kShaderReadState;
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
        B.Transition.StateAfter  = kShaderReadState;
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
        // FAIL-CLOSED: a tabela so volta a ficar pronta na ultima linha. O mapa muda antes do
        // armazenamento, e uma excecao entre os dois deixaria linhas da cena nova sobre o buffer da
        // antiga, com `Dirty_` desligado — a porta aprovaria "nada pendente". Quem lanca sai LIBERADA.
        Built_    = false;
        Prepared_ = false;
        try {
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

            Storage_.Create(_Device, _SRVHeap, kRowBytes,
                            TableCapacityFor(static_cast<u32>(RowMaterials_.size())),
                            "GpuMaterialTable");
        } catch (...) {
            Release(_SRVHeap);
            throw;
        }

        // O conteudo do buffer novo e indefinido; o shadow tem de refletir isso, e nao o que a
        // tabela anterior continha — senao a comparacao do primeiro RecordUpload concluiria que
        // nada mudou e a GPU leria lixo. Limpar o shadow e o que torna TODAS as linhas sujas.
        Shadow_.clear();
        // As linhas publicadas acabaram de mudar (para nenhuma): o layout que as contou nao vale mais.
        ++LayoutInputsVersion_;
        Dirty_             = true;
        LastDirtyRows_     = 0;
        LastCopyRegions_   = 0;
        TotalRowsUploaded_ = 0;
        Built_             = true;
    }

    void FGpuMaterialTable::Release(FTextureSRVHeap& _SRVHeap) noexcept {
        // Sem alocacao — e o rollback do Setup sob falta de memoria. A marca vem primeiro mesmo
        // assim: a tabela deixa de estar pronta antes de qualquer coisa ser solta.
        Built_    = false;
        Prepared_ = false;
        Storage_.Release(_SRVHeap);
        RowMaterials_.clear();
        RowByMaterial_.clear();
        Shadow_.clear();
        ++LayoutInputsVersion_;
        Fresh_.clear();
        Changed_.clear();
        Dirty_             = false;
        LastDirtyRows_     = 0;
        LastCopyRegions_   = 0;
        TotalRowsUploaded_ = 0;
    }

    bool FGpuMaterialTable::PrepareUpload(ID3D12GraphicsCommandList* _CommandList) {
        Prepared_        = false;
        PreparedList_    = nullptr;
        LastDirtyRows_   = 0;
        LastCopyRegions_ = 0;
        // Fail-closed: tabela sem Setup completo nao passa por "nada pendente".
        if (!Built_) return false;

        Changed_.clear();
        PreparedAltersLayout_ = false;
        if (Dirty_) {
            const u32 Rows = static_cast<u32>(RowMaterials_.size());
            Fresh_.resize(Rows);
            for (u32 I = 0; I < Rows; ++I)
                Fresh_[I] = FGpuMaterialRecord::FromMaterial(RowMaterials_[I]);

            // A publicacao e TRANSACIONAL: o shadow e a versao so avancam depois de a copia ser
            // gravada. Recusada, nada muda — o shadow continua dizendo o que a GPU tem, e `Dirty_` fica
            // ligado para o proximo frame tentar de novo.
            FindChangedRows<FGpuMaterialRecord>(Fresh_, Shadow_, Changed_);
            if (!Storage_.AcceptsUpload(_CommandList, Fresh_.data(), Changed_)) return false;
            // O commit so redimensiona o shadow DENTRO desta reserva: depois do BeginFrame nada aloca.
            Shadow_.reserve(Rows);

            bool AlteraLayout = Shadow_.size() != Rows;
            for (u32 I : Changed_) {
                if (AlteraLayout) break;
                AlteraLayout = GpuMaterialAltersLayout(Shadow_[I], Fresh_[I]);
            }
            PreparedAltersLayout_ = AlteraLayout;
        }
        Prepared_     = true;
        PreparedList_ = _CommandList;
        return true;
    }

    bool FGpuMaterialTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList,
                                         u32 _FrameSlot) noexcept {
        if (!IsPreparedFor(_CommandList)) return false;
        Prepared_ = false;
        if (!Dirty_) return true;

        LastCopyRegions_ = Storage_.RecordUpload(_CommandList, _FrameSlot, Fresh_.data(), Changed_);
        CommitChangedRows<FGpuMaterialRecord>(Fresh_, Changed_, Shadow_);
        if (PreparedAltersLayout_) ++LayoutInputsVersion_;
        Dirty_ = false;
        LastDirtyRows_ = static_cast<u32>(Changed_.size());
        TotalRowsUploaded_ += LastDirtyRows_;
        return true;
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
        // FAIL-CLOSED, como no material: pronta so no fim, e quem lanca sai LIBERADA — inclusive o
        // range de descritores de geometria que ja tinha sido alocado e escrito.
        Built_    = false;
        Prepared_ = false;
        try {
            BuildForScene(_Device, _SRVHeap, _Scene);
        } catch (...) {
            Release(_SRVHeap);
            throw;
        }
        Shadow_.clear();
        ++LayoutInputsVersion_;
        Dirty_         = true;
        LastDirtyRows_ = 0;
        Built_         = true;
    }

    void FGpuMeshTable::BuildForScene(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
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
        GeometryPools_.clear();
        UnrepresentableOffsets_ = 0;
        UnusableIndexCounts_    = 0;
        for (u32 I = 0; I < NumMeshes; ++I) {
            const FGpuMesh* M = UniqueMeshes_[I];
            FGpuMeshRecord& Row = Fresh_[I + 1];
            Row.LocalAABBMin  = Mins[I];
            Row.LocalAABBMax  = Maxs[I];
            Row.VertexSrv     = DescriptorBase_ + I * kDescriptorsPerMesh;
            Row.IndexSrv      = Row.VertexSrv + 1;
            Row.TriangleSrv   = Row.VertexSrv + 2;
            Row.VertexCount   = M->VertexCount();
            // Ver GpuMeshIndexCountUsable: contagem que nao descreve triangulos inteiros sai do
            // caminho indireto, e nao do raster. TRIANGLELIST descarta primitiva incompleta em
            // silencio, entao sem esta marca o defeito apareceria como pedaco de malha faltando.
            if (!GpuMeshFillIndexCount(Row, M->GetIndexCount())) {
                ++UnusableIndexCounts_;
                LogError("GpuMeshTable: IndexCount " + std::to_string(M->GetIndexCount()) +
                         " nao descreve triangulos inteiros; a malha fica no fallback CPU");
            }
            Row.TriangleCount = M->RTTriangleCount();

            // O PAR de recursos, e nao so o vertex buffer. No `AddMeshesBatch` os dois sao o mesmo
            // chunk; no `AddMesh` — cena default, primitivas do editor, preview de material — sao
            // dois recursos por malha. Identidade por ponteiro: dois chunks distintos sao dois
            // recursos distintos, sempre.
            Row.GeometryPoolId = GpuGeometryPoolIdFor(
                GeometryPools_, FGpuGeometryPool{ M->VertexResource(), M->IndexResource() });

            // Conservador: quem nao cabe perde o caminho INDIRETO, e nao o raster. A politica mora
            // na funcao compartilhada porque o ramo de poison e inalcancavel a partir daqui — ele
            // exigiria um pool de 68 GB —, e uma copia inline dele nunca seria exercitada.
            if (!GpuMeshFillPoolAddress(Row, M->VertexFirstElement(), M->IndexFirstElement())) {
                ++UnrepresentableOffsets_;
                LogError("GpuMeshTable: offset de pool fora do contrato do argumento indireto "
                         "(vertice " + std::to_string(M->VertexFirstElement()) +
                         "); a malha fica no fallback CPU");
            }
        }
    }

    void FGpuMeshTable::Release(FTextureSRVHeap& _SRVHeap) noexcept {
        // Sem alocacao, como no material. O range sai dos membros antes do Free para uma segunda
        // liberacao nunca devolve-lo duas vezes ao heap.
        Built_    = false;
        Prepared_ = false;
        const u32 Base  = DescriptorBase_;
        const u32 Count = DescriptorCount_;
        DescriptorBase_  = kInvalidSlot;
        DescriptorCount_ = 0;
        if (Base != kInvalidSlot) _SRVHeap.Free(Base, Count);
        Storage_.Release(_SRVHeap);
        UniqueMeshes_.clear();
        GeometryPools_.clear();
        UnrepresentableOffsets_ = 0;
        UnusableIndexCounts_    = 0;
        RowMeshes_.clear();
        RowByMesh_.clear();
        // Sem bump: `GeometryPools_` vazio acima ja muda o numero de pools que o layout compara, e uma
        // tabela sem pool nao tem linha publicada que conte.
        Shadow_.clear();
        Fresh_.clear();
        Changed_.clear();
        Dirty_         = false;
        LastDirtyRows_ = 0;
    }

    bool FGpuMeshTable::PrepareUpload(ID3D12GraphicsCommandList* _CommandList) {
        Prepared_      = false;
        PreparedList_  = nullptr;
        LastDirtyRows_ = 0;
        if (!Built_) return false;

        Changed_.clear();
        PreparedPublishes_ = false;
        if (Dirty_) {
            // Sem recalculo: ao contrario do material, nada de malha muda sem passar por um Setup —
            // geometria nao e editavel em runtime. As linhas ja estao prontas desde o SetupForScene.
            //
            // Transacional, como no material: recusada, shadow, versao e `Dirty_` ficam como estavam.
            FindChangedRows<FGpuMeshRecord>(Fresh_, Shadow_, Changed_);
            if (!Storage_.AcceptsUpload(_CommandList, Fresh_.data(), Changed_)) return false;
            Shadow_.reserve(Fresh_.size());
            PreparedPublishes_ = !Changed_.empty() || Shadow_.size() != Fresh_.size();
        }
        Prepared_     = true;
        PreparedList_ = _CommandList;
        return true;
    }

    bool FGpuMeshTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList,
                                     u32 _FrameSlot) noexcept {
        if (!IsPreparedFor(_CommandList)) return false;
        Prepared_ = false;
        if (!Dirty_) return true;

        Storage_.RecordUpload(_CommandList, _FrameSlot, Fresh_.data(), Changed_);
        CommitChangedRows<FGpuMeshRecord>(Fresh_, Changed_, Shadow_);
        // Qualquer linha publicada conta (ver LayoutInputsVersion).
        if (PreparedPublishes_) ++LayoutInputsVersion_;
        Dirty_ = false;
        LastDirtyRows_ = static_cast<u32>(Changed_.size());
        return true;
    }

    // ================================================================================================
    // Instancias
    // ================================================================================================
    void FGpuSceneInstanceTable::SetupForScene(ID3D12Device* _Device,
                                               FTextureSRVHeap& _SRVHeap,
                                               const FScene& _Scene) {
        // FAIL-CLOSED, com uma razao a mais que nas outras tabelas: sao QUATRO armazenamentos e os
        // vetores de CPU dimensionados por eles. Uma falha na terceira coluna deixava meta e bounds
        // novos sobre transform antigo, e vetores do tamanho velho sob a capacidade nova — o
        // PrepareFrame indexaria alem deles. Pronta so no fim; quem lanca sai LIBERADA.
        Built_    = false;
        Prepared_ = false;
        try {
            const u32 Required = std::max(1u, _Scene.GpuSlots().Capacity());
            const u32 Capacity = TableCapacityFor(Required);
            MetaStorage_.Create(_Device, _SRVHeap, kMetaBytes, Capacity, "GpuSceneInstanceMeta");
            BoundsStorage_.Create(_Device, _SRVHeap, kBoundsBytes, Capacity,
                                  "GpuSceneInstanceBounds");
            TransformStorage_.Create(_Device, _SRVHeap, kTransformBytes, Capacity,
                                     "GpuSceneInstanceTransform");
            PrevTransformStorage_.Create(_Device, _SRVHeap, kTransformBytes, Capacity,
                                         "GpuSceneInstancePrevTransform");

            // Dimensionados pela capacidade DO BUFFER, e nao pela recem-calculada: o Create
            // preserva um buffer maior quando a cena nova e menor, e o Capacity() publico — o mesmo
            // que PrepareFrame usa como limite de slot — passa a ser o do buffer. Com a local, um
            // slot entre os dois valores passaria pela guarda e indexaria fora do vetor.
            const u32 RowCapacity = MetaStorage_.Capacity();
            MetaRows_.assign(RowCapacity, {});
            BoundsRows_.assign(RowCapacity, {});
            TransformRows_.assign(RowCapacity, {});
            PrevTransformRows_.assign(RowCapacity, {});
            SeenEpoch_.assign(RowCapacity, 0u);
            DirtyMarks_.assign(RowCapacity, 0u);
        } catch (...) {
            Release(_SRVHeap);
            throw;
        }

        MetaShadow_.clear();
        ++LayoutInputsVersion_;
        BoundsShadow_.clear();
        TransformShadow_.clear();
        PrevTransformShadow_.clear();
        LiveSlots_.clear();
        NextLiveSlots_.clear();
        TouchedSlots_.clear();
        SettleSlots_.clear();
        NextSettleSlots_.clear();
        PrepareEpoch_ = 0;
        PreparedStructureVersion_ = ~0ull;
        PreparedTransformVersion_ = ~0ull;
        PreparedStaticCastersVersion_ = ~0ull;
        LiveRows_ = 0;
        NeedsSettle_ = false;
        Dirty_ = false;
        LastDirtyRows_ = 0;
        LastDirtyBytes_ = 0;
        LastCopyRegions_ = 0;
        LastRowsScanned_ = 0;
        TotalBytesUploaded_ = 0;
        FitsScene_ = true;
        Built_ = true;
    }

    void FGpuSceneInstanceTable::Release(FTextureSRVHeap& _SRVHeap) noexcept {
        // Sem alocacao e com a marca primeiro, como no material.
        Built_    = false;
        Prepared_ = false;
        MetaStorage_.Release(_SRVHeap);
        BoundsStorage_.Release(_SRVHeap);
        TransformStorage_.Release(_SRVHeap);
        PrevTransformStorage_.Release(_SRVHeap);
        MetaRows_.clear();
        BoundsRows_.clear();
        TransformRows_.clear();
        PrevTransformRows_.clear();
        MetaShadow_.clear();
        ++LayoutInputsVersion_;
        BoundsShadow_.clear();
        TransformShadow_.clear();
        PrevTransformShadow_.clear();
        MetaChanged_.clear();
        BoundsChanged_.clear();
        TransformChanged_.clear();
        PrevTransformChanged_.clear();
        LiveSlots_.clear();
        NextLiveSlots_.clear();
        TouchedSlots_.clear();
        SettleSlots_.clear();
        NextSettleSlots_.clear();
        SeenEpoch_.clear();
        DirtyMarks_.clear();
        PreparedStructureVersion_ = ~0ull;
        PreparedTransformVersion_ = ~0ull;
        PreparedStaticCastersVersion_ = ~0ull;
        PrepareEpoch_ = 0;
        LiveRows_ = 0;
        NeedsSettle_ = false;
        Dirty_ = false;
        LastDirtyRows_ = 0;
        LastDirtyBytes_ = 0;
        LastCopyRegions_ = 0;
        LastRowsScanned_ = 0;
        TotalBytesUploaded_ = 0;
    }

    void FGpuSceneInstanceTable::PrepareFrame(FScene& _Scene,
                                              const FGpuMeshTable& _Meshes,
                                              const FGpuMaterialTable& _Materials) {
        // O produtor muda linhas e tocados: uma publicacao preparada antes dele nao as descreve mais.
        Prepared_ = false;
        if (!Built_) return;
        // Uma cena que cresceu alem da tabela, sem o Setup que acompanha o crescimento, tem slots que
        // nao cabem nela. Sair em silencio deixava o frame seguir com a imagem de antes; a marca faz
        // a porta recusar ate o Setup chegar.
        FitsScene_ = _Scene.GpuSlots().Capacity() <= Capacity();
        if (!FitsScene_) return;

        const u64 StructureVersion = _Scene.StructureVersion();
        const u64 TransformVersion = _Scene.TransformsVersion();
        // Visibilidade e mobilidade mudam o conteudo do mapa estatico e, portanto, esta versao.
        // Mobilidade nao e transform nem estrutura; observar so as duas anteriores deixava o bit
        // GpuSceneInstanceDynamic congelado depois de uma edicao no Outliner.
        const u64 StaticCastersVersion = _Scene.StaticCastersVersion();
        if (StructureVersion == PreparedStructureVersion_ &&
            TransformVersion == PreparedTransformVersion_ &&
            StaticCastersVersion == PreparedStaticCastersVersion_ && !NeedsSettle_) return;

        // Varredura completa quando NAO da para confiar na caixa de correio da cena: primeira
        // publicacao, mudanca de estrutura (indices se deslocam) ou alguem que bumpou sem dizer
        // quem. Versao nova com caixa vazia tambem cai aqui — nao deveria acontecer, e o desfecho
        // conservador custa uma varredura, nunca uma linha desatualizada.
        const std::span<const u32> Hints = _Scene.DirtyRenderableHints();
        const bool VersionsMoved = StructureVersion != PreparedStructureVersion_ ||
                                   TransformVersion != PreparedTransformVersion_ ||
                                   StaticCastersVersion != PreparedStaticCastersVersion_;
        const bool FullScan = PreparedStructureVersion_ == ~0ull ||
                              StructureVersion != PreparedStructureVersion_ ||
                              !_Scene.DirtyHintComplete() ||
                              (VersionsMoved && Hints.empty());

        if (++PrepareEpoch_ == 0) {
            std::fill(SeenEpoch_.begin(), SeenEpoch_.end(), 0u);
            PrepareEpoch_ = 1;
        }
        const u32 Epoch = PrepareEpoch_;
        // Uma publicacao RECUSADA deixa os tocados do ciclo anterior pendentes, com `Dirty_` ligado (ver
        // RecordUpload). Limpa-los aqui perderia linhas que a GPU nunca recebeu; somados aos deste
        // ciclo, o sort do fim tira a repeticao.
        if (!Dirty_) TouchedSlots_.clear();
        NextSettleSlots_.clear();

        // O previous de um ocupante e o current que a GPU TEM para ele: o da ultima publicacao
        // ACEITA, e nao o da ultima escrita. Uma publicacao recusada deixa `TransformRows_` a frente
        // da GPU, e derivar o previous dali apagaria um movimento que ela nunca recebeu — A→B
        // recusado e depois C publicaria C/B com a GPU ainda em A.
        //
        // A generation tambem e a PUBLICADA. Sem publicacao deste ocupante — slot nunca publicado,
        // reciclado, ou ocupante nascido numa publicacao recusada — nao ha passado na GPU, e o
        // previous e o proprio current. Conferir a generation das linhas escritas daria a um objeto
        // nascido na recusa o transform que o shadow guarda naquele slot, que nao e dele.
        auto PublishedCurrent = [&](u32 Slot, u32 Generation, const Mat44& Current) -> Mat44 {
            return Generation != 0u && Slot < MetaShadow_.size() &&
                           MetaShadow_[Slot].Generation == Generation
                       ? TransformShadow_[Slot].Value : Current;
        };

        // Recompoe as quatro colunas de UM renderavel. Devolve o slot escrito, ou kInvalidSlot
        // quando o objeto nao tem residencia — o mesmo filtro que a varredura completa aplicava
        // no laco, agora num lugar so para os dois ramos nao poderem divergir.
        auto WriteRow = [&](const FRenderable& R) -> u32 {
            const FGpuSceneHandle Handle = R.GpuHandle;
            if (!_Scene.GpuSlots().IsAlive(Handle) || Handle.Slot >= Capacity())
                return FGpuSceneHandle::kInvalidSlot;

            const u32 Slot = Handle.Slot;
            // UMA escrita por slot por ciclo: a caixa de correio repete indice de rotina. Mover um
            // objeto ESTATICO bumpa transform e static casters, e os dois marcam o mesmo renderavel.
            //
            // A segunda escrita refaria o mesmo trabalho e repetiria o slot na lista de assentamento.
            // Ela ja nao seria destrutiva — o previous sai do shadow publicado, que o PrepareFrame
            // nunca move —, mas o epoch continua sendo o que a limpeza e o assentamento abaixo
            // consultam para saber quem ja foi escrito neste ciclo.
            if (SeenEpoch_[Slot] == Epoch) return Slot;
            const Mat44 Current = R.Transform.Matrix();
            const Mat44 Previous = PublishedCurrent(Slot, Handle.Generation, Current);
            const bool TransformChanged =
                FGpuSceneInstanceTransform{ Current } != FGpuSceneInstanceTransform{ Previous };

            u32 Flags = 0;
            if (R.Visible)        Flags |= GpuSceneInstanceVisible;
            if (R.RaytracingOnly) Flags |= GpuSceneInstanceRaytracingOnly;
            if (R.Mobility == EMobility::Dynamic) Flags |= GpuSceneInstanceDynamic;
            if (TransformChanged) Flags |= GpuSceneInstanceTransformChanged;
            MetaRows_[Slot] = {
                _Meshes.RowOf(R.Mesh), _Materials.RowOf(R.Material), 0u, Flags, Handle.Generation
            };

            FGpuSceneInstanceBounds Bounds{};
            Bounds.Center = { (R.AABBMin.X + R.AABBMax.X) * 0.5f,
                              (R.AABBMin.Y + R.AABBMax.Y) * 0.5f,
                              (R.AABBMin.Z + R.AABBMax.Z) * 0.5f, 0.0f };
            Bounds.Extent = { (R.AABBMax.X - R.AABBMin.X) * 0.5f,
                              (R.AABBMax.Y - R.AABBMin.Y) * 0.5f,
                              (R.AABBMax.Z - R.AABBMin.Z) * 0.5f, 0.0f };
            BoundsRows_[Slot] = Bounds;
            TransformRows_[Slot].Value = Current;
            PrevTransformRows_[Slot].Value = Previous;
            if (TransformChanged)
                NextSettleSlots_.push_back(Slot);

            SeenEpoch_[Slot] = Epoch;
            TouchedSlots_.push_back(Slot);
            return Slot;
        };

        if (FullScan) {
            NextLiveSlots_.clear();
            NextLiveSlots_.reserve(_Scene.Renderables().size());
            for (const FRenderable& R : _Scene.Renderables()) {
                const u32 Slot = WriteRow(R);
                if (Slot != FGpuSceneHandle::kInvalidSlot) NextLiveSlots_.push_back(Slot);
            }

            // Limpa apenas quem estava vivo na imagem anterior. Nao varre a capacidade: remover um
            // objeto deve tocar a linha dele, nao um milhao de slots vazios.
            for (u32 Slot : LiveSlots_) {
                if (Slot >= SeenEpoch_.size() || SeenEpoch_[Slot] == Epoch) continue;
                MetaRows_[Slot] = {};
                BoundsRows_[Slot] = {};
                TransformRows_[Slot] = {};
                PrevTransformRows_[Slot] = {};
                // O slot limpo tambem foi ESCRITO, e a linha zerada precisa chegar a GPU: sem ele
                // no conjunto tocado, um objeto removido continuaria residente na tabela ate
                // alguem reusar o slot.
                TouchedSlots_.push_back(Slot);
            }
            LiveSlots_.swap(NextLiveSlots_);
            LiveRows_ = static_cast<u32>(LiveSlots_.size());
        } else {
            // O conjunto vivo NAO mudou — estrutura igual e a condicao de estar neste ramo —,
            // entao LiveSlots_ e LiveRows_ seguem validos sem ser refeitos.
            const std::vector<FRenderable>& List = _Scene.Renderables();
            for (u32 Index : Hints)
                if (Index < List.size()) WriteRow(List[Index]);
        }

        // Assentamento do previous transform. Um slot reescrito acima ja recebeu o previous certo;
        // os que sobraram so precisam alcancar o current PUBLICADO, e para isso nao ha o que
        // perguntar ao renderavel — nada nele mudou desde o ciclo passado.
        //
        // Enquanto o movimento nao foi aceito, o shadow ainda tem o current antigo e o par continua
        // em movimento: o slot fica na lista, e so assenta no frame seguinte ao da publicacao.
        // Igualar o previous as linhas escritas publicaria na nova tentativa um par parado, e a GPU
        // nunca veria o movimento.
        for (u32 Slot : SettleSlots_) {
            if (Slot >= SeenEpoch_.size() || SeenEpoch_[Slot] == Epoch) continue;
            const Mat44 Current = TransformRows_[Slot].Value;
            const Mat44 Previous = PublishedCurrent(Slot, MetaRows_[Slot].Generation, Current);
            PrevTransformRows_[Slot].Value = Previous;
            if (FGpuSceneInstanceTransform{ Current } != FGpuSceneInstanceTransform{ Previous }) {
                MetaRows_[Slot].Flags |= GpuSceneInstanceTransformChanged;
                NextSettleSlots_.push_back(Slot);
            } else {
                // O fast path temporal depende desta limpeza. Sem ela, um objeto que parou continua
                // pagando inversas por pixel para sempre, embora as duas matrizes ja sejam iguais.
                MetaRows_[Slot].Flags &= ~GpuSceneInstanceTransformChanged;
            }
            SeenEpoch_[Slot] = Epoch;
            TouchedSlots_.push_back(Slot);
        }
        SettleSlots_.swap(NextSettleSlots_);

        // Ordenado e sem repeticao porque o RecordUpload coalesce intervalos contiguos, e slots
        // vem de free list — a ordem de percurso da cena nao e a ordem de slot.
        std::sort(TouchedSlots_.begin(), TouchedSlots_.end());
        TouchedSlots_.erase(std::unique(TouchedSlots_.begin(), TouchedSlots_.end()),
                            TouchedSlots_.end());

        _Scene.ConsumeDirtyRenderableHints();
        PreparedStructureVersion_ = StructureVersion;
        PreparedTransformVersion_ = TransformVersion;
        PreparedStaticCastersVersion_ = StaticCastersVersion;
        NeedsSettle_ = !SettleSlots_.empty();
        Dirty_ = true;
    }

    bool FGpuSceneInstanceTable::PrepareUpload(ID3D12GraphicsCommandList* _CommandList) {
        Prepared_        = false;
        PreparedList_    = nullptr;
        LastDirtyRows_   = 0;
        LastDirtyBytes_  = 0;
        LastCopyRegions_ = 0;
        LastRowsScanned_ = 0;
        if (!Built_ || !FitsScene_) return false;

        MetaChanged_.clear();
        BoundsChanged_.clear();
        TransformChanged_.clear();
        PrevTransformChanged_.clear();
        PreparedAltersLayout_ = false;
        if (Dirty_) {
            // O conjunto tocado e o mesmo para as quatro colunas — quem escreve uma linha escreve as
            // quatro —, entao o custo do diff e |tocados| x 4, e nao a capacidade da tabela x 4.
            const std::span<const u32> Touched{ TouchedSlots_ };
            FindChangedRowsAt<FGpuSceneInstanceMeta>(MetaRows_, Touched, MetaShadow_, MetaChanged_,
                                                     LastRowsScanned_);
            FindChangedRowsAt<FGpuSceneInstanceBounds>(BoundsRows_, Touched, BoundsShadow_,
                                                       BoundsChanged_, LastRowsScanned_);
            FindChangedRowsAt<FGpuSceneInstanceTransform>(TransformRows_, Touched, TransformShadow_,
                                                          TransformChanged_, LastRowsScanned_);
            FindChangedRowsAt<FGpuSceneInstanceTransform>(PrevTransformRows_, Touched,
                                                          PrevTransformShadow_,
                                                          PrevTransformChanged_, LastRowsScanned_);

            // A publicacao e TRANSACIONAL. As quatro copias sao aceitas antes de qualquer uma ser
            // gravada — uma coluna recusada depois de outra gravada deixaria meta e transform de
            // imagens diferentes —, e os shadows e a versao so avancam depois das quatro gravadas.
            // Recusada, nada muda: `Dirty_` e os tocados ficam para o proximo frame tentar de novo.
            if (!MetaStorage_.AcceptsUpload(_CommandList, MetaRows_.data(), MetaChanged_) ||
                !BoundsStorage_.AcceptsUpload(_CommandList, BoundsRows_.data(), BoundsChanged_) ||
                !TransformStorage_.AcceptsUpload(_CommandList, TransformRows_.data(),
                                                 TransformChanged_) ||
                !PrevTransformStorage_.AcceptsUpload(_CommandList, PrevTransformRows_.data(),
                                                     PrevTransformChanged_))
                return false;

            // Os shadows crescem ate a capacidade na primeira publicacao depois de um Setup — em 1M,
            // centenas de MB. A reserva vem AQUI, para o commit depois do BeginFrame nao alocar.
            MetaShadow_.reserve(MetaRows_.size());
            BoundsShadow_.reserve(BoundsRows_.size());
            TransformShadow_.reserve(TransformRows_.size());
            PrevTransformShadow_.reserve(PrevTransformRows_.size());

            // O layout, com o shadow ainda ANTIGO: e o que a GPU tinha. So sobre as linhas de meta que
            // mudaram, entao nao ha varredura nova — e fora do `LastRowsScanned`, que mede o diff.
            for (u32 Slot : MetaChanged_) {
                const FGpuSceneInstanceMeta Antes =
                    Slot < MetaShadow_.size() ? MetaShadow_[Slot] : FGpuSceneInstanceMeta{};
                if (GpuInstanceMetaAltersLayout(Antes, MetaRows_[Slot])) {
                    PreparedAltersLayout_ = true;
                    break;
                }
            }
        }
        Prepared_     = true;
        PreparedList_ = _CommandList;
        return true;
    }

    bool FGpuSceneInstanceTable::RecordUpload(ID3D12GraphicsCommandList* _CommandList,
                                              u32 _FrameSlot) noexcept {
        if (!IsPreparedFor(_CommandList)) return false;
        Prepared_ = false;
        if (!Dirty_) return true;

        auto Upload = [&](FGpuTableStorage& Storage, const auto& Rows,
                          const std::vector<u32>& Changed, u32 RowBytes) {
            if (Changed.empty()) return;
            const u32 Regions = Storage.RecordUpload(_CommandList, _FrameSlot,
                                                     Rows.data(), Changed);
            if (Regions == 0) return;
            LastCopyRegions_ += Regions;
            const u64 Bytes = static_cast<u64>(Changed.size()) * RowBytes;
            LastDirtyBytes_ += Bytes;
            TotalBytesUploaded_ += Bytes;
            for (u32 Slot : Changed) {
                if (!DirtyMarks_[Slot]) { DirtyMarks_[Slot] = 1; ++LastDirtyRows_; }
            }
        };
        Upload(MetaStorage_, MetaRows_, MetaChanged_, kMetaBytes);
        Upload(BoundsStorage_, BoundsRows_, BoundsChanged_, kBoundsBytes);
        Upload(TransformStorage_, TransformRows_, TransformChanged_, kTransformBytes);
        Upload(PrevTransformStorage_, PrevTransformRows_, PrevTransformChanged_, kTransformBytes);

        CommitChangedRows<FGpuSceneInstanceMeta>(MetaRows_, MetaChanged_, MetaShadow_);
        CommitChangedRows<FGpuSceneInstanceBounds>(BoundsRows_, BoundsChanged_, BoundsShadow_);
        CommitChangedRows<FGpuSceneInstanceTransform>(TransformRows_, TransformChanged_,
                                                      TransformShadow_);
        CommitChangedRows<FGpuSceneInstanceTransform>(PrevTransformRows_, PrevTransformChanged_,
                                                      PrevTransformShadow_);
        if (PreparedAltersLayout_) ++LayoutInputsVersion_;
        Dirty_ = false;

        for (u32 Slot : MetaChanged_)          DirtyMarks_[Slot] = 0;
        for (u32 Slot : BoundsChanged_)        DirtyMarks_[Slot] = 0;
        for (u32 Slot : TransformChanged_)     DirtyMarks_[Slot] = 0;
        for (u32 Slot : PrevTransformChanged_) DirtyMarks_[Slot] = 0;
        return true;
    }

    // As duas consultas leem os SHADOWS: a imagem publicada, a mesma que os leitores da GPU tem neste
    // frame. Com um upload aceito as linhas escritas e o shadow sao iguais; so divergem depois de uma
    // recusa, e ai o par das linhas escritas e um que a GPU nunca recebeu. Os quatro shadows avancam
    // juntos no mesmo commit, entao o tamanho da meta vale para as matrizes.
    bool FGpuSceneInstanceTable::TransformsFor(FGpuSceneHandle _Handle, Mat44& _Current,
                                               Mat44& _Previous) const {
        if (_Handle.Generation == 0 || _Handle.Slot >= MetaShadow_.size()) return false;
        if (MetaShadow_[_Handle.Slot].Generation != _Handle.Generation) return false;
        _Current  = TransformShadow_[_Handle.Slot].Value;
        _Previous = PrevTransformShadow_[_Handle.Slot].Value;
        return true;
    }

    bool FGpuSceneInstanceTable::MetaFor(FGpuSceneHandle _Handle,
                                         FGpuSceneInstanceMeta& _Meta) const {
        if (_Handle.Generation == 0 || _Handle.Slot >= MetaShadow_.size()) return false;
        const FGpuSceneInstanceMeta& Row = MetaShadow_[_Handle.Slot];
        if (Row.Generation != _Handle.Generation) return false;
        _Meta = Row;
        return true;
    }

    u64 FGpuSceneInstanceTable::ResidentBytes() const {
        return MetaStorage_.ResidentBytes() + BoundsStorage_.ResidentBytes() +
               TransformStorage_.ResidentBytes() + PrevTransformStorage_.ResidentBytes();
    }

    u64 FGpuSceneInstanceTable::StagingBytes() const {
        return MetaStorage_.StagingBytes() + BoundsStorage_.StagingBytes() +
               TransformStorage_.StagingBytes() + PrevTransformStorage_.StagingBytes();
    }

    // ================================================================================================
    // As tres tabelas da cena
    // ================================================================================================
    void GpuSceneSetupTables(FGpuMaterialTable& _Materials, FGpuMeshTable& _Meshes,
                             FGpuSceneInstanceTable& _Instances, ID3D12Device* _Device,
                             FTextureSRVHeap& _SRVHeap, std::span<FMaterial* const> _MaterialList,
                             const FScene& _Scene) {
        try {
            _Materials.SetupForMaterials(_Device, _SRVHeap, _MaterialList);
            _Meshes.SetupForScene(_Device, _SRVHeap, _Scene);
            _Instances.SetupForScene(_Device, _SRVHeap, _Scene);
        } catch (...) {
            // A que lancou ja saiu liberada. As outras tambem saem: a que completou descreve a cena
            // nova, e a que ainda nao rodou descreve a antiga — nenhuma das duas combina com a falha.
            GpuSceneReleaseTables(_Materials, _Meshes, _Instances, _SRVHeap);
            throw;
        }
    }

    void GpuSceneReleaseTables(FGpuMaterialTable& _Materials, FGpuMeshTable& _Meshes,
                               FGpuSceneInstanceTable& _Instances,
                               FTextureSRVHeap& _SRVHeap) noexcept {
        _Materials.Release(_SRVHeap);
        _Meshes.Release(_SRVHeap);
        _Instances.Release(_SRVHeap);
    }
}
