#pragma once

// O main view INDIRETO (Fase 5 do GPU-DRIVEN-PLAN): o dono dos buffers da cadeia, e quem a grava.
//
//     candidatos -> expand -> classify -> scatter -> finalizacao -> ExecuteIndirect dos passes
//
// Os quatro shaders, os contratos de regiao, versao, reset e contabilidade e a gravacao dos
// `ExecuteIndirect` ja existem (GpuSceneCandidates.h, GpuClassify.h, GpuIndirectRecord.h). Aqui eles
// ganham um dono de producao: UM conjunto de buffers DEFAULT, staging, cbuffers, tabelas de descritores e
// readbacks por frame slot, e a gravacao da cadeia inteira na lista da fila direta.
//
// O z-prepass opaco e o G-buffer consomem a saida. Masked, two-sided, blend e linhas de malha
// bloqueadas continuam no fallback CPU; os dois bracos usam `GpuIndirectEligible` sobre a mesma
// imagem publicada das tabelas.
//
// ==============================================================================================
// DUAS FASES, como a publicacao das tabelas
// ==============================================================================================
//
// `Prepare` roda ANTES do BeginFrame, depois de `GpuScenePreparePublication` aceitar o frame: tudo o
// que aloca ou pode lancar — a lista de candidatos, os diffs do upload, as views dos pools e a reserva do
// layout. `Record` roda DEPOIS de `GpuSceneRecordPublication`, com a lista aberta, e nao aloca nem lanca:
// reconta o layout sobre as linhas que acabaram de ser publicadas, confere TODAS as portas e so entao
// grava. Uma excecao ali deixaria a lista aberta com parte da cadeia gravada — e com o shadow de algum
// upload a frente de uma copia que a GPU nunca executa.
//
// RECUSA NAO E FALHA DE FRAME. Qualquer porta reprovada — em qualquer das duas fases — tira o frame
// inteiro do caminho indireto ANTES de qualquer comando da cadeia ser gravado, e o frame segue pelo
// caminho classico (decisao D9). Nada e desfeito, porque nada foi gravado: os uploads recusados ficam
// pendentes para o frame seguinte. Nenhuma recusa depende do que a GPU escreveu (regra 7).
//
// ==============================================================================================
// ESTADOS DE REPOUSO, e nao estado rastreado entre frames
// ==============================================================================================
//
// Cada recurso tem UM estado entre duas cadeias, e toda cadeia gravada parte dele e termina nele. Com
// isso a reutilizacao entre frames nao depende de um rastreador acertar o que a lista anterior deixou:
// o `StateBefore` da primeira barreira e sempre o repouso, e os recursos nascem nele. Uma cadeia recusada
// nao grava barreira nenhuma, e o repouso continua valendo.
//
// Os `ExecuteIndirect` dos passes leem `IndirectArgs` e `DrawCounts` no repouso deles, sem barreira.
//
// ⚠️ Nenhum repouso e UAV. O D3D12 cria buffer efetivamente em COMMON e ignora o InitialState (a camada de
// debug avisa), e de COMMON um buffer so promove implicitamente para estados de LEITURA e de copia. Os tres
// repousos daqui — leitura em compute, origem de copia, argumento indireto — sao alcancaveis assim; um
// repouso em UAV faria a primeira barreira de todo recurso partir de um estado que ele nunca teve.
//
// ==============================================================================================
// POR FRAME SLOT, e por que
// ==============================================================================================
//
//   - staging dos quatro buffers que sobem (candidatos, ranges, prefixos, regioes): a CPU escreve o slice
//     deste frame enquanto a GPU ainda pode estar copiando o do anterior;
//   - cbuffers: o frame anterior, ainda em voo, le os seus;
//   - tabelas de descritores: reescrever um descritor que uma lista ja gravada referencia e comportamento
//     indefinido. As views dos buffers da cadeia sao escritas no Setup; as das tabelas canonicas e dos
//     buffers que sobem sao COPIADAS a cada gravacao, porque o `FGpuTableStorage::Create` reescreve o slot
//     de origem quando o recurso muda;
//   - readbacks dos contadores e do status: TELEMETRIA, lida no uso seguinte do mesmo slot, depois da
//     espera do BeginFrame. Nunca decide draw.
//
// Os buffers DEFAULT sao um conjunto so: a fila direta executa as listas na ordem gravada, entao a cadeia
// do frame seguinte so escreve depois de os draws do anterior lerem.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Scene/GpuClassify.h"
#include "Smile/Graphics/Scene/GpuIndirectLayoutState.h"
#include "Smile/Graphics/Scene/GpuIndirectRecord.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include <array>
#include <span>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FScene;
    class FTextureSRVHeap;

    // Um passe que vai gravar `ExecuteIndirect` com a saida da cadeia: a command signature dele, criada
    // contra a root signature do passe, e a classe de pipeline que ele desenha. A porta da GRAVACAO dos
    // draws (`GpuIndirectRecordPreflight`) roda para cada um ANTES da cadeia: recusada no passe, a cadeia
    // nao roda, porque um dispatch ja gravado nao se desfaz.
    struct FGpuIndirectPassSignature {
        ID3D12CommandSignature* Signature     = nullptr;
        u32                     PipelineClass = GpuPipelineClassOpaqueOneSided;
    };

    // Por que a cadeia do frame NAO roda. Toda recusa manda o frame inteiro para o caminho classico.
    enum EGpuMainView : u32 {
        GpuMainViewOk = 0,
        // Sem Setup completo, ou liberado.
        GpuMainViewNotBuilt = 1,
        // `Prepare` sem command list: a cadeia sempre grava.
        GpuMainViewNoCommandList = 2,
        // `Record` sem uma preparacao valida para ESTA lista e para estes pools.
        GpuMainViewNotPrepared = 3,
        // A lista de candidatos nao coube na capacidade (`FGpuSceneCandidates::IsUsable`). Nunca ha lista
        // parcial: geometria sumindo e o que a regra 6 proibe.
        GpuMainViewCandidatesOverflow = 4,
        // Mais ranges que `kRangeCapacity`.
        GpuMainViewTooManyRanges = 5,
        // Mais saidas da expansao que linhas de `ExpandedSlots`.
        GpuMainViewTooManyOutputs = 6,
        // Mais saidas do que um dispatch de uma dimensao alcanca (`kMaxOutputs`).
        GpuMainViewDispatchTooLarge = 7,
        // Um pool nao virou view (`GpuPoolViewsFor`); o detalhe e o `EGpuPoolViews`.
        GpuMainViewBadPoolViews = 8,
        // O staging recusou a copia (linha alem da capacidade).
        GpuMainViewUploadRefused = 9,
        // A recontagem do layout recusou; o detalhe e o `EGpuLayoutRefresh`.
        GpuMainViewLayoutRefused = 10,
        // O layout tem mais lotes do que os buffers por lote (pools cresceram sem Setup).
        GpuMainViewSmallBatchBuffers = 11,
        // `GpuRegionPreflight` recusou com a capacidade ALOCADA de `IndirectArgs`; o detalhe e o
        // `EGpuRegionPreflight`. E aqui que `TotalCommands <= capacidade` e garantido antes do dispatch.
        GpuMainViewRegionsRefused = 12,
        // `GpuClassifyPreflightFor` recusou; o detalhe e o `EGpuClassifyPreflight`.
        GpuMainViewClassifyRefused = 13,
        // A porta da gravacao dos draws recusou para algum passe; o detalhe e o `EGpuIndirectRecord`.
        GpuMainViewRecordRefused = 14,
        GpuMainViewReasonCount = 15,
    };

    const char* GpuMainViewReason(u32 Reason);

    // Contadores da GPU de UMA cadeia executada, lidos de volta no uso seguinte do mesmo slot. So
    // telemetria.
    struct FGpuMainViewGpuCounters {
        bool Valid  = false; // houve cadeia gravada no uso anterior do slot
        u32  Status = 0;     // 1 = frame fechou; 0 = a finalizacao zerou o `DrawCounts`
        u32  Reason = 0;     // bits `EGpuFinalizeReason`
        std::array<u32, GpuExpandCounterCount>   Expand{};
        std::array<u32, GpuClassifyOutcomeCount> Classify{};
        std::array<u32, GpuScatterCounterCount>  Scatter{};
    };

    // Telemetria CPU. Os maximos existem porque a instantanea esconde o pico entre dois frames olhados
    // (regra 5 do plano).
    struct FGpuMainViewStats {
        u64 ChainsRecorded = 0;
        std::array<u64, GpuMainViewReasonCount> Refusals{}; // por motivo; o indice Ok nao e usado
        u32 LastReason        = GpuMainViewNotBuilt;
        u32 LastDetail        = 0;
        u32 Outputs           = 0; // saidas da expansao na ultima cadeia gravada
        u32 OutputsHighWater  = 0;
        u32 Commands          = 0; // `TotalCommands` do layout da ultima cadeia gravada
        u32 CommandsHighWater = 0;
        u32 CandidateRowsUploaded = 0; // linhas de candidato copiadas na ultima cadeia gravada
        u64 RegionUploads         = 0; // tabelas de regiao copiadas (uma por versao de layout nova)
    };

    class FGpuIndirectMainView {
    public:
        static constexpr u32 kFrameSlots = FGpuTableStorage::kFrameSlots;
        static_assert(kFrameSlots == 2, "DescriptorBase_ e inicializado para dois frame slots");
        // Ranges que os buffers comportam. A cena plana emite um; o produtor por celula (Fase 8) e quem
        // vai precisar de mais, e ai este numero vira capacidade dimensionada.
        static constexpr u32 kRangeCapacity = 64;
        // `[numthreads(64, 1, 1)]` dos tres shaders por candidato.
        static constexpr u32 kThreadGroupSize = 64;
        static constexpr u32 kMaxOutputs =
            kThreadGroupSize * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

        static constexpr D3D12_RESOURCE_STATES kArgsRestState       = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        static constexpr D3D12_RESOURCE_STATES kDrawCountsRestState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        static constexpr D3D12_RESOURCE_STATES kExpandedRestState   = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // `BatchCounts` e `BatchCursors`.
        static constexpr D3D12_RESOURCE_STATES kBatchRestState      = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // Os tres buffers de contadores e o `Status`: terminam a cadeia copiados para o readback.
        static constexpr D3D12_RESOURCE_STATES kCountersRestState   = D3D12_RESOURCE_STATE_COPY_SOURCE;

        // Root signatures e PSOs dos quatro passos. Lanca se faltar shader.
        void Initialize(ID3D12Device* Device);

        // Os buffers da cadeia para uma cena de ate `SlotCapacity` slots de instancia e `PoolCount` pools.
        // A capacidade vem da TABELA de instancias — candidatos, saidas da expansao e comandos sao todos
        // limitados por ela —, e a troca de classe redistribui as regioes dentro dela.
        //
        // Mesmo contrato de dreno e mesma forma fail-closed dos Setup das tabelas: substitui recursos que a
        // fila pode estar lendo, e lancou, sai LIBERADO.
        void Setup(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 SlotCapacity, u32 PoolCount);
        void Release(FTextureSRVHeap& SRVHeap) noexcept;

        // FASE 1, antes do BeginFrame e depois de a publicacao das tabelas ser aceita. Devolve o motivo;
        // recusada, `Record` recusa tambem.
        u32 Prepare(const FScene& Scene, const FGpuMeshTable& Meshes,
                    ID3D12GraphicsCommandList* CommandList);

        // FASE 2, depois de `GpuSceneRecordPublication`, com a MESMA lista. Nao aloca nem lanca. Grava a
        // cadeia inteira ou nada. `Passes` sao os consumidores dos draws deste frame (pode ser vazio).
        u32 Record(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot, FTextureSRVHeap& SRVHeap,
                   const FGpuMaterialTable& Materials, const FGpuMeshTable& Meshes,
                   const FGpuSceneInstanceTable& Instances,
                   std::span<const FGpuIndirectPassSignature> Passes) noexcept;

        bool IsBuilt() const { return Built_; }
        u32  SlotCapacity() const { return SlotCapacity_; }
        u32  BatchCapacity() const { return BatchCapacity_; }

        // O layout e as views da ULTIMA gravacao aceita: os mesmos que os `ExecuteIndirect` dos passes
        // tem de usar.
        const FGpuIndirectLayout&      Layout() const { return LayoutState_.Layout(); }
        u32                            LastLayoutReason() const { return LayoutState_.LastReason(); }
        u64                            LayoutRebuildAttempts() const { return LayoutState_.RebuildAttempts(); }
        std::span<const FGpuPoolViews> PoolViews() const { return PoolViews_; }
        const FGpuSceneCandidates&     Candidates() const { return Candidates_; }

        // Os recursos, para os passes e para leitura de conferencia. Cada um no seu estado de repouso.
        ID3D12Resource* IndirectArgs() const { return IndirectArgs_.Get(); }
        ID3D12Resource* DrawCounts() const { return DrawCounts_.Get(); }
        ID3D12Resource* BatchCounts() const { return BatchCounts_.Get(); }
        ID3D12Resource* BatchCursors() const { return BatchCursors_.Get(); }
        ID3D12Resource* ExpandedSlots() const { return ExpandedSlots_.Get(); }
        ID3D12Resource* ExpandCounters() const { return ExpandCounters_.Get(); }
        ID3D12Resource* ClassifyCounters() const { return ClassifyCounters_.Get(); }
        ID3D12Resource* ScatterCounters() const { return ScatterCounters_.Get(); }
        ID3D12Resource* StatusBuffer() const { return Status_.Get(); }

        const FGpuMainViewStats&       Stats() const { return Stats_; }
        const FGpuMainViewGpuCounters& LastGpuCounters() const { return LastGpuCounters_; }

        // Bytes de CONTEUDO por orcamento, como no FGpuTableFootprint: VRAM, staging do host, upload dos
        // cbuffers e dos zeros do reset, e readback.
        u64 ResidentBytes() const;
        u64 StagingBytes() const;
        u64 UploadBytes() const;
        u64 ReadbackBytes() const;
        u32 DescriptorCount() const;

    private:
        u32  Refuse(u32 Reason, u32 Detail = 0) noexcept;
        void CollectGpuCounters(u32 Slot) noexcept;

        Microsoft::WRL::ComPtr<ID3D12Device> Device_;
        FComputePipeline ExpandPipeline_;
        FComputePipeline ClassifyPipeline_;
        FComputePipeline ScatterPipeline_;
        FComputePipeline FinalizePipeline_;
        bool Initialized_ = false;

        // Os quatro que SOBEM: DEFAULT + staging por frame slot + SRV, e o diff contra o shadow.
        FGpuTableStorage CandidateStorage_;
        FGpuTableStorage RangeStorage_;
        FGpuTableStorage BaseStorage_;
        FGpuTableStorage RegionStorage_;

        // Os que a GPU escreve.
        Microsoft::WRL::ComPtr<ID3D12Resource> ExpandedSlots_;
        Microsoft::WRL::ComPtr<ID3D12Resource> ExpandCounters_;
        Microsoft::WRL::ComPtr<ID3D12Resource> BatchCounts_;
        Microsoft::WRL::ComPtr<ID3D12Resource> ClassifyCounters_;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndirectArgs_;
        Microsoft::WRL::ComPtr<ID3D12Resource> BatchCursors_;
        Microsoft::WRL::ComPtr<ID3D12Resource> ScatterCounters_;
        Microsoft::WRL::ComPtr<ID3D12Resource> DrawCounts_;
        Microsoft::WRL::ComPtr<ID3D12Resource> Status_;

        GpuResources::FUploadBuffer            Zeros_;
        GpuResources::FUploadBuffer            ConstantBuffers_; // 4 por frame slot
        Microsoft::WRL::ComPtr<ID3D12Resource> Readback_;
        const u8*                              ReadbackMapped_ = nullptr;
        std::array<bool, kFrameSlots>          ReadbackPending_{};
        // A tabela de descritores de cada frame slot; invalida ate o Setup.
        std::array<u32, kFrameSlots>           DescriptorBase_{ 0xFFFFFFFFu, 0xFFFFFFFFu };

        FGpuSceneCandidates     Candidates_;
        FGpuIndirectLayoutState LayoutState_;
        std::vector<FGpuPoolViews> PoolViews_;

        std::vector<u32>             BaseRows_;
        std::vector<FGpuCandidateId> CandidateShadow_;
        std::vector<FInstanceRange>  RangeShadow_;
        std::vector<u32>             BaseShadow_;
        std::vector<u32>             CandidateChanged_;
        std::vector<u32>             RangeChanged_;
        std::vector<u32>             BaseChanged_;
        // [0, BatchCapacity): a tabela de regioes sobe inteira quando a versao do layout muda.
        std::vector<u32>             AllRegionRows_;

        FGpuMainViewStats       Stats_;
        FGpuMainViewGpuCounters LastGpuCounters_;

        u32  SlotCapacity_          = 0;
        u32  BatchCapacity_         = 0; // lotes enderecaveis: 0 sem pool
        u32  BatchRows_             = 0; // linhas dos buffers por lote: pelo menos uma
        u32  UploadedRegionsVersion_ = 0; // versao do layout cuja tabela o DEFAULT tem (0 = nenhuma)
        u32  PreparedOutputs_       = 0;
        u32  PreparedPools_         = 0;
        bool CandidatesPending_     = false;
        bool Built_                 = false;
        bool Prepared_              = false;
        ID3D12GraphicsCommandList* PreparedList_ = nullptr;
    };
}
