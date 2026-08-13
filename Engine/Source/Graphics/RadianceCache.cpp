#include "Smile/Graphics/RadianceCache.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/FrameContext.h"
#include "Smile/Graphics/SceneTargets.h"
#include "Smile/Graphics/DebugTargets.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // Espelham o layout do RadianceCacheResolve.cs.hlsl. Entries e 1 uint (checksum), Accum
        // sao 4 uints (rgb em ponto fixo + contagem) e Resolved e 1 uint4.
        constexpr u32 kEntryStride    = sizeof(u32);
        constexpr u32 kAccumStride    = sizeof(u32) * 4;
        constexpr u32 kResolvedStride = sizeof(u32) * 4;
        // Espelha RC_STAT_COUNT do RadianceCache.hlsli.
        constexpr u32 kStatCount      = 30;
        constexpr u32 kStatBytes      = kStatCount * sizeof(u32);
        // Os seis contadores de miss sao endereçados no shader por
        // `RC_STAT_MISS_SHORT + (status - RC_QUERY_SHORT_SEGMENT)`, ou seja as duas sequencias
        // andam juntas. Aqui a leitura do readback os desdobra na ordem do struct, e o que trava o
        // par e este bloco: os seis motivos comecam no slot 6, e o primeiro slot depois deles e o
        // da telemetria de insercao. Mudar um dos lados sem o outro trocaria "cone estreito" por
        // "chave ausente" no painel — dois diagnosticos opostos, e nada acusaria.
        // O mesmo vale para o bloco do PRODUTOR, endereçado por `RC_STAT_TERM_SKY + kind`.
        static_assert(kStatCount == 30, "layout dos contadores: 7 de tabela/query + 6 de miss + 7 "
                                        "de insercao + 10 do produtor. Mudou o shader? Mude a "
                                        "leitura tambem.");
        // Modos do dispatch do resolve (StatsParams.x). Ver o cabecalho do shader.
        constexpr f32 kModeResolve    = 0.0f;
        constexpr f32 kModeClearStats = 1.0f;
        constexpr DXGI_FORMAT kDebugFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateStructuredUAV(ID3D12Device* Device, u32 Count, u32 Stride,
                                                   const char* Label) {
            return GpuResources::CreateBuffer(
                Device, static_cast<u64>(Count) * Stride,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                EVramCategory::GI, Label);
        }
    }

    void FRadianceCache::Initialize(ID3D12Device* Device) {
        // 1 SRV / 4 UAVs. O shader nao le SRV nenhum — o caminho de dados do cache e todo UAV,
        // porque o ShadeSurfaceHit le e escreve na mesma invocacao. Mas um range de 0 descritores
        // nao serializa em D3D12, entao o root param 1 existe e o RecordResolve o aponta para o
        // BindingSRV. Os 4 UAVs precisam ser CONTIGUOS — sao um range so.
        ResolvePSO.Initialize(Device, "RadianceCacheResolve.cs_6_6.cso", 1, 4, false);
        // Visualizador: 2 SRVs (depth, normal) + 1 UAV de saida. Bindless para chegar na tabela
        // do cache, igual aos traces.
        DebugPSO.Initialize(Device, "RadianceCacheDebug.cs_6_6.cso", 2, 1, true);
        CreateUpdatePipeline(Device);
        CreateConstantBuffer(Device);
        Initialized = true;
    }

    void FRadianceCache::CreateUpdatePipeline(ID3D12Device* Device) {
        // 9 SRVs (ver kUpdateSrvCount) e bindless, porque a tabela do cache e alcancada por
        // ResourceDescriptorHeap, igual aos traces.
        //
        // 1 UAV que o shader NAO declara: o caminho de escrita do passe e todo bindless, mas um
        // range de 0 descritores nao serializa em D3D12 — o mesmo motivo do BindingSRV do resolve.
        // O destino e o buffer de contadores porque e nele que a telemetria deste passe (caminhos
        // lancados, profundidade, terminal por tipo) escreve desde a Fase 4 — por bindless, como
        // todo o resto; o binding aqui continua existindo so para o range serializar.
        UpdatePSO.Initialize(Device, "RadianceCacheUpdate.cs_6_6.cso", kUpdateSrvCount, 1, true);
    }

    void FRadianceCache::OnRecreatePipelines(const FPassInitContext& Ctx) {
        if (!Initialized || !Ctx.Device) return;
        ResolvePSO.Initialize(Ctx.Device, "RadianceCacheResolve.cs_6_6.cso", 1, 4, false);
        DebugPSO.Initialize(Ctx.Device, "RadianceCacheDebug.cs_6_6.cso", 2, 1, true);
        CreateUpdatePipeline(Ctx.Device);
        // Full HLSL reloads may change the shared hash/key semantics. Old entries are then
        // undecodable by the new query code even though the buffers themselves survived.
        //
        // ANTES do ResetOnce, que ja poe o estado em Resetting e tornaria a resposta falsa: a
        // consulta estava aberta e vai FECHAR, e isso e uma troca de terminador como qualquer
        // outra — o ReSTIR GI e o atlas do DDGI seguram radiancia que veio do cache, sob uma
        // semantica de chave que este reload pode ter acabado de mudar.
        //
        // Este e o unico ResetOnce da classe que nao vem acompanhado de invalidacao: os outros
        // chegam por setter do FRenderSettings ou pelo proprio funil. Por isso o latch e armado
        // AQUI e nao dentro do ResetOnce — la ele pegaria tambem o reset da sessao de captura,
        // que roda sob a guarda justamente para nao cancelar nada, e o cancelaria no frame
        // seguinte, fora dela.
        const bool WasQuerying = QueryEffective();
        if (Ready) ResetOnce();
        // De brinde, o que fechava a captura por fora: um hot reload durante o aquecimento
        // resetava o cache sem cancelar a sessao. Consumido pelo funil, agora cancela — e deve:
        // os pipelines mudaram no meio da contagem.
        if (WasQuerying) QueryChanged = true;
    }

    FPassShaderStems FRadianceCache::ShaderStems() const {
        // O RadianceCache.hlsli nao entra: header nao gera .cso. Editar um .hlsli dispara o
        // reload COMPLETO, que passa por aqui de qualquer forma — e tambem pelos traces que o
        // incluem, que e o que importa quando o hash muda.
        static const char* const Stems[] = { "RadianceCacheResolve", "RadianceCacheDebug",
                                             "RadianceCacheUpdate" };
        return { Stems, 3u };
    }

    bool FRadianceCache::IsActive(const FFrameModes&) const {
        // Nao depende de qual consumidor roda: o resolve tem de rodar sempre que houve escrita,
        // e a escrita vem de dentro do ShadeSurfaceHit, que os tres traces compartilham. Gatear
        // por ReSTIRGIActive deixaria o Accum sujo do frame anterior entrar na media seguinte.
        return IsInitialized() && Enabled;
    }

    void FRadianceCache::CreateConstantBuffer(ID3D12Device* Device) {
        static_assert(sizeof(RadianceCacheConstants) % 256 == 0 &&
                      sizeof(RadianceCacheDebugConstants) % 256 == 0 &&
                      sizeof(RadianceCacheUpdateConstants) % 256 == 0,
                      "os CBAddr indexam por sizeof(); root CBV exige 256-alinhado");
        // A cauda de cascatas espelha o FDDGICascadeConstants, que este header nao pode incluir
        // (ciclo). Sem o tipo, o que resta e travar o TAMANHO: 16 + 4x16 + 4x16 = 144 B, os
        // mesmos que o static_assert do DDGI.h exige la. Divergir aqui deslocaria todo o bloco
        // do cache no cbuffer e o shader leria os UAVs errados.
        static_assert(sizeof(RadianceCacheUpdateConstants) -
                          offsetof(RadianceCacheUpdateConstants, GICascadeParams) >= 144,
                      "cauda de cascatas menor que o FDDGICascadeConstants que ela espelha");

        // 2 variantes por frame em voo (resolve e clear-stats; ver CBAddr).
        const GpuResources::FUploadBuffer Main = GpuResources::CreateUploadBuffer(
            Device, sizeof(RadianceCacheConstants), FCommandQueue::kFramesInFlight * 2);
        CB       = Main.Resource;
        MappedCB = Main.Mapped;

        const GpuResources::FUploadBuffer Debug = GpuResources::CreateUploadBuffer(
            Device, sizeof(RadianceCacheDebugConstants), FCommandQueue::kFramesInFlight);
        DebugCB       = Debug.Resource;
        MappedDebugCB = Debug.Mapped;

        const GpuResources::FUploadBuffer Upd = GpuResources::CreateUploadBuffer(
            Device, sizeof(RadianceCacheUpdateConstants), FCommandQueue::kFramesInFlight);
        UpdateCB       = Upd.Resource;
        MappedUpdateCB = Upd.Mapped;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FRadianceCache::CBAddr(u32 Variant) const {
        const UINT64 Index = static_cast<UINT64>(FrameSlot) * 2ull + Variant;
        return CB->GetGPUVirtualAddress() + Index * sizeof(RadianceCacheConstants);
    }

    D3D12_GPU_VIRTUAL_ADDRESS FRadianceCache::DebugCBAddr() const {
        return DebugCB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(RadianceCacheDebugConstants);
    }

    D3D12_GPU_VIRTUAL_ADDRESS FRadianceCache::UpdateCBAddr() const {
        // UpdateFrameSlot, e nao FrameSlot: o CB e preenchido no PrepareIndirectLighting e o
        // dispatch acontece depois do G-buffer. Sao o mesmo frame hoje, mas amarrar a gravacao ao
        // slot que ESCREVEU o buffer e o que mantem os dois pontos independentes.
        return UpdateCB->GetGPUVirtualAddress() +
               static_cast<UINT64>(UpdateFrameSlot) * sizeof(RadianceCacheUpdateConstants);
    }

    u64 FRadianceCache::MemoryBytes() const {
        if (!Ready) return 0;
        return static_cast<u64>(CapacityV) * (kEntryStride + kAccumStride + kResolvedStride);
    }

    void FRadianceCache::Release(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(UavTable, 4);
        Free(BindingSRV, 1);
        // A tabela do passe de update carrega o snapshot InstanceGeo e a TLAS, que a troca de
        // cena realoca. Solta-la aqui garante que o UpdateReady so volte a ser true depois de um
        // SetupUpdatePass novo — senao o dispatch usaria descritores da cena anterior.
        ReleaseUpdatePass(SRVHeap);
        Entries.Reset(); Accum.Reset(); Resolved.Reset(); StatsBuf.Reset();
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
            StatsReadback[f].Reset();
            StatsReadbackPending[f] = false;
        }
        CaptureStatsReadback.Reset();
        CaptureStatsPending = false;
        StatsCPU = {};
        EntriesState = AccumState = ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        StatsState   = D3D12_RESOURCE_STATE_COMMON;
        CapacityV = 0;
        Ready = false;
    }

    void FRadianceCache::ReleaseUpdatePass(FTextureSRVHeap& SRVHeap) {
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
            if (UpdateSrvTable[f] != kInvalidSlot) {
                SRVHeap.Free(UpdateSrvTable[f], kUpdateSrvCount);
                UpdateSrvTable[f] = kInvalidSlot;
            }
        }
        UpdateWidth = UpdateHeight = 0;
        UpdateReady = false;
    }

    void FRadianceCache::ReleaseDebug(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(DebugSRV, 1);
        Free(DebugUAV, 1);
        Free(DebugSrvTable, 2);
        DebugTex.Reset();
        DebugTexState = D3D12_RESOURCE_STATE_COMMON;
        DebugWidth = DebugHeight = 0;
    }

    void FRadianceCache::SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                       u32 CapacityLog2) {
        if (!Initialized) return;
        Release(SRVHeap);

        CapacityLog2 = std::clamp(CapacityLog2, 16u, kMaxCapacityLog2);
        CapacityV    = 1u << CapacityLog2;

        Entries  = CreateStructuredUAV(Device, CapacityV, kEntryStride,
                                       "Radiance cache · chaves");
        Accum    = CreateStructuredUAV(Device, CapacityV * 4u, kEntryStride,
                                       "Radiance cache · acumulacao");
        Resolved = CreateStructuredUAV(Device, CapacityV, kResolvedStride,
                                       "Radiance cache · resolvido");
        StatsBuf = CreateStructuredUAV(Device, kStatCount, kEntryStride,
                                       "Radiance cache · contadores");

        // Anel de readback dos contadores (ver o membro StatsReadback).
        {
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                StatsReadback[f] = GpuResources::CreateReadbackBuffer(Device, kStatBytes);
                StatsReadbackPending[f] = false;
            }
            CaptureStatsReadback = GpuResources::CreateReadbackBuffer(Device, kStatBytes);
            CaptureStatsPending  = false;
        }

        // Os 4 UAVs em slots contiguos: o root param 2 do ResolvePSO e UM range de 4.
        UavTable = SRVHeap.Allocate(4);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                     = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements         = CapacityV;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, Entries.Get(), Uav, UavTable + 0);

        // O Accum e declarado como RWStructuredBuffer<uint> no shader e indexado por
        // entryIndex*4 + canal — por isso NumElements e 4x e o stride e de uint, nao do quarteto.
        Uav.Buffer.NumElements         = CapacityV * 4u;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, Accum.Get(), Uav, UavTable + 1);

        Uav.Buffer.NumElements         = CapacityV;
        Uav.Buffer.StructureByteStride = kResolvedStride;
        SRVHeap.CreateUAV(Device, Resolved.Get(), Uav, UavTable + 2);

        Uav.Buffer.NumElements         = kStatCount;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, StatsBuf.Get(), Uav, UavTable + 3);

        // Descritor de amarracao para o root param 1 do ResolvePSO (ver o membro BindingSRV).
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                     = DXGI_FORMAT_UNKNOWN;
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.NumElements         = CapacityV;
        Srv.Buffer.StructureByteStride = kEntryStride;
        BindingSRV = SRVHeap.Allocate(1);
        SRVHeap.CreateSRV(Device, Entries.Get(), Srv, BindingSRV);

        // Os buffers nascem com lixo (CreateCommittedResource nao zera). Uma chave aleatoria que
        // por acaso valha um checksum plausivel devolveria radiancia inventada no primeiro frame,
        // entao o primeiro resolve TEM que ser um reset.
        //
        // Pelo ResetOnce, e nao escrevendo o ResetPending a dedo: o aquecimento global tambem
        // nasce daqui, e uma cena nova herdando o `Active` da anterior abriria a consulta sobre a
        // tabela recem-criada. Escrever a flag direto e a porta lateral que faz o estado divergir
        // do evento — a mesma que ja custou um bug de "reset armado depois do tick".
        ResetOnce();
        Ready = true;

        LogDebug("[GI] - Radiance cache: " + std::to_string(CapacityV) + " celulas, " +
                 std::to_string(MemoryBytes() / (1024ull * 1024ull)) + " MB, celula base " +
                 std::to_string(BaseCellSize) + " m");
    }

    const char* FRadianceCache::WarmupStateName() const {
        switch (WarmupState()) {
            case ERadianceCacheWarmup::Resetting: return "resetting";
            case ERadianceCacheWarmup::Filling:   return "filling";
            default:                              return "active";
        }
    }

    // Resetting -> Filling -> Active. Roda uma vez por frame, no UpdatePerFrame, ANTES de qualquer
    // consumidor pedir params — e depois do CollectStats do BeginFrame, que e de onde sai o unico
    // sinal que ele le.
    //
    // Ele le a VARREDURA DA TABELA (celulas confiaveis / celulas com amostra), e nao o hit rate,
    // e isso nao e detalhe de implementacao: o hit rate so existe com a instrumentacao ligada, que
    // e um regime de MEDICAO e nao roda em producao. Um gate que dependesse dela ficaria preso em
    // Filling para sempre no unico modo em que ele importa. A varredura, ao contrario, e escrita
    // pelo resolve todo frame, sem flag nenhuma.
    void FRadianceCache::TickWarmup() {
        // Cache desligado: CONGELA, nao volta para Resetting. A tabela continua la (SetEnabled nao
        // invalida), entao religar deve devolver o estado que a tabela merece, e nao uma espera
        // nova por um conteudo que ja existe.
        if (!Ready || !Enabled) return;

        // Ainda esperando o resolve de reset. Quem pos o estado em Resetting foi o proprio
        // ResetOnce — este tick nao amostra o `ResetPending` para decidir, so espera ele sair.
        if (ResetPending) return;

        if (Warmup == ERadianceCacheWarmup::Resetting) {
            Warmup     = ERadianceCacheWarmup::Filling;
            FillFrames = 0;
        }
        if (Warmup != ERadianceCacheWarmup::Filling) return;

        ++FillFrames;
        if (FillFrames <= kWarmupStatsLag) return; // StatsCPU ainda e do regime anterior

        // TABELA COM CONTEUDO e pre-requisito dos DOIS caminhos, e nao so do de convergencia. Sem
        // isto o teto de frames abriria a consulta sobre uma tabela vazia — o produtor desligado,
        // ou parado por qualquer motivo —, que e exatamente o custo (busca em toda consulta, miss
        // em todas) que este estado existe para evitar, e sem prazo para acabar. Enquanto nao ha o
        // que ler, esperar e a resposta certa; o A/B manual continua disponivel para forcar.
        if (StatsCPU.HasSamples == 0u) return;

        // "Confiaveis o bastante" tem denominador nas celulas COM AMOSTRA, e nao na capacidade:
        // a fracao ocupada da tabela e uma propriedade da CENA (quanta superficie o produtor
        // alcanca), nao do aquecimento. Medir contra a capacidade faria uma cena pequena nunca
        // aquecer.
        const bool Converged = static_cast<f32>(StatsCPU.Confident) >=
                               kWarmupConfidentRatio * static_cast<f32>(StatsCPU.HasSamples);
        // `>` e nao `>=`: FillFrames e o frame que esta COMECANDO, entao a igualdade ainda deve um
        // frame de preenchimento.
        if (Converged || FillFrames > kWarmupMaxFrames) {
            Warmup = ERadianceCacheWarmup::Active;
            // O latch descreve uma mudanca EFETIVA do que o render consulta — nao a transicao de
            // estado. A maquina continua andando com o AutoWarmup desligado, de proposito (para o
            // estado estar certo quando o knob voltar), mas ali ela nao estava fechando nada:
            // invalidar seria derrubar historico sem que nada tivesse mudado para consumidor
            // nenhum. E dentro de uma sessao de captura seria pior que inutil — o funil
            // cancelaria a propria sessao que desliga o AutoWarmup justamente para nao cancelar.
            //
            // `QueryEnabled` entra pelo mesmo argumento, um nivel acima: com a leitura desligada o
            // render nao consulta em estado nenhum, e abrir um portao atras de outro fechado nao
            // muda o que chega aos consumidores.
            //
            // E `AutoWarmup && QueryEffective()` escrito por extenso: os retornos antecipados la
            // em cima ja garantem Enabled, Ready e o pending fora.
            QueryChanged = AutoWarmup && QueryEnabled;
        }
    }

    void FRadianceCache::UpdatePerFrame(u32 InFrameSlot, const Vec3& InCameraPos) {
        FrameSlot = InFrameSlot;
        CameraPos = InCameraPos;
        // Comeco do frame: ninguem pediu params ainda. Os tres ShaderParams dos consumidores vem
        // logo a seguir, no mesmo PrepareIndirectLighting.
        PublishedUpdate = false;
        PublishedQuery  = false;
        PublishedStats  = false;
        PublishedDetail = false;
        PublishedDedicated    = false;
        PublishedUpdateCells  = 0;
        PublishedVertices     = 0;
        PublishedMinRoughness = 0.0f;
        PublishedMinSamples   = 0;
        if (!Ready || !MappedCB) return;
        CPU.CacheParams = { static_cast<f32>(CapacityV),
                            static_cast<f32>(kMaxAccumSamples),
                            static_cast<f32>(kStaleFrameMax),
                            ResetPending ? 1.0f : 0.0f };
        const size_t Base = static_cast<size_t>(FrameSlot) * 2u * sizeof(RadianceCacheConstants);
        // O piso vai nas DUAS variantes por simetria, mas so a do resolve o le (o clear nao conta
        // celula nenhuma). A varredura precisa dele para separar "tem amostra" de "e confiavel".
        const f32 MinS = static_cast<f32>(MinSampleCount);
        CPU.StatsParams = { kModeResolve, MinS, 0.0f, 0.0f };
        std::memcpy(MappedCB + Base, &CPU, sizeof(CPU));
        CPU.StatsParams = { kModeClearStats, MinS, 0.0f, 0.0f };
        std::memcpy(MappedCB + Base + sizeof(RadianceCacheConstants), &CPU, sizeof(CPU));
    }

    void FRadianceCache::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                                    D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
        if (State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Resource;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = After;
        CL->ResourceBarrier(1, &B);
        State = After;
    }

    void FRadianceCache::TransitionForTrace(ID3D12GraphicsCommandList* CL) {
        if (!Ready) return;
        // Os tres ficam em UNORDERED_ACCESS o frame inteiro. O ShadeSurfaceHit LE Entries e
        // Resolved e ESCREVE Entries e Accum na mesma invocacao: se a leitura fosse por SRV o
        // recurso teria que estar em NON_PIXEL e em UAV ao mesmo tempo. Ver o cabecalho do
        // RadianceCache.hlsli.
        Transition(CL, Entries.Get(),  EntriesState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Accum.Get(),    AccumState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, StatsBuf.Get(), StatsState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void FRadianceCache::RecordResolve(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!Ready) return;
        const bool ResetThisFrame = ResetPending;

        Transition(CL, Entries.Get(),  EntriesState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Accum.Get(),    AccumState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Os tres JA estao em UNORDERED_ACCESS desde o TransitionForTrace, entao as chamadas
        // acima sao no-op — e e exatamente por isso que a barreira de UAV precisa ser explicita.
        // Sem ela nada ordena as escritas atomicas dos traces contra a leitura do resolve, e o
        // sintoma seria a media incorporar um Accum parcial: celulas escuras intermitentes, sem
        // padrao, que passariam facil por ruido de amostragem.
        D3D12_RESOURCE_BARRIER Uav[3]{};
        for (u32 i = 0; i < 3; ++i) Uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[0].UAV.pResource = Entries.Get();
        Uav[1].UAV.pResource = Accum.Get();
        Uav[2].UAV.pResource = Resolved.Get();
        CL->ResourceBarrier(3, Uav);

        // COPIA ANTES DE ZERAR. Os contadores de QUERY foram escritos pelos traces, que ja
        // rodaram neste frame; os de OCUPACAO, pelo resolve, que ainda vai rodar. Copiar aqui
        // captura query deste frame e ocupacao do frame ANTERIOR. Um frame de defasagem num
        // painel de diagnostico e invisivel; a alternativa (zerar antes) apagaria a medida de
        // query do proprio frame, que e justamente a mais cara de obter.
        // No primeiro resolve os buffers ainda contem o lixo de CreateCommittedResource.
        // Copiar os contadores antes do reset publicaria uma amostra espuria no painel.
        if (!ResetThisFrame) {
            Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CL->CopyBufferRegion(StatsReadback[FrameSlot].Get(), 0, StatsBuf.Get(), 0, kStatBytes);
            StatsReadbackPending[FrameSlot] = true;
            Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        ResolvePSO.Bind(CL);
        // Root param 1 existe so para o range de 1 SRV do root signature nao ficar sem binding
        // (ver Initialize); o shader nao le nada dele.
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(BindingSRV));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(UavTable));

        // Zera os contadores num dispatch proprio: nada ordena "zerar" contra "acumular" dentro
        // de um mesmo dispatch, entre grupos.
        CL->SetComputeRootConstantBufferView(0, CBAddr(1));
        CL->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER StatsBarrier{};
        StatsBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        StatsBarrier.UAV.pResource = StatsBuf.Get();
        CL->ResourceBarrier(1, &StatsBarrier);

        CL->SetComputeRootConstantBufferView(0, CBAddr(0));
        CL->Dispatch((CapacityV + 63u) / 64u, 1, 1);

        ResetPending = false;
    }

    // ================================================================================================
    // Passe dedicado de update (Fase 3)
    // ================================================================================================
    void FRadianceCache::SetupUpdatePass(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                         u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot,
                                         const FGIFallbackBindings& Fallback,
                                         u32 DepthSlot, u32 GBufferSlot,
                                         u32 Width, u32 Height) {
        if (!Initialized) return;
        ReleaseUpdatePass(SRVHeap);

        // Os OITO slots estaveis, e nao so os tres obvios: todos vao direto para
        // CpuHandleStaging, e la um kInvalidSlot (0xFFFFFFFF) vira base + 0xFFFFFFFF * HandleSize
        // — ~128 GiB fora do heap, lidos pelo CopyDescriptors. Mesma validacao do FReSTIRGI, pelo
        // mesmo motivo: e invariante da CLASSE, nao do call site.
        //
        // Os tres do fallback entram mesmo com Available == false. Este passe nunca os le, mas
        // eles ocupam t3/t4/t5 e a tabela precisa estar cheia; "sem volume" quer dizer slot
        // NEUTRO, nunca slot invalido.
        if (Width == 0 || Height == 0 ||
            TlasSlot == kInvalidSlot || SkyViewSlot == kInvalidSlot ||
            InstanceSlot == kInvalidSlot || DepthSlot == kInvalidSlot ||
            GBufferSlot == kInvalidSlot ||
            Fallback.IrradianceAtlasSRV == kInvalidSlot ||
            Fallback.DistanceAtlasSRV == kInvalidSlot ||
            Fallback.ProbeDataSRV == kInvalidSlot) {
            return;
        }

        UpdateWidth  = Width;
        UpdateHeight = Height;

        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
            UpdateSrvTable[f] = SRVHeap.Allocate(kUpdateSrvCount);
            // NOVE entradas, e a nona e FILLER. O t8 (luzes) e reescrito por frame pelo
            // SetUpdatePunctualLightsSRV, e a ordem do frame garante que isso aconteca antes do
            // dispatch — mas garantia por ORDEM DE CHAMADA e mais fraca que a tabela nascer
            // completa. Sem o filler, um caminho novo que chegasse ao RecordUpdate sem passar pelo
            // PrepareIndirectLighting leria o conteudo velho do heap naquele slot; com ele, o pior
            // caso e ler o snapshot de instancias como se fosse luz — e nem isso acontece, porque
            // o CB desse mesmo frame traria NumLights = 0 e o loop nao roda. Mesmo padrao dos
            // fillers da tabela de trace do FReSTIRGI.
            const D3D12_CPU_DESCRIPTOR_HANDLE Src[9] = {
                SRVHeap.CpuHandleStaging(TlasSlot),
                SRVHeap.CpuHandleStaging(SkyViewSlot),
                SRVHeap.CpuHandleStaging(InstanceSlot),
                SRVHeap.CpuHandleStaging(Fallback.IrradianceAtlasSRV),
                SRVHeap.CpuHandleStaging(Fallback.DistanceAtlasSRV),
                SRVHeap.CpuHandleStaging(Fallback.ProbeDataSRV),
                SRVHeap.CpuHandleStaging(DepthSlot),
                SRVHeap.CpuHandleStaging(GBufferSlot),
                SRVHeap.CpuHandleStaging(InstanceSlot), // t8: filler ate as luzes do 1o frame
            };
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(UpdateSrvTable[f]);
            UINT Count = kUpdateSrvCount; UINT Ones[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
            Device->CopyDescriptors(1, &Dst, &Count, kUpdateSrvCount, Src, Ones,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        UpdateReady = true;
    }

    void FRadianceCache::SetUpdatePunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                    u32 StagingSlot, u32 InFrameSlot) {
        if (!UpdateReady || InFrameSlot >= FCommandQueue::kFramesInFlight ||
            StagingSlot == kInvalidSlot) {
            return;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(UpdateSrvTable[InFrameSlot] + 8);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = SRVHeap.CpuHandleStaging(StagingSlot);
        UINT One = 1;
        Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FRadianceCache::UpdatePassPerFrame(u32 InFrameSlot, const Mat44& InvViewProj,
                                            const Vec3& InCameraPos, const Vec3& SunDir,
                                            f32 SunIntensity, const Vec3& SunColor,
                                            u32 ShadowRayMask, u32 FrameIndex, f32 SkyIntensity,
                                            f32 MaxRayDist, u32 PunctualLightCount,
                                            bool BackfacePolicy) {
        if (!UpdateReady || !MappedUpdateCB) return;
        UpdateFrameSlot = InFrameSlot;

        RadianceCacheUpdateConstants& U = UpdateCPU;
        U = RadianceCacheUpdateConstants{}; // a cauda de cascatas fica zerada, e e assim que ela viaja
        U.InvViewProj  = InvViewProj;
        U.CameraPos    = { InCameraPos.X, InCameraPos.Y, InCameraPos.Z, 1.0f };
        U.ScreenParams = { static_cast<f32>(UpdateWidth), static_cast<f32>(UpdateHeight),
                           1.0f / static_cast<f32>(UpdateWidth),
                           1.0f / static_cast<f32>(UpdateHeight) };
        // Grade/atlas do DDGI: NEUTROS, nao os reais. Este passe nao amostra sonda, e o unico
        // consumidor destes campos e o gather que ele nao chama. 1 no lugar de 0 nas dimensoes do
        // atlas porque o FHitShadeParams calcula 1/AtlasParams.yz ao montar a struct — com zero
        // sairia inf num campo que ninguem le, e inf num registrador e o tipo de coisa que
        // reaparece meses depois num shader diferente.
        U.AtlasParams  = { 6.0f, 1.0f, 1.0f, 0.0f };
        U.SunDirIntensity = { SunDir.X, SunDir.Y, SunDir.Z, SunIntensity };
        U.SunColor        = { SunColor.X, SunColor.Y, SunColor.Z,
                              static_cast<f32>(ShadowRayMask) };
        U.TraceParams     = { static_cast<f32>(FrameIndex), MaxRayDist, SkyIntensity,
                              RayEps.HitShadowRayBias };
        U.ShadeParams     = { static_cast<f32>(PunctualLightCount), kUpdateAlbedoLOD, 0.0f, 0.0f };
        U.PolicyParams    = { BackfacePolicy ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };

        // Fracao -> celulas do tile 5x5. O piso de 1 e deliberado: com o passe ligado e fracao
        // arredondando para zero, nenhum caminho sairia e a tabela envelheceria ate o despejo com
        // a UI dizendo "update ativo". Quem quer zero desliga o produtor dedicado.
        const f32 CellsF = UpdateFraction * 25.0f;
        u32 Cells = static_cast<u32>(CellsF + 0.5f);
        Cells = std::clamp(Cells, 1u, 25u);
        // O que o shader vai receber de fato — e so isso e que o manifesto reporta. A quantizacao
        // acontece AQUI, entao publicar o knob cru faria a captura afirmar uma fracao que nao foi
        // a usada (0,10 pedido vira 3/25 = 0,12).
        if (UpdatePassActive()) {
            PublishedUpdateCells  = Cells;
            PublishedVertices     = UpdateMaxVertices;
            PublishedMinRoughness = MinCacheableRoughness;
        }
        U.UpdateParams = { static_cast<f32>(Cells), static_cast<f32>(UpdateMaxVertices),
                           UsePrevCacheAtTerminal ? 1.0f : 0.0f, MinCacheableRoughness };

        U.RayEpsA = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                      RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        U.RayEpsB = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                      FRayEpsilonProfile::kOriginAngularMinRatio };
        // GIDistParams so viaja pelo contrato de nome; o .w traz o skipMode com o bit de corte do
        // terminador ja ligado (FallbackAvailable/TerminatorOff), o que e coerente com um passe
        // que nao consulta o volume de jeito nenhum.
        U.GIDistParams = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                           GIHit.SkipModePacked() };
        // .w e o unico campo desta linha que o passe LE: o piso de roughness do secundario. Ele
        // importa aqui mais que em qualquer consumidor — este e o produtor do cache
        // nao-direcional.
        U.GIBiasParams = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes,
                           GIHit.SecondaryRoughnessMin };
        U.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        U.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        U.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        U.ReGIRResources        = ReGIRParams.Resources;
        U.SkyParams             = SkyLutParams;

        const FRadianceCacheShaderParams P = UpdatePassParams();
        U.RadianceCacheCamCell     = P.CameraPosCell;
        U.RadianceCacheLodCapFlags = P.LodCapacityFlags;
        U.RadianceCacheResources   = P.Resources;

        std::memcpy(MappedUpdateCB +
                        static_cast<size_t>(UpdateFrameSlot) * sizeof(RadianceCacheUpdateConstants),
                    &U, sizeof(U));
    }

    void FRadianceCache::RecordUpdate(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!UpdatePassActive() || UpdateSrvTable[UpdateFrameSlot] == kInvalidSlot) return;

        // Os buffers do cache ja estao em UNORDERED_ACCESS desde o TransitionForTrace do
        // PrepareIndirectLighting, e continuam ate o resolve. Nao ha barreira a emitir contra os
        // traces de render: este passe ESCREVE Accum e LE Resolved, e o resolve do fim do frame ja
        // ordena as duas coisas contra a leitura dele.
        UpdatePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, UpdateCBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(UpdateSrvTable[UpdateFrameSlot]));
        // Root param 2 existe so porque um range de 0 descritores nao serializa (ver
        // CreateUpdatePipeline); o shader nao declara UAV nenhum.
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(UavTable + 3));

        // Dispatch de tela CHEIA com early-out pela mascara, e nao um dispatch compactado de 4%
        // dos pixels: o plano manda comprovar a correcao antes de comprimir, e a compactacao
        // (work list + indirect dispatch) e a Fase 7.
        CL->Dispatch((UpdateWidth + 7u) / 8u, (UpdateHeight + 7u) / 8u, 1);
    }

    FRadianceCacheShaderParams FRadianceCache::UpdatePassParams() const {
        FRadianceCacheShaderParams P{};
        u32 Flags = kFlagUpdate;
        if (UsePrevCacheAtTerminal) Flags |= kFlagQuery;
        // Sem kFlagStats, sempre — ver a nota da declaracao.
        //
        // COM kFlagStatsDetail, e isso nao contradiz a regra acima. Ela existe porque QUERIES/HITS
        // descrevem os RAIOS DE RENDER, e somar o terminal deste passe misturaria duas populacoes.
        // O detalhe carrega dois grupos com gates diferentes justamente por isso: os misses por
        // motivo exigem os DOIS bits (e este passe nao tem o de stats, entao nao os toca), e a
        // telemetria de INSERCAO exige so este — e ela descreve quem ESCREVE na tabela, que com o
        // produtor dedicado e este passe e mais ninguem. Sem isto, a saude do hash nao teria como
        // ser medida no unico regime em que ela existe.
        if (StatsEnabled && StatsDetail) Flags |= kFlagStatsDetail;
        P.CameraPosCell    = { CameraPos.X, CameraPos.Y, CameraPos.Z, BaseCellSize };
        // O MESMO piso de confianca do render. Ele so age se o terminal for consultar, e por isso
        // o registro abaixo esta amarrado ao kFlagQuery e nao ao passe rodar.
        P.LodCapacityFlags = { LodDistance, static_cast<f32>(CapacityV),
                               static_cast<f32>(Flags), static_cast<f32>(MinSampleCount) };
        P.Resources        = { static_cast<f32>(UavTable + 0),
                               static_cast<f32>(UavTable + 1),
                               static_cast<f32>(UavTable + 2),
                               static_cast<f32>(UavTable + 3) };
        // Quem realmente entregou o update neste frame passou a ser este passe. O manifesto da
        // captura le daqui (ver PublishedUpdateThisFrame): sem isto, uma captura com o produtor
        // dedicado ativo se declararia "cache sem update", porque nenhum consumidor de render
        // recebe mais o bit.
        //
        // Sob o MESMO criterio dos outros consumidores — "vai rodar de fato". O CB e preenchido
        // todo frame (custa nada), mas um frame que nao dispara o passe nao pode se registrar
        // como alimentado. E exatamente o caso do primeiro frame depois de um reset.
        if (UpdatePassActive()) {
            PublishedUpdate    = true;
            PublishedDedicated = true;
            if ((Flags & kFlagQuery) != 0u)       PublishedMinSamples = MinSampleCount;
            if ((Flags & kFlagStatsDetail) != 0u) PublishedDetail     = true;
        }
        return P;
    }

    void FRadianceCache::OnResize(const FPassInitContext& Ctx) {
        if (!Initialized || !Ctx.Device || !Ctx.SRVHeap || !Ctx.Targets) return;
        FTextureSRVHeap& SRVHeap = *Ctx.SRVHeap;
        ReleaseDebug(SRVHeap);
        if (Ctx.RenderWidth == 0 || Ctx.RenderHeight == 0) return;

        const u32 DepthSlot  = Ctx.Targets->DepthSRVSlot;
        const u32 NormalSlot = Ctx.Targets->NormalSRVSlot;
        // O NormalBuffer e opcional na engine (nasceu com a GTAO). Sem ele o visualizador nao tem
        // como reproduzir a chave — que usa a normal geometrica — entao ele simplesmente nao
        // existe, em vez de mentir com a normal de sombreamento.
        if (DepthSlot == FSceneTargets::kInvalidSlot ||
            NormalSlot == FSceneTargets::kInvalidSlot) {
            return;
        }

        DebugWidth  = Ctx.RenderWidth;
        DebugHeight = Ctx.RenderHeight;

        DebugTex = GpuResources::CreateTex2D(
            Ctx.Device, DebugWidth, DebugHeight, kDebugFormat,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
            EVramCategory::GI, nullptr, 1, 1, "Radiance cache · visualizador");
        DebugTexState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kDebugFormat;
        Srv.Texture2D.MipLevels     = 1;
        DebugSRV = SRVHeap.Allocate(1);
        SRVHeap.CreateSRV(Ctx.Device, DebugTex.Get(), Srv, DebugSRV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kDebugFormat;
        DebugUAV = SRVHeap.Allocate(1);
        SRVHeap.CreateUAV(Ctx.Device, DebugTex.Get(), Uav, DebugUAV);

        // t0/t1 contiguos: o root param 1 do DebugPSO e UM range de 2.
        DebugSrvTable = SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DebugSrvTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
                SRVHeap.CpuHandleStaging(DepthSlot),
                SRVHeap.CpuHandleStaging(NormalSlot),
            };
            UINT Two = 2; UINT Ones[2] = { 1, 1 };
            Ctx.Device->CopyDescriptors(1, &Dst, &Two, 2, Src, Ones,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

    }

    void FRadianceCache::OnRegisterDebugTargets() {
        // O OnResize acima pode ter desistido (sem NormalBuffer, ou render-res zerada) e deixado
        // o alvo inexistente. Publicar mesmo assim poria um SRV invalido na UI.
        if (!DebugTex || DebugSRV == kInvalidSlot) return;

        // Raw: o shader ja escreve cor em faixa visivel (falsa-cor de diagnostico, nao radiancia).
        // Passar por HDR/tonemap distorceria a leitura — verde e vermelho tem que sair como sao.
        DebugTargets::Register(kDebugTargetName, DebugSRV, EDebugDecode::Raw,
                               0, 1, 1.0f, 0, /*LinearFilter*/ false);
    }

    void FRadianceCache::RecordDebug(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                     const Mat44& InvViewProj, const Vec3& InCameraPos,
                                     u32 InFrameSlot) {
        if (!Ready || !DebugTex || DebugSrvTable == kInvalidSlot || !MappedDebugCB) return;

        // Os traces do frame podem ter acabado de publicar novas chaves. Entries continua em UAV,
        // portanto uma transition seria no-op; a barreira UAV e o que torna essas escritas
        // visiveis para a varredura do visualizador.
        D3D12_RESOURCE_BARRIER CacheBarrier{};
        CacheBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        CacheBarrier.UAV.pResource = Entries.Get();
        CL->ResourceBarrier(1, &CacheBarrier);

        RadianceCacheDebugConstants D{};
        D.InvViewProj   = InvViewProj;
        D.CameraPosMode = { InCameraPos.X, InCameraPos.Y, InCameraPos.Z,
                            static_cast<f32>(DebugMode) };
        D.ScreenParams  = { static_cast<f32>(DebugWidth), static_cast<f32>(DebugHeight),
                            1.0f / static_cast<f32>(DebugWidth),
                            1.0f / static_cast<f32>(DebugHeight) };
        // O visualizador consulta a tabela mas nunca escreve nela: params SEM update e SEM stats.
        // Um dispatch de tela cheia contando "queries" poluiria a taxa de acerto que o painel
        // mostra, que e sobre os RAIOS e nao sobre os pixels.
        //
        // E ConsumerRuns = FALSE, apesar de este passe rodar de fato. O registro descreve a
        // configuracao da IMAGEM CAPTURADA, e o visualizador escreve num alvo de debug que nao e o
        // backbuffer. Com o default (true), abrir a janela de debug bastava para o manifesto
        // declarar `cacheQuery: true` num frame em que nenhum trace de render consultou o cache.
        const FRadianceCacheShaderParams P = ShaderParams(/*AllowUpdate*/ false,
                                                          /*ConsumerRuns*/ false);
        D.RadianceCacheCamCell     = P.CameraPosCell;
        D.RadianceCacheLodCapFlags = P.LodCapacityFlags;
        D.RadianceCacheResources   = P.Resources;
        std::memcpy(MappedDebugCB +
                        static_cast<size_t>(InFrameSlot) * sizeof(RadianceCacheDebugConstants),
                    &D, sizeof(D));

        Transition(CL, DebugTex.Get(), DebugTexState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DebugPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, DebugCBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(DebugSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(DebugUAV));
        CL->Dispatch((DebugWidth + 7u) / 8u, (DebugHeight + 7u) / 8u, 1);
        Transition(CL, DebugTex.Get(), DebugTexState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void FRadianceCache::RecordStatsCopy(ID3D12GraphicsCommandList* CL) {
        if (!Ready || !CL || !CaptureStatsReadback) return;
        // A transicao para COPY_SOURCE tambem serve de barreira sobre a varredura do resolve, que
        // acabou de escrever estes contadores por UAV.
        Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CL->CopyBufferRegion(CaptureStatsReadback.Get(), 0, StatsBuf.Get(), 0, kStatBytes);
        Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CaptureStatsPending = true;
    }

    bool FRadianceCache::CollectCaptureStats(FRadianceCacheStats& Out) {
        if (!CaptureStatsPending || !CaptureStatsReadback) return false;
        CaptureStatsPending = false;

        void* Mapped = nullptr;
        D3D12_RANGE ReadRange{ 0, kStatBytes };
        if (FAILED(CaptureStatsReadback->Map(0, &ReadRange, &Mapped)) || !Mapped) return false;
        const u32* S = static_cast<const u32*>(Mapped);
        Out.Occupied   = S[0];
        Out.HasSamples = S[1];
        Out.Samples    = S[2];
        Out.Evicted    = S[3];
        Out.Confident  = S[4];
        // Query/Hits saem ZERO por construcao: o resolve zera os contadores logo antes da
        // varredura, e esta copia e posterior a ela. Quem quer o hit rate do frame le o StatsCPU,
        // que vem da copia PRE-resolve. Ver o comentario do RecordStatsCopy.
        //
        // O mesmo vale para os doze do detalhe — misses e insercao sao escritos antes do resolve —,
        // e por isso eles nem sao lidos aqui: quem monta o manifesto os pega do anel, junto do
        // hit rate, que e a metade da telemetria que essa copia nao ve.
        Out.Queries = 0;
        Out.Hits    = 0;
        D3D12_RANGE NoWrite{ 0, 0 };
        CaptureStatsReadback->Unmap(0, &NoWrite);
        return true;
    }

    void FRadianceCache::CollectStats(u32 InFrameSlot) {
        if (InFrameSlot >= FCommandQueue::kFramesInFlight ||
            !StatsReadbackPending[InFrameSlot] || !StatsReadback[InFrameSlot]) {
            return;
        }
        StatsReadbackPending[InFrameSlot] = false;

        void* Mapped = nullptr;
        D3D12_RANGE ReadRange{ 0, kStatBytes };
        if (FAILED(StatsReadback[InFrameSlot]->Map(0, &ReadRange, &Mapped)) || !Mapped) return;
        const u32* S = static_cast<const u32*>(Mapped);
        StatsCPU.Occupied   = S[0];
        StatsCPU.HasSamples = S[1];
        StatsCPU.Samples    = S[2];
        StatsCPU.Evicted    = S[3];
        StatsCPU.Confident  = S[4];
        StatsCPU.Queries    = S[5];
        StatsCPU.Hits       = S[6];
        // Zerados sem o regime de detalhe. Deixa-los assim e o correto: o painel distingue "zero
        // medido" de "nao medido" pelo proprio knob, como ja faz com o hit rate.
        StatsCPU.MissShort   = S[7];
        StatsCPU.MissCone    = S[8];
        StatsCPU.MissNoEntry = S[9];
        StatsCPU.MissEmpty   = S[10];
        StatsCPU.MissWarming = S[11];
        StatsCPU.MissStale   = S[12];
        StatsCPU.InsertTries = S[13];
        StatsCPU.InsertFull  = S[14];
        StatsCPU.Contended   = S[15];
        StatsCPU.Retries     = S[16];
        StatsCPU.ProbeSum    = S[17];
        StatsCPU.ProbeMax    = S[18];
        StatsCPU.Capped      = S[19];
        StatsCPU.Paths       = S[20];
        StatsCPU.PathVerts   = S[21];
        StatsCPU.PathDepth   = S[22];
        StatsCPU.TermSky     = S[23];
        StatsCPU.TermCache   = S[24];
        StatsCPU.TermKilled  = S[25];
        StatsCPU.TermMiss    = S[26];
        StatsCPU.TermNoQuery = S[27];
        StatsCPU.TermLobe    = S[28];
        StatsCPU.TermOther   = S[29];
        D3D12_RANGE NoWrite{ 0, 0 };
        StatsReadback[InFrameSlot]->Unmap(0, &NoWrite);
    }

    FRadianceCacheShaderParams FRadianceCache::ShaderParams(bool AllowUpdate,
                                                            bool ConsumerRuns) const {
        FRadianceCacheShaderParams P{};
        // Sem cena montada o shader nao pode nem consultar nem escrever: os indices bindless
        // apontariam para descritores livres.
        // Reset acontece no resolve, no fim do frame. Ate ele terminar, consultar devolveria uma
        // tabela do regime anterior (e atualizar seria trabalho descartado alguns passes depois).
        // O resolve continua ativo via IsActive; so os consumidores recebem flags zeradas.
        const bool On = Enabled && Ready && !ResetPending;
        // A consulta EFETIVA, e nao o knob: em Filling o render ainda nao le a tabela. Daqui para
        // baixo o `QueryEnabled` cru nao aparece mais — flags, contadores e registro do manifesto
        // saem todos deste booleano, senao um frame de aquecimento se declararia consultando.
        //
        // Pelo QueryEffective(), que e a mesma expressao que decide invalidar historico: se as
        // duas divergissem, a invalidacao estaria olhando uma coisa e os traces recebendo outra.
        // `Query` ja implica `On`, entao o aninhamento abaixo continua correto.
        const bool Query = QueryEffective();
        u32 Flags = 0u;
        if (On) {
            if (Query)       Flags |= kFlagQuery;
            if (AllowUpdate) Flags |= kFlagUpdate;
            // Registro do que foi ENTREGUE A QUEM RODA, para o manifesto da captura nao ter de
            // reconstituir esta condicao por fora e divergir dela. Ver PublishedUpdateThisFrame.
            if (ConsumerRuns) {
                PublishedUpdate = PublishedUpdate || AllowUpdate;
                PublishedQuery  = PublishedQuery  || Query;
                PublishedStats  = PublishedStats  || (StatsEnabled && Query);
                PublishedDetail = PublishedDetail || (StatsEnabled && StatsDetail);
                // So com consulta: sem ela o piso nao decide nada neste consumidor, e registra-lo
                // faria o manifesto declarar um controle que nao agiu sobre a imagem.
                if (Query) PublishedMinSamples = MinSampleCount;
            }
            // So conta acerto/erro quando ha o que contar: sem query o contador so mediria zero
            // e ainda assim pagaria os atomicos.
            if (StatsEnabled && Query) Flags |= kFlagStats;
            // O detalhe NAO exige QueryEnabled: metade dele (a telemetria de insercao) vive no
            // RC_Update, e o produtor legado escreve por este mesmo caminho. O que ele exige e a
            // instrumentacao ligada, para o regime ser um so e nao dois.
            if (StatsEnabled && StatsDetail) Flags |= kFlagStatsDetail;
        }
        P.CameraPosCell    = { CameraPos.X, CameraPos.Y, CameraPos.Z, BaseCellSize };
        P.LodCapacityFlags = { LodDistance, static_cast<f32>(CapacityV),
                               static_cast<f32>(Flags), static_cast<f32>(MinSampleCount) };
        P.Resources        = { static_cast<f32>(UavTable + 0),
                               static_cast<f32>(UavTable + 1),
                               static_cast<f32>(UavTable + 2),
                               static_cast<f32>(UavTable + 3) };
        return P;
    }
}
