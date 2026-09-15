#include "Smile/Graphics/Scene/GpuIndirectMainView.h"

#include "Smile/Core/HResultCheck.h"
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace Smile {
    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        // A tabela de descritores de UM frame slot: os quatro passos em sequencia, cada um com as SRVs e
        // depois as UAVs, na ordem dos registradores do shader.
        //
        //   expand:   t0 ranges  t1 prefixos  t2 candidatos  t3 meta                       u0 saidas  u1 contadores
        //   classify: t0 saidas  t1 meta  t2 materiais  t3 malhas                          u0 lotes   u1 contadores
        //   scatter:  t0 saidas  t1-t3 tabelas  t4 regioes  t5 lotes                        u0 args  u1 cursores  u2 contadores
        //   final:    t0 lotes  t1 regioes  t2 cont. scatter  t3 cursores  t4 cont. classify u0 DrawCounts  u1 status
        struct FPasso {
            u32 Srv;
            u32 NumSrv;
            u32 Uav;
            u32 NumUav;
        };
        constexpr FPasso kExpand   { 0u, 4u, 4u, 2u };
        constexpr FPasso kClassify { 6u, 4u, 10u, 2u };
        constexpr FPasso kScatter  { 12u, 6u, 18u, 3u };
        constexpr FPasso kFinalize { 21u, 5u, 26u, 2u };
        constexpr u32 kDescriptorsPerSlot = 28u;
        static_assert(kExpand.Uav == kExpand.Srv + kExpand.NumSrv &&
                      kClassify.Srv == kExpand.Uav + kExpand.NumUav &&
                      kClassify.Uav == kClassify.Srv + kClassify.NumSrv &&
                      kScatter.Srv == kClassify.Uav + kClassify.NumUav &&
                      kScatter.Uav == kScatter.Srv + kScatter.NumSrv &&
                      kFinalize.Srv == kScatter.Uav + kScatter.NumUav &&
                      kFinalize.Uav == kFinalize.Srv + kFinalize.NumSrv &&
                      kDescriptorsPerSlot == kFinalize.Uav + kFinalize.NumUav,
                      "as tabelas dos quatro passos sao contiguas e cobrem o slot inteiro");

        constexpr u32 kCbExpand = 0u, kCbClassify = 1u, kCbScatter = 2u, kCbFinalize = 3u;
        constexpr u32 kCbsPerSlot = 4u;
        // Os dois `uint4` de cada cbuffer.
        constexpr u32 kCbBytes = 32u;

        constexpr u64 kStatusBytes   = 2u * sizeof(u32);
        constexpr u64 kExpandBytes   = GpuExpandCounterCount * sizeof(u32);
        constexpr u64 kClassifyBytes = GpuClassifyOutcomeCount * sizeof(u32);
        constexpr u64 kScatterBytes  = GpuScatterCounterCount * sizeof(u32);

        // O readback de um slot: status e os contadores dos tres passos por candidato.
        constexpr u64 kReadStatus      = 0u;
        constexpr u64 kReadExpand      = kReadStatus + kStatusBytes;
        constexpr u64 kReadClassify    = kReadExpand + kExpandBytes;
        constexpr u64 kReadScatter     = kReadClassify + kClassifyBytes;
        constexpr u64 kReadbackPerSlot = kReadScatter + kScatterBytes;

        // Os estados de repouso que a cadeia deixa SEM barreira final: ela termina com os tres lendo em
        // compute. Se um repouso mudar, a barreira que falta aparece aqui, e nao na camada de debug.
        static_assert(FGpuIndirectMainView::kExpandedRestState ==
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        static_assert(FGpuIndirectMainView::kBatchRestState ==
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    const char* GpuMainViewReason(u32 _Reason) {
        switch (_Reason) {
        case GpuMainViewOk:                 return "cadeia gravada";
        case GpuMainViewNotBuilt:           return "sem Setup completo";
        case GpuMainViewNoCommandList:      return "sem command list";
        case GpuMainViewNotPrepared:        return "sem preparacao valida para a lista";
        case GpuMainViewCandidatesOverflow: return "a lista de candidatos nao coube";
        case GpuMainViewTooManyRanges:      return "ranges demais";
        case GpuMainViewTooManyOutputs:     return "saidas da expansao alem da capacidade";
        case GpuMainViewDispatchTooLarge:   return "saidas alem de um dispatch";
        case GpuMainViewBadPoolViews:       return "view de pool invalida";
        case GpuMainViewUploadRefused:      return "copia recusada pelo staging";
        case GpuMainViewLayoutRefused:      return "layout de regioes recusado";
        case GpuMainViewSmallBatchBuffers:  return "lotes alem dos buffers por lote";
        case GpuMainViewRegionsRefused:     return "tabela de regioes recusada";
        case GpuMainViewClassifyRefused:    return "classificacao recusada";
        case GpuMainViewRecordRefused:      return "gravacao dos draws recusada";
        default:                            return "motivo desconhecido";
        }
    }

    void FGpuIndirectMainView::Initialize(ID3D12Device* _Device) {
        Initialized_ = false;
        ExpandPipeline_.Initialize(_Device, "ExpandInstanceRanges.cs_6_0.cso", kExpand.NumSrv,
                                   kExpand.NumUav);
        ClassifyPipeline_.Initialize(_Device, "ClassifyCandidates.cs_6_0.cso", kClassify.NumSrv,
                                     kClassify.NumUav);
        ScatterPipeline_.Initialize(_Device, "ScatterIndirectArgs.cs_6_0.cso", kScatter.NumSrv,
                                    kScatter.NumUav);
        FinalizePipeline_.Initialize(_Device, "FinalizeDrawCounts.cs_6_0.cso", kFinalize.NumSrv,
                                     kFinalize.NumUav);
        Device_      = _Device;
        Initialized_ = true;
    }

    void FGpuIndirectMainView::Setup(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _SlotCapacity, u32 _PoolCount) {
        // FAIL-CLOSED, como os Setup das tabelas: pronto so na ultima linha, e quem lanca sai LIBERADO. O
        // anterior sai ANTES de o novo ser criado — o pico e so o novo, como no FGpuTableStorage::Create.
        Release(_SRVHeap);
        try {
            if (!Initialized_ || Device_.Get() != _Device)
                throw std::logic_error("FGpuIndirectMainView::Setup sem Initialize neste device");

            const u32 Slots = std::max(_SlotCapacity, 1u);
            // Sem pool, ou pools demais para a tabela de lotes, nao ha lote: os buffers por lote nascem com
            // uma linha (recurso de zero bytes e ilegal), e a recontagem do layout recusa o frame.
            u32 Lotes = 0;
            if (GpuClassifyBatchBufferSlots(_PoolCount, Lotes) != GpuClassifyPreflightOk) Lotes = 0;
            const u32 LinhasDeLote = std::max(Lotes, 1u);

            CandidateStorage_.Create(_Device, _SRVHeap, sizeof(FGpuCandidateId), Slots,
                                     "GpuMainViewCandidates");
            RangeStorage_.Create(_Device, _SRVHeap, sizeof(FInstanceRange), kRangeCapacity,
                                 "GpuMainViewRanges");
            BaseStorage_.Create(_Device, _SRVHeap, sizeof(u32), kRangeCapacity, "GpuMainViewRangeBases");
            RegionStorage_.Create(_Device, _SRVHeap, sizeof(FGpuIndirectRegion), LinhasDeLote,
                                  "GpuMainViewRegions");

            // Cada um NASCE no seu estado de repouso: a primeira barreira da primeira cadeia parte dele.
            auto Uav = [&](u64 _Bytes, D3D12_RESOURCE_STATES _Repouso, const char* _Rotulo) {
                return GpuResources::CreateBuffer(_Device, _Bytes,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, _Repouso,
                                                  EVramCategory::Misc, _Rotulo);
            };
            const u64 BytesDeLote = static_cast<u64>(LinhasDeLote) * sizeof(u32);
            ExpandedSlots_    = Uav(static_cast<u64>(Slots) * sizeof(u32), kExpandedRestState,
                                    "GpuMainViewExpandedSlots");
            ExpandCounters_   = Uav(kExpandBytes, kCountersRestState, "GpuMainViewExpandCounters");
            BatchCounts_      = Uav(BytesDeLote, kBatchRestState, "GpuMainViewBatchCounts");
            ClassifyCounters_ = Uav(kClassifyBytes, kCountersRestState, "GpuMainViewClassifyCounters");
            IndirectArgs_     = Uav(GpuScatterArgsBytes(Slots), kArgsRestState, "GpuMainViewIndirectArgs");
            BatchCursors_     = Uav(BytesDeLote, kBatchRestState, "GpuMainViewBatchCursors");
            ScatterCounters_  = Uav(kScatterBytes, kCountersRestState, "GpuMainViewScatterCounters");
            DrawCounts_       = Uav(BytesDeLote, kDrawCountsRestState, "GpuMainViewDrawCounts");
            Status_           = Uav(kStatusBytes, kCountersRestState, "GpuMainViewStatus");

            // Zeros do tamanho do maior acumulador: o reset copia daqui. Conteudo imutavel, entao um so
            // serve os dois frame slots.
            const u64 BytesDeZero = std::max(BytesDeLote, kClassifyBytes);
            Zeros_ = GpuResources::CreateUploadBuffer(_Device, BytesDeZero, 1, /*ForConstantBuffer=*/false);
            std::memset(Zeros_.Mapped, 0, static_cast<size_t>(BytesDeZero));

            ConstantBuffers_ = GpuResources::CreateUploadBuffer(_Device, kCbBytes,
                                                                kCbsPerSlot * kFrameSlots, true);
            std::memset(ConstantBuffers_.Mapped, 0,
                        static_cast<size_t>(ConstantBuffers_.SliceBytes * ConstantBuffers_.SliceCount));

            Readback_ = GpuResources::CreateReadbackBuffer(_Device, kReadbackPerSlot * kFrameSlots);
            u8* Mapeado = nullptr;
            SMILE_HR(Readback_->Map(0, nullptr, reinterpret_cast<void**>(&Mapeado)));
            ReadbackMapped_ = Mapeado;

            // As views dos buffers da cadeia, escritas UMA vez por slot. As das tabelas canonicas e dos
            // buffers que sobem sao copiadas a cada gravacao (ver `Record`).
            auto Srv = [&](ID3D12Resource* _R, u32 _Elementos, u32 _Stride, u32 _Indice) {
                const D3D12_SHADER_RESOURCE_VIEW_DESC D =
                    GpuResources::SrvStructuredBuffer(_Elementos, _Stride);
                _Device->CreateShaderResourceView(_R, &D, _SRVHeap.CpuHandle(_Indice));
            };
            auto UavView = [&](ID3D12Resource* _R, u32 _Elementos, u32 _Stride, u32 _Indice) {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC D =
                    GpuResources::UavStructuredBuffer(_Elementos, _Stride);
                _Device->CreateUnorderedAccessView(_R, nullptr, &D, _SRVHeap.CpuHandle(_Indice));
            };
            const u32 Dword = sizeof(u32);
            for (u32& Base : DescriptorBase_) {
                Base = _SRVHeap.Allocate(kDescriptorsPerSlot);
                UavView(ExpandedSlots_.Get(), Slots, Dword, Base + kExpand.Uav + 0u);
                UavView(ExpandCounters_.Get(), GpuExpandCounterCount, Dword, Base + kExpand.Uav + 1u);

                Srv(ExpandedSlots_.Get(), Slots, Dword, Base + kClassify.Srv + 0u);
                UavView(BatchCounts_.Get(), LinhasDeLote, Dword, Base + kClassify.Uav + 0u);
                UavView(ClassifyCounters_.Get(), GpuClassifyOutcomeCount, Dword, Base + kClassify.Uav + 1u);

                Srv(ExpandedSlots_.Get(), Slots, Dword, Base + kScatter.Srv + 0u);
                Srv(BatchCounts_.Get(), LinhasDeLote, Dword, Base + kScatter.Srv + 5u);
                UavView(IndirectArgs_.Get(), Slots, sizeof(FGpuIndirectDrawArgs), Base + kScatter.Uav + 0u);
                UavView(BatchCursors_.Get(), LinhasDeLote, Dword, Base + kScatter.Uav + 1u);
                UavView(ScatterCounters_.Get(), GpuScatterCounterCount, Dword, Base + kScatter.Uav + 2u);

                Srv(BatchCounts_.Get(), LinhasDeLote, Dword, Base + kFinalize.Srv + 0u);
                Srv(ScatterCounters_.Get(), GpuScatterCounterCount, Dword, Base + kFinalize.Srv + 2u);
                Srv(BatchCursors_.Get(), LinhasDeLote, Dword, Base + kFinalize.Srv + 3u);
                Srv(ClassifyCounters_.Get(), GpuClassifyOutcomeCount, Dword, Base + kFinalize.Srv + 4u);
                UavView(DrawCounts_.Get(), LinhasDeLote, Dword, Base + kFinalize.Uav + 0u);
                UavView(Status_.Get(), 2u, Dword, Base + kFinalize.Uav + 1u);
            }

            AllRegionRows_.resize(LinhasDeLote);
            std::iota(AllRegionRows_.begin(), AllRegionRows_.end(), 0u);

            // A lista e montada sob o teto dos buffers; mudar o teto a invalida.
            Candidates_.SetCapacity(Slots);
            LayoutState_.Reserve(_PoolCount);

            SlotCapacity_  = Slots;
            BatchCapacity_ = Lotes;
            BatchRows_     = LinhasDeLote;
        } catch (...) {
            Release(_SRVHeap);
            throw;
        }

        // O conteudo dos buffers novos e indefinido: a primeira preparacao sobe a lista inteira e a tabela
        // de regioes da primeira versao.
        CandidatesPending_      = true;
        UploadedRegionsVersion_ = 0;
        Built_                  = true;
    }

    void FGpuIndirectMainView::Release(FTextureSRVHeap& _SRVHeap) noexcept {
        Built_        = false;
        Prepared_     = false;
        PreparedList_ = nullptr;
        for (u32& Base : DescriptorBase_) _SRVHeap.Release(Base, kDescriptorsPerSlot);
        CandidateStorage_.Release(_SRVHeap);
        RangeStorage_.Release(_SRVHeap);
        BaseStorage_.Release(_SRVHeap);
        RegionStorage_.Release(_SRVHeap);
        ExpandedSlots_.Reset();
        ExpandCounters_.Reset();
        BatchCounts_.Reset();
        ClassifyCounters_.Reset();
        IndirectArgs_.Reset();
        BatchCursors_.Reset();
        ScatterCounters_.Reset();
        DrawCounts_.Reset();
        Status_.Reset();
        Zeros_           = {};
        ConstantBuffers_ = {};
        Readback_.Reset();
        ReadbackMapped_ = nullptr;
        ReadbackPending_.fill(false);
        LastGpuCounters_ = {};
        // O teto sai junto: a lista da cena descrevia buffers que nao existem mais. A fonte de versoes do
        // layout NAO: ela vive pela vida do objeto, para nenhuma tabela antiga coincidir com um layout novo.
        Candidates_.Reset();
        Candidates_.SetCapacity(0u);
        PoolViews_.clear();
        BaseRows_.clear();
        CandidateShadow_.clear();
        RangeShadow_.clear();
        BaseShadow_.clear();
        CandidateChanged_.clear();
        RangeChanged_.clear();
        BaseChanged_.clear();
        AllRegionRows_.clear();
        SlotCapacity_           = 0;
        BatchCapacity_          = 0;
        BatchRows_              = 0;
        UploadedRegionsVersion_ = 0;
        PreparedOutputs_        = 0;
        PreparedPools_          = 0;
        CandidatesPending_      = false;
    }

    u32 FGpuIndirectMainView::Refuse(u32 _Reason, u32 _Detail) noexcept {
        if (_Reason < GpuMainViewReasonCount) ++Stats_.Refusals[_Reason];
        Stats_.LastReason = _Reason;
        Stats_.LastDetail = _Detail;
        return _Reason;
    }

    u32 FGpuIndirectMainView::Prepare(const FScene& _Scene, const FGpuMeshTable& _Meshes,
                                      ID3D12GraphicsCommandList* _CommandList) {
        Prepared_     = false;
        PreparedList_ = nullptr;
        if (!Built_) return Refuse(GpuMainViewNotBuilt);
        if (!_CommandList) return Refuse(GpuMainViewNoCommandList);

        // A lista so e remontada quando a MEMBRESIA pode ter mudado. Remontada, sobe — e continua
        // pendente ate uma cadeia gravar a copia.
        if (Candidates_.RefreshIfStale(_Scene)) CandidatesPending_ = true;
        if (!Candidates_.IsUsable()) return Refuse(GpuMainViewCandidatesOverflow);

        const std::span<const FInstanceRange> Ranges = Candidates_.Ranges();
        if (Ranges.size() > kRangeCapacity) return Refuse(GpuMainViewTooManyRanges);
        const u32 Saidas = BuildRangeOutputBases(Ranges, BaseRows_);
        if (Saidas > SlotCapacity_) return Refuse(GpuMainViewTooManyOutputs);
        if (Saidas > kMaxOutputs) return Refuse(GpuMainViewDispatchTooLarge);

        // Refeitas a cada frame, a partir dos RECURSOS: e barato (unidades de pools), e um recurso novo no
        // mesmo endereco nao reaproveita a view do antigo.
        const std::span<const FGpuGeometryPool> Pools = _Meshes.GeometryPools();
        if (const u32 Motivo = GpuPoolViewsFor(Pools, PoolViews_); Motivo != GpuPoolViewsOk)
            return Refuse(GpuMainViewBadPoolViews, Motivo);

        // Os diffs dos tres buffers que sobem com a lista, sem avancar shadow nenhum — o commit e da
        // gravacao, depois da copia gravada. A reserva e daqui, para o commit nao alocar.
        const std::span<const FGpuCandidateId> Ids = Candidates_.InstanceIds();
        if (CandidatesPending_) {
            FindChangedRows<FGpuCandidateId>(Ids, CandidateShadow_, CandidateChanged_);
            FindChangedRows<FInstanceRange>(Ranges, RangeShadow_, RangeChanged_);
            FindChangedRows<u32>(BaseRows_, BaseShadow_, BaseChanged_);
            CandidateShadow_.reserve(Ids.size());
            RangeShadow_.reserve(Ranges.size());
            BaseShadow_.reserve(BaseRows_.size());
        } else {
            CandidateChanged_.clear();
            RangeChanged_.clear();
            BaseChanged_.clear();
        }
        if (!CandidateStorage_.AcceptsUpload(_CommandList, Ids.data(), CandidateChanged_) ||
            !RangeStorage_.AcceptsUpload(_CommandList, Ranges.data(), RangeChanged_) ||
            !BaseStorage_.AcceptsUpload(_CommandList, BaseRows_.data(), BaseChanged_))
            return Refuse(GpuMainViewUploadRefused);

        const u32 NumPools = static_cast<u32>(Pools.size());
        LayoutState_.Reserve(NumPools);

        PreparedOutputs_ = Saidas;
        PreparedPools_   = NumPools;
        PreparedList_    = _CommandList;
        Prepared_        = true;
        return GpuMainViewOk;
    }

    void FGpuIndirectMainView::CollectGpuCounters(u32 _Slot) noexcept {
        if (!ReadbackPending_[_Slot] || !ReadbackMapped_) return;
        const u8* P = ReadbackMapped_ + static_cast<size_t>(_Slot) * kReadbackPerSlot;
        FGpuMainViewGpuCounters C;
        C.Valid = true;
        std::memcpy(&C.Status, P + kReadStatus, sizeof(u32));
        std::memcpy(&C.Reason, P + kReadStatus + sizeof(u32), sizeof(u32));
        std::memcpy(C.Expand.data(), P + kReadExpand, kExpandBytes);
        std::memcpy(C.Classify.data(), P + kReadClassify, kClassifyBytes);
        std::memcpy(C.Scatter.data(), P + kReadScatter, kScatterBytes);
        LastGpuCounters_         = C;
        ReadbackPending_[_Slot] = false;
    }

    u32 FGpuIndirectMainView::Record(ID3D12GraphicsCommandList* _CommandList, u32 _FrameSlot,
                                     FTextureSRVHeap& _SRVHeap, const FGpuMaterialTable& _Materials,
                                     const FGpuMeshTable& _Meshes,
                                     const FGpuSceneInstanceTable& _Instances,
                                     std::span<const FGpuIndirectPassSignature> _Passes) noexcept {
        const u32 Slot = _FrameSlot % kFrameSlots;
        // O uso anterior deste slot ja executou — o BeginFrame esperou a fence dele —, entao o readback
        // que ele gravou esta completo. Telemetria: nada abaixo o consulta.
        CollectGpuCounters(Slot);

        if (!Built_) return Refuse(GpuMainViewNotBuilt);
        const bool Preparada = Prepared_ && _CommandList != nullptr && PreparedList_ == _CommandList &&
                               _Meshes.GeometryPools().size() == PreparedPools_;
        Prepared_ = false;
        if (!Preparada) return Refuse(GpuMainViewNotPrepared);

        // ---- PORTAS: todas antes do primeiro comando da cadeia ----

        // O layout, sobre as linhas que a gravacao da publicacao acabou de avancar: as mesmas que o
        // classify deste frame le. A reserva veio da preparacao; sem ela, recusa em vez de alocar.
        const u32 Pools = PreparedPools_;
        const FGpuLayoutInputs Entradas{
            _Instances.PublishedMeta(), _Materials.PublishedRows(), _Meshes.PublishedRows(), Pools,
            _Instances.LayoutInputsVersion(), _Materials.LayoutInputsVersion(),
            _Meshes.LayoutInputsVersion()
        };
        if (const u32 M = LayoutState_.Refresh(Entradas); M != GpuLayoutRefreshOk)
            return Refuse(GpuMainViewLayoutRefused, M);
        const FGpuIndirectLayout&                 Layout  = LayoutState_.Layout();
        const std::span<const FGpuIndirectRegion> Regioes = Layout.Regions();
        const u32                                 Lotes   = static_cast<u32>(Regioes.size());

        if (Lotes > BatchCapacity_) return Refuse(GpuMainViewSmallBatchBuffers);
        // `TotalCommands <= capacidade ALOCADA`: a capacidade que a porta recebe e a do recurso, e nao a do
        // layout. E a mesma que o scatter recebe no cbuffer.
        if (const u32 M = GpuRegionPreflight(Regioes, Layout.Version(), Lotes, SlotCapacity_);
            M != GpuRegionPreflightOk)
            return Refuse(GpuMainViewRegionsRefused, M);
        if (const u32 M = GpuClassifyPreflightFor(PreparedOutputs_, Entradas.Meta, Entradas.Materials,
                                                  Entradas.Meshes, Pools, BatchCapacity_);
            M != GpuClassifyPreflightOk)
            return Refuse(GpuMainViewClassifyRefused, M);
        for (const FGpuIndirectPassSignature& Passe : _Passes) {
            const FGpuIndirectDrawTargets Alvos{ Passe.Signature, IndirectArgs_.Get(), DrawCounts_.Get() };
            if (const u32 M = GpuIndirectRecordPreflight(Layout, Passe.PipelineClass, PoolViews_, Alvos);
                M != GpuIndirectRecordOk)
                return Refuse(GpuMainViewRecordRefused, M);
        }
        const u32 MetaSrv = _Instances.MetaSrv(), MaterialSrv = _Materials.Srv(), MeshSrv = _Meshes.Srv();
        if (MetaSrv == kInvalidSlot || MaterialSrv == kInvalidSlot || MeshSrv == kInvalidSlot)
            return Refuse(GpuMainViewNotBuilt);
        // A tabela de regioes sobe inteira quando a versao do layout muda. O DEFAULT e um so: a copia
        // gravada neste frame executa antes do scatter dele, e antes da cadeia do frame seguinte.
        const bool                 SobemRegioes = Layout.Version() != UploadedRegionsVersion_;
        const std::span<const u32> LinhasDeRegiao(AllRegionRows_.data(), Lotes);
        if (SobemRegioes && !RegionStorage_.AcceptsUpload(_CommandList, Regioes.data(), LinhasDeRegiao))
            return Refuse(GpuMainViewUploadRefused);

        // ---- daqui em diante nada recusa, nada aloca e nada lanca ----

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        _CommandList->SetDescriptorHeaps(1, Heaps);

        // Os uploads. O shadow so anda depois da copia gravada; uma cadeia recusada deixa tudo pendente.
        const std::span<const FGpuCandidateId> Ids    = Candidates_.InstanceIds();
        const std::span<const FInstanceRange>  Ranges = Candidates_.Ranges();
        Stats_.CandidateRowsUploaded = 0;
        if (CandidatesPending_) {
            CandidateStorage_.RecordUpload(_CommandList, Slot, Ids.data(), CandidateChanged_);
            RangeStorage_.RecordUpload(_CommandList, Slot, Ranges.data(), RangeChanged_);
            BaseStorage_.RecordUpload(_CommandList, Slot, BaseRows_.data(), BaseChanged_);
            CommitChangedRows<FGpuCandidateId>(Ids, CandidateChanged_, CandidateShadow_);
            CommitChangedRows<FInstanceRange>(Ranges, RangeChanged_, RangeShadow_);
            CommitChangedRows<u32>(BaseRows_, BaseChanged_, BaseShadow_);
            Stats_.CandidateRowsUploaded = static_cast<u32>(CandidateChanged_.size());
            CandidatesPending_           = false;
        }
        if (SobemRegioes) {
            RegionStorage_.RecordUpload(_CommandList, Slot, Regioes.data(), LinhasDeRegiao);
            UploadedRegionsVersion_ = Layout.Version();
            ++Stats_.RegionUploads;
        }

        // Os cbuffers DESTE slot, escritos do zero. A versao do scatter sai do MESMO layout cujas regioes
        // subiram e cujos offsets os passes vao gravar.
        const u32 Saidas       = PreparedOutputs_;
        const u32 LinhasMeta   = static_cast<u32>(Entradas.Meta.size());
        const u32 LinhasMat    = static_cast<u32>(Entradas.Materials.size());
        const u32 LinhasMalha  = static_cast<u32>(Entradas.Meshes.size());
        const u32 ExpandCb[8]   { Saidas, static_cast<u32>(Ranges.size()), SlotCapacity_, LinhasMeta,
                                  static_cast<u32>(Ids.size()), 0u, 0u, 0u };
        const u32 ClassifyCb[8] { Saidas, LinhasMeta, LinhasMat, LinhasMalha, Pools, BatchCapacity_, 0u, 0u };
        const u32 ScatterCb[8]  { Saidas, LinhasMeta, LinhasMat, LinhasMalha, Pools, SlotCapacity_,
                                  Layout.Version(), 0u };
        const u32 FinalizeCb[8] { Lotes, Saidas, 0u, 0u, 0u, 0u, 0u, 0u };
        const u32 CbBase = Slot * kCbsPerSlot;
        std::memcpy(ConstantBuffers_.Slice(CbBase + kCbExpand), ExpandCb, kCbBytes);
        std::memcpy(ConstantBuffers_.Slice(CbBase + kCbClassify), ClassifyCb, kCbBytes);
        std::memcpy(ConstantBuffers_.Slice(CbBase + kCbScatter), ScatterCb, kCbBytes);
        std::memcpy(ConstantBuffers_.Slice(CbBase + kCbFinalize), FinalizeCb, kCbBytes);

        // As views que moram em slots de OUTROS donos, copiadas para a tabela deste frame slot: o
        // `FGpuTableStorage::Create` reescreve a origem quando o recurso muda, e a copia do slot so e
        // reescrita depois da espera dele.
        const u32 Base = DescriptorBase_[Slot];
        auto Copiar = [&](u32 _Destino, u32 _Origem) {
            Device_->CopyDescriptorsSimple(1, _SRVHeap.CpuHandle(Base + _Destino),
                                           _SRVHeap.CpuHandleStaging(_Origem),
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };
        Copiar(kExpand.Srv + 0u, RangeStorage_.Srv());
        Copiar(kExpand.Srv + 1u, BaseStorage_.Srv());
        Copiar(kExpand.Srv + 2u, CandidateStorage_.Srv());
        Copiar(kExpand.Srv + 3u, MetaSrv);
        Copiar(kClassify.Srv + 1u, MetaSrv);
        Copiar(kClassify.Srv + 2u, MaterialSrv);
        Copiar(kClassify.Srv + 3u, MeshSrv);
        Copiar(kScatter.Srv + 1u, MetaSrv);
        Copiar(kScatter.Srv + 2u, MaterialSrv);
        Copiar(kScatter.Srv + 3u, MeshSrv);
        Copiar(kScatter.Srv + 4u, RegionStorage_.Srv());
        Copiar(kFinalize.Srv + 1u, RegionStorage_.Srv());

        auto Amarrar = [&](const FComputePipeline& _Pipeline, u32 _Cb, const FPasso& _Passo) {
            _Pipeline.Bind(_CommandList);
            _CommandList->SetComputeRootConstantBufferView(0, ConstantBuffers_.Address(CbBase + _Cb));
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(Base + _Passo.Srv));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(Base + _Passo.Uav));
        };
        const auto SRV  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto UAV  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto DEST = D3D12_RESOURCE_STATE_COPY_DEST;
        const UINT Grupos = (Saidas + kThreadGroupSize - 1u) / kThreadGroupSize;
        const u64  BytesDeLote = static_cast<u64>(BatchRows_) * sizeof(u32);

        // 0. RESET: os CINCO acumuladores — os contadores do expand tambem, agora que ele esta na cadeia.
        // Um reset so, um dono so; sem ele, um frame vazio depois de um cheio publicaria os contadores do
        // cheio.
        {
            FBarrierBatch B;
            B.Transition(ExpandCounters_.Get(), kCountersRestState, DEST);
            B.Transition(ClassifyCounters_.Get(), kCountersRestState, DEST);
            B.Transition(ScatterCounters_.Get(), kCountersRestState, DEST);
            B.Transition(BatchCounts_.Get(), kBatchRestState, DEST);
            B.Transition(BatchCursors_.Get(), kBatchRestState, DEST);
            B.Flush(_CommandList);
        }
        _CommandList->CopyBufferRegion(ExpandCounters_.Get(), 0, Zeros_.Get(), 0, kExpandBytes);
        _CommandList->CopyBufferRegion(ClassifyCounters_.Get(), 0, Zeros_.Get(), 0, kClassifyBytes);
        _CommandList->CopyBufferRegion(ScatterCounters_.Get(), 0, Zeros_.Get(), 0, kScatterBytes);
        _CommandList->CopyBufferRegion(BatchCounts_.Get(), 0, Zeros_.Get(), 0, BytesDeLote);
        _CommandList->CopyBufferRegion(BatchCursors_.Get(), 0, Zeros_.Get(), 0, BytesDeLote);

        // 1. expand. Os tres passos por candidato pulam o dispatch sem candidato, mas NAO as barreiras: o
        // frame vazio termina nos mesmos estados de um cheio.
        {
            FBarrierBatch B;
            B.Transition(ExpandedSlots_.Get(), kExpandedRestState, UAV);
            B.Transition(ExpandCounters_.Get(), DEST, UAV);
            B.Flush(_CommandList);
        }
        Amarrar(ExpandPipeline_, kCbExpand, kExpand);
        if (Grupos > 0) _CommandList->Dispatch(Grupos, 1, 1);

        // 2. classify. A saida do expand vira leitura: a barreira e o ponto de sincronizacao da secao 6.4.
        {
            FBarrierBatch B;
            B.Transition(ExpandedSlots_.Get(), UAV, SRV);
            B.Transition(BatchCounts_.Get(), DEST, UAV);
            B.Transition(ClassifyCounters_.Get(), DEST, UAV);
            B.Flush(_CommandList);
        }
        Amarrar(ClassifyPipeline_, kCbClassify, kClassify);
        if (Grupos > 0) _CommandList->Dispatch(Grupos, 1, 1);

        // 3. scatter
        {
            FBarrierBatch B;
            B.Transition(BatchCounts_.Get(), UAV, SRV);
            B.Transition(IndirectArgs_.Get(), kArgsRestState, UAV);
            B.Transition(BatchCursors_.Get(), DEST, UAV);
            B.Transition(ScatterCounters_.Get(), DEST, UAV);
            B.Flush(_CommandList);
        }
        Amarrar(ScatterPipeline_, kCbScatter, kScatter);
        if (Grupos > 0) _CommandList->Dispatch(Grupos, 1, 1);

        // 4. finalizacao, SEMPRE: e ela que escreve TODO o `DrawCounts`. Pulada no frame vazio, o count
        // buffer do frame cheio anterior continuaria no ar.
        {
            FBarrierBatch B;
            B.Transition(ScatterCounters_.Get(), UAV, SRV);
            B.Transition(BatchCursors_.Get(), UAV, SRV);
            B.Transition(ClassifyCounters_.Get(), UAV, SRV);
            B.Transition(DrawCounts_.Get(), kDrawCountsRestState, UAV);
            B.Transition(Status_.Get(), kCountersRestState, UAV);
            B.Flush(_CommandList);
        }
        Amarrar(FinalizePipeline_, kCbFinalize, kFinalize);
        _CommandList->Dispatch(1, 1, 1);

        // 5. De volta ao repouso, e a telemetria no readback deste slot. `ExpandedSlots`, `BatchCounts` e
        // `BatchCursors` ja terminaram lendo em compute, que e o repouso deles.
        {
            FBarrierBatch B;
            B.Transition(IndirectArgs_.Get(), UAV, kArgsRestState);
            B.Transition(DrawCounts_.Get(), UAV, kDrawCountsRestState);
            B.Transition(Status_.Get(), UAV, kCountersRestState);
            B.Transition(ExpandCounters_.Get(), UAV, kCountersRestState);
            B.Transition(ClassifyCounters_.Get(), SRV, kCountersRestState);
            B.Transition(ScatterCounters_.Get(), SRV, kCountersRestState);
            B.Flush(_CommandList);
        }
        const u64 Leitura = static_cast<u64>(Slot) * kReadbackPerSlot;
        _CommandList->CopyBufferRegion(Readback_.Get(), Leitura + kReadStatus, Status_.Get(), 0, kStatusBytes);
        _CommandList->CopyBufferRegion(Readback_.Get(), Leitura + kReadExpand, ExpandCounters_.Get(), 0,
                                       kExpandBytes);
        _CommandList->CopyBufferRegion(Readback_.Get(), Leitura + kReadClassify, ClassifyCounters_.Get(), 0,
                                       kClassifyBytes);
        _CommandList->CopyBufferRegion(Readback_.Get(), Leitura + kReadScatter, ScatterCounters_.Get(), 0,
                                       kScatterBytes);
        ReadbackPending_[Slot] = true;

        ++Stats_.ChainsRecorded;
        Stats_.LastReason        = GpuMainViewOk;
        Stats_.LastDetail        = 0;
        Stats_.Outputs           = Saidas;
        Stats_.OutputsHighWater  = std::max(Stats_.OutputsHighWater, Saidas);
        Stats_.Commands          = Layout.TotalCommands();
        Stats_.CommandsHighWater = std::max(Stats_.CommandsHighWater, Layout.TotalCommands());
        return GpuMainViewOk;
    }

    u64 FGpuIndirectMainView::ResidentBytes() const {
        if (!Built_) return 0;
        const u64 Slots = SlotCapacity_;
        const u64 Lote  = BatchRows_;
        return CandidateStorage_.ResidentBytes() + RangeStorage_.ResidentBytes() +
               BaseStorage_.ResidentBytes() + RegionStorage_.ResidentBytes() +
               Slots * sizeof(u32) + GpuScatterArgsBytes(SlotCapacity_) + 3u * Lote * sizeof(u32) +
               kExpandBytes + kClassifyBytes + kScatterBytes + kStatusBytes;
    }

    u64 FGpuIndirectMainView::StagingBytes() const {
        return CandidateStorage_.StagingBytes() + RangeStorage_.StagingBytes() +
               BaseStorage_.StagingBytes() + RegionStorage_.StagingBytes();
    }

    u64 FGpuIndirectMainView::UploadBytes() const {
        return Zeros_.SliceBytes * Zeros_.SliceCount +
               ConstantBuffers_.SliceBytes * ConstantBuffers_.SliceCount;
    }

    u64 FGpuIndirectMainView::ReadbackBytes() const {
        return Readback_ ? kReadbackPerSlot * kFrameSlots : 0u;
    }

    u32 FGpuIndirectMainView::DescriptorCount() const {
        // Os quatro SRVs dos buffers que sobem, e a tabela de cada frame slot.
        return Built_ ? 4u + kDescriptorsPerSlot * kFrameSlots : 0u;
    }
}
