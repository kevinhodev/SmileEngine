#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include "Smile/Graphics/CommandQueue.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Contrato publicado nos CBs dos consumidores do HitShading.hlsli, no mesmo padrao do
    // FReGIRShaderParams: linhas Vec4 que o shader desempacota em FRadianceCacheParams.
    // A documentacao do algoritmo (e as fontes) esta em Shaders/GI/RadianceCache.hlsli.
    struct FRadianceCacheShaderParams {
        Vec4 CameraPosCell;    // xyz = camera; w = aresta da celula no nivel 0, em metros
        Vec4 LodCapacityFlags; // x = distancia do LOD; y = capacidade; z = flags; w = livre
        Vec4 Resources;        // x = Entries UAV; y = Accum UAV; z = Resolved UAV; w = Stats UAV
    };

    struct alignas(256) RadianceCacheConstants {
        Vec4 CacheParams; // x = capacidade; y = maxAccumSamples; z = staleFrameMax; w = reset
        Vec4 StatsParams; // x = modo do dispatch (0 resolve, 1 clear dos contadores)
    };

    struct alignas(256) RadianceCacheDebugConstants {
        Mat44 InvViewProj;
        Vec4  CameraPosMode;  // xyz = camera; w = modo (ver ERadianceCacheDebugMode)
        Vec4  ScreenParams;   // x = W; y = H; z = 1/W; w = 1/H
        Vec4  RadianceCacheCamCell;
        Vec4  RadianceCacheLodCapFlags;
        Vec4  RadianceCacheResources;
    };

    // Espelham os RC_DEBUG_* do RadianceCacheDebug.cs.hlsl.
    enum class ERadianceCacheDebugMode : u32 {
        Cells = 0,   // cor por celula — a estrutura do hash no mundo
        HitMiss,     // cobertura: verde = utilizavel; laranja = refresh; amarelo = sem amostra
        Lod,         // nivel da celula por distancia da camera
        Converged,   // N/kMaxAccumSamples
        Count
    };

    // O que o painel mostra. Os quatro primeiros vem da varredura da tabela no resolve; os dois
    // ultimos, do RC_Query dentro dos traces (so quando a instrumentacao esta ligada).
    struct FRadianceCacheStats {
        u32 Occupied = 0;  // celulas com chave valida
        u32 Valid    = 0;  // dessas, quantas ja tem radiancia utilizavel
        u32 Samples  = 0;  // soma de N; / Valid = convergencia media
        u32 Evicted  = 0;
        u32 Queries  = 0;
        u32 Hits     = 0;
    };

    // World Radiance Cache — radiancia de SAIDA em espaco de mundo, hash espacial
    // multirresolucao [Gautron2020] / [Sousa2025] / SHaRC.
    //
    // NAO substitui o DDGI. O atlas de irradiancia continua sendo o que o deferred le por pixel;
    // este cache e o terminador de raio secundario, consultado dentro do ShadeSurfaceHit — o
    // funil unico por onde passam DDGI, ReSTIR GI e reflexoes.
    class FRadianceCache final : public FRenderPass {
    public:
        static constexpr const char* kDebugTargetName = "Radiance cache · ocupacao";

        // --- contrato de passe (RenderPass.h) ---------------------------------------------
        const char* Name() const override { return "Radiance cache"; }
        bool IsInitialized() const override { return Initialized && Ready; }
        bool CanVisualize() const { return IsInitialized() && !ResetPending; }
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        FPassShaderStems ShaderStems() const override;
        bool IsActive(const FFrameModes& Modes) const override;

        // O cache E um acumulador temporal: media movel com teto de kMaxAccumSamples. Sem entrar
        // no grafo, um knob que muda o que o raio enxerga deixaria radiancia velha viva por ate
        // kStaleFrameMax frames — o mesmo "historico que nao morre" que o HistoryDomain.h
        // descreve. O OnResize existe so pelo alvo do VISUALIZADOR; a tabela em si e de MUNDO e
        // resolucao de tela nao a toca.
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::RadianceCache; }
        void OnInvalidateHistory(EHistoryTarget) override { ResetOnce(); }

        // Espelham RC_FLAG_* do RadianceCache.hlsli.
        static constexpr u32 kFlagQuery  = 1u;
        static constexpr u32 kFlagUpdate = 2u;
        static constexpr u32 kFlagStats  = 4u;

        // Espelham RC_SAMPLE_NUM_MAX / RC_STALE_FRAME_MAX do shader. Mudar aqui sem mudar la
        // deixa o teto de amostras incoerente com o teto do ponto fixo do acumulador.
        static constexpr u32 kMaxAccumSamples = 64;
        static constexpr u32 kStaleFrameMax   = 64;

        // 2^20 = ~1,05 M celulas = 36 MB (4 + 16 + 16). A SHaRC sugere 2^22 (160 MB) para Night
        // City; as cenas da Smile sao uma ordem de grandeza menores em extensao, e a categoria
        // "GI e reflexos" do VramTracker ja e a segunda maior da engine.
        static constexpr u32 kDefaultCapacityLog2 = 20;
        static constexpr u32 kMaxCapacityLog2     = 22;

        void Initialize(ID3D12Device* Device);

        // Capacidade e potencia de 2 porque o modulo do hash vira mascara (Capacity - 1).
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           u32 CapacityLog2 = kDefaultCapacityLog2);
        void Release(FTextureSRVHeap& SRVHeap);

        void UpdatePerFrame(u32 InFrameSlot, const Vec3& InCameraPos);
        void RecordResolve(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Visualizador. Alvo em resolucao de render, publicado no DebugTargets pelo nome
        // "Radiance cache · ocupacao" — a janela de debug o acha pela busca, como os demais.
        // O OnResize (re)cria a textura e as views; a publicacao e separada porque o registro do
        // visualizador e reconstruido do zero pelo Renderer e so aceita entrada na hora dele.
        void OnResize(const FPassInitContext& Ctx) override;
        void OnRegisterDebugTargets() override;
        void RecordDebug(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                         const Mat44& InvViewProj, const Vec3& CameraPos, u32 FrameSlot);

        // Le o readback do slot (ja esperado pela fence do BeginFrame). Chamar no inicio do frame,
        // junto dos outros Collect* do Renderer.
        void CollectStats(u32 InFrameSlot);
        const FRadianceCacheStats& Stats() const { return StatsCPU; }

        void SetDebugMode(ERadianceCacheDebugMode M) { DebugMode = M; }
        ERadianceCacheDebugMode GetDebugMode() const { return DebugMode; }
        // Instrumentacao de acerto/erro do query. Custa dois atomicos disputados por wave em todo
        // trace do frame, entao e knob proprio e nao vem junto do Enabled.
        void SetStatsEnabled(bool V) { StatsEnabled = V; }
        bool GetStatsEnabled() const { return StatsEnabled; }

        // Os passes de trace escrevem por atomico bindless e leem o Resolved; ambos precisam do
        // recurso em UNORDERED_ACCESS / NON_PIXEL. Chamar antes de gravar os traces do frame.
        void TransitionForTrace(ID3D12GraphicsCommandList* CL);

        // Limpa a tabela inteira no proximo resolve. Necessario quando a cena troca ou a camera
        // teleporta: o conteudo descreve outro lugar do mundo e envelhecer celula a celula
        // levaria kStaleFrameMax frames de fantasma.
        void ResetOnce() { ResetPending = true; }

        u32  Capacity() const { return CapacityV; }
        u64  MemoryBytes() const;

        void SetEnabled(bool V)       { Enabled = V; }
        bool GetEnabled() const       { return Enabled; }
        // Query e update sao knobs separados de proposito: o A/B util e ligar o update, deixar o
        // cache encher por alguns segundos e SO entao ligar a leitura. Ligando os dois de uma vez
        // os primeiros frames leem uma tabela vazia e o resultado parece um bug.
        void SetQueryEnabled(bool V)  { QueryEnabled = V; }
        bool GetQueryEnabled() const  { return QueryEnabled; }

        // Aresta da celula no nivel 0. 0,25 m e o valor do idTech 8.
        void SetBaseCellSize(f32 V)   { BaseCellSize = V < 0.01f ? 0.01f : V; }
        f32  GetBaseCellSize() const  { return BaseCellSize; }
        // Distancia em que o nivel sobe de 0 para 1 (e depois dobra). Controla quao rapido as
        // celulas crescem com o afastamento da camera.
        void SetLodDistance(f32 V)    { LodDistance = V < 0.1f ? 0.1f : V; }
        f32  GetLodDistance() const   { return LodDistance; }

        // Update = false para consumidor DIRECIONAL (reflexoes): a radiancia que ele produz vale
        // para uma direcao de espelho e o cache nao guarda direcao. Ele ainda pode CONSULTAR.
        FRadianceCacheShaderParams ShaderParams(bool AllowUpdate) const;

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        // Variant 0 = resolve, 1 = clear dos contadores. Duas entradas por frame em voo porque os
        // dois dispatches acontecem no MESMO frame e precisam de StatsParams.x diferentes — um CB
        // por slot so daria um valor.
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr(u32 Variant) const;
        D3D12_GPU_VIRTUAL_ADDRESS DebugCBAddr() const;

        void ReleaseDebug(FTextureSRVHeap& SRVHeap);

        FVolumetricPipeline ResolvePSO;
        FVolumetricPipeline DebugPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> Entries;  // checksum por celula (0 = vazia)
        Microsoft::WRL::ComPtr<ID3D12Resource> Accum;    // 4 uints/celula: rgb ponto fixo + N
        Microsoft::WRL::ComPtr<ID3D12Resource> Resolved; // uint4/celula: asuint(rgb) + N|frames
        Microsoft::WRL::ComPtr<ID3D12Resource> StatsBuf; // RC_STAT_COUNT uints

        D3D12_RESOURCE_STATES EntriesState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AccumState    = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES StatsState    = D3D12_RESOURCE_STATE_COMMON;

        // Anel de readback dos contadores, no molde do PointDiagnostic do FDDGIDebug: grava no
        // slot do frame e le kFramesInFlight frames depois, quando a fence do BeginFrame ja
        // garantiu que a copia terminou. Sem o anel, ler no mesmo frame seria um stall da fila.
        Microsoft::WRL::ComPtr<ID3D12Resource> StatsReadback[FCommandQueue::kFramesInFlight];
        bool StatsReadbackPending[FCommandQueue::kFramesInFlight] = {};
        FRadianceCacheStats StatsCPU{};

        // Alvo do visualizador (resolucao de render) + o CB proprio dele.
        Microsoft::WRL::ComPtr<ID3D12Resource> DebugTex;
        Microsoft::WRL::ComPtr<ID3D12Resource> DebugCB;
        u8* MappedDebugCB = nullptr;
        D3D12_RESOURCE_STATES DebugTexState = D3D12_RESOURCE_STATE_COMMON;
        u32 DebugSRV      = kInvalidSlot;
        u32 DebugUAV      = kInvalidSlot;
        u32 DebugSrvTable = kInvalidSlot; // [depth, normal] contiguos
        u32 DebugWidth = 0, DebugHeight = 0;
        ERadianceCacheDebugMode DebugMode = ERadianceCacheDebugMode::Cells;
        bool StatsEnabled = false;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        // Tabela de 3 UAVs CONTIGUOS (o range do root param 2 e um so). Os indices bindless
        // publicados no ShaderParams saem dela: base+0 Entries, base+1 Accum, base+2 Resolved.
        u32 UavTable = kInvalidSlot;
        // Existe SO para dar um destino valido ao root param 1 do ResolvePSO: um range de SRV com
        // 0 descritores nao serializa em D3D12, entao o root signature declara 1 e o shader nao le
        // nenhum. Nao faz parte do caminho de dados do cache.
        u32 BindingSRV = kInvalidSlot;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        RadianceCacheConstants CPU{};
        u32 FrameSlot = 0;

        Vec3 CameraPos{ 0.0f, 0.0f, 0.0f };
        u32  CapacityV    = 0;
        // Ponto de operacao medido no Bistro em 2026-08-08: 22,8% de ocupacao. O par antigo
        // 0,25 m / 12 m saturava 95,2% da tabela e fazia qualquer medida de custo perder valor.
        f32  BaseCellSize = 0.50f;
        f32  LodDistance  = 6.0f;
        bool Enabled      = false; // opt-in ate fechar o A/B visual do segundo bounce
        bool QueryEnabled = false;
        bool ResetPending = false;
        bool Initialized  = false;
        bool Ready        = false;
    };
}
