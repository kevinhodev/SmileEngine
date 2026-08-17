#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FScene;
    class FTextureSRVHeap;

    // Espelham MeshLightCommon.hlsli. Os static_assert abaixo sao a unica coisa que impede um
    // lado mudar sem o outro e corromper o buffer em silencio.
    struct FMeshLightTaskGPU {
        u32  InstanceIndex;
        u32  TriangleCount;
        u32  LightOffset;
        u32  Pad;
        Vec4 Row0;   // 3 linhas da Mat44 TRANSPOSTA, mesma convencao do TLAS
        Vec4 Row1;
        Vec4 Row2;
    };
    static_assert(sizeof(FMeshLightTaskGPU) == 64);

    struct FTriangleLightGPU {
        Vec3 Base;
        u32  Edges0;
        u32  Edges1;
        u32  Edges2;
        u32  Radiance;
        f32  Flux;
    };
    static_assert(sizeof(FTriangleLightGPU) == 32);

    // Espelha FMeshLightAlias em MeshLightCommon.hlsli.
    struct FMeshLightAliasGPU {
        f32 Threshold;
        u32 Alias;
        f32 ProbSelf;
        f32 ProbAlias;
    };
    static_assert(sizeof(FMeshLightAliasGPU) == 16);

    struct alignas(256) MeshLightConstants {
        u32 NumTasks     = 0;
        u32 NumTriangles = 0;
        u32 Pad0         = 0;
        u32 Pad1         = 0;
    };

    // Levantamento dos triangulos emissivos da cena — primeira fase do projeto de mesh lights.
    //
    // Existe para MEDIR antes de escolher a estrategia de amostragem inicial, porque e a ordem de
    // grandeza da contagem que decide, e nao a intuicao:
    //   centenas          -> proposta uniforme ainda funciona
    //   milhares          -> alias table por potencia vira obrigatoria
    //   dezenas de milhar -> alias table + ReGIR consertado (o pool de 64 slots por celula colapsa
    //                        nessa faixa, ver a auditoria do ReGIR)
    //
    // O que NAO da para levantar aqui: area e fluxo por triangulo. As posicoes dos vertices vivem
    // so na GPU depois do upload (FGpuMesh nao guarda copia na CPU), entao area = |cross(e1,e2)|/2
    // e o fluxo = area*pi*luminancia so saem no passe de extracao. A contagem, que e o numero que
    // destrava a decisao, vem de FGpuMesh::GetIndexCount() e esta disponivel de graca.
    class FMeshLights : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) -------------------------------------------
        // O stem "MeshLightExtract.cs" nao estava na tabela do ReloadShaders, apesar de o
        // RecreatePSO abaixo existir e ja ser chamado pelo RecreateAllPSOs: editar o shader
        // logava "sem pipeline mapeado". Declarado aqui, os dois caminhos passam a concordar
        // por construcao.
        const char* Name() const override { return "MeshLights (extract)"; }
        bool IsInitialized() const override { return Ready; }
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        FPassShaderStems ShaderStems() const override;

        struct FStats {
            u32 EmissiveMeshes      = 0; // renderables com material emissivo
            u32 EmissiveTriangles   = 0; // total de triangulos emissivos (o numero que decide)
            u32 MeshesWithMap       = 0; // precisam amostrar textura emissiva na extracao
            u32 TrianglesWithMap    = 0;
            u32 LargestMeshTris     = 0; // maior contribuinte isolado
            u32 TotalRenderables    = 0;
            u32 TotalTriangles      = 0;
        };

        // Telemetria da DISTRIBUICAO, para o manifesto da Fase 0 (MESH-LIGHTS-PLAN.md §5).
        //
        // Sai de graca: o BuildAliasTable ja percorre os N triangulos na CPU para somar o fluxo, e
        // estes contadores sao decididos no mesmo laco. Nao ha atomico, nao ha passe extra e nao ha
        // regime de medicao separado — que e justamente o motivo de eles entrarem AGORA enquanto os
        // contadores por pixel/candidata do plano ficam para depois da baseline limpa.
        //
        // A separacao entre Degenerate e ZeroFlux importa porque as duas causas pedem acoes
        // opostas: area nula e defeito de GEOMETRIA (malha ruim, transform degenerada) enquanto
        // fluxo zero com area valida e CONTEUDO (RTEmissiveScale zerado, mapa emissivo preto) e
        // pode ser exatamente o que o artista quis. Somadas num numero so, uma esconderia a outra.
        struct FDistributionStats {
            f64  TotalFlux  = 0.0; // soma do fluxo por triangulo; o denominador da alias table
            u32  Triangles  = 0;   // N considerado na tabela
            u32  ZeroFlux   = 0;   // area valida, fluxo 0 -> radiancia zero
            // Mesmo limiar do DI_SampleTriangleLight (crLen < 1e-12): o contador conta os
            // triangulos que o SHADER tambem rejeita, e nao uma nocao paralela de degenerado.
            u32  Degenerate = 0;
            u32  NonFinite  = 0;   // NaN/Inf no fluxo — corrupcao, nao conteudo
            // Fluxo total zero derruba a tabela para uniforme (ver BuildAliasTable). O manifesto
            // precisa disto porque nesse regime a etiqueta "por potencia" deixa de ser verdade.
            bool UniformFallback = false;
        };

        // Percorre a cena e preenche as estatisticas. Barato: so le material e contagem de indices,
        // sem tocar em GPU. Chamado por cena.
        void Survey(const FScene& Scene);

        // Emite o resumo no log, com a leitura do que a contagem implica para a amostragem.
        void LogSummary() const;

        const FStats& Stats() const { return SceneStats; }
        u32  EmissiveTriangleCount() const { return SceneStats.EmissiveTriangles; }
        bool HasEmissiveGeometry()   const { return SceneStats.EmissiveTriangles > 0; }

        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        // Faz o Survey, monta as tasks e cria os buffers. Precisa do SRV do InstanceGeo
        // (FRaytracingScene::InstanceGeoSRV), onde moram VB/IB bindless e o material emissivo.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene,
                           u32 InstanceSlot);

        // Dispara a extracao SO quando sujo. A geometria emissiva do projeto e estatica, entao o
        // caso comum e nao fazer nada — ao contrario do RTXDI, que reconstroi tudo todo frame.
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Marcar sujo quando transform, material emissivo ou o conjunto de renderables mudar.
        void MarkDirty() { Dirty = true; }

        bool IsReady()          const { return Ready; }
        // O buffer que o Pass A e o Pass B AMOSTRAM. E sempre a copia compacta em VRAM: quando a
        // compactacao esta desligada ela recebe os mesmos N triangulos do original, entao o
        // consumidor nao precisa saber qual dos dois regimes esta valendo — o que muda e a
        // CONTAGEM publicada pelo LightCount, nao o descritor.
        u32  LightSRVSlot()     const { return CompactLightsSRV; }
        // O descritor que o Pass A AMOSTRA — a copia em VRAM, nao o staging em upload. Ver o bloco
        // de RESIDENCIA abaixo.
        u32  AliasSRVSlot()     const { return AliasDefaultSRV; }
        // Contagem que o DI deve usar como pool: 0 enquanto a alias table nao estiver pronta E
        // copiada para a VRAM. Sem a tabela, a proposta seria uniforme sobre dezenas de milhares de
        // triangulos, que e pior que nao ter mesh light nenhuma; sem a copia, o Pass A leria um
        // buffer nao inicializado.
        //
        // Gatear a CONTAGEM, e nao trocar descritor, e o que dispensa qualquer caminho de fallback:
        // "ainda nao copiei" fica indistinguivel de "nao ha mesh light neste frame", que e um
        // estado que o DI ja sabia tratar.
        u32  LightCount()       const { return DomainPublished ? NumSamplable : 0u; }

        // --- PUBLICACAO DO DOMINIO -----------------------------------------------------------
        // O dominio amostrado muda quando a tabela e reconstruida — e com a compactacao ele muda de
        // TAMANHO, o que faz o mesmo indice de reservoir passar a apontar para OUTRO triangulo.
        // Publicar sem limpar o historico serviria reservoirs do dominio antigo contra o novo.
        //
        // Limpar no PEDIDO nao resolve: entre o pedido e a tabela nova existem varios frames
        // (extracao, readback diferido, construcao, copia), e o historico limpo no pedido volta a
        // encher sob o dominio ANTIGO nesse intervalo. Por isso a publicacao e explicita: o Record
        // deixa pendente, o Renderer consome, limpa o historico, e so entao o LightCount abre.
        // Custa um frame vendo pool vazio, que e um estado que o DI ja sabia tratar.
        bool ConsumeDomainPublish() {
            if (!PendingDomainPublish) return false;
            PendingDomainPublish = false;
            DomainPublished      = true;
            return true;
        }

        // --- Telemetria da Fase 0 ------------------------------------------------------------
        // LEVANTADA contra AMOSTRAVEL. Sao numeros diferentes e a diferenca e o diagnostico: o
        // primeiro e o que a cena tem, o segundo e o que a distribuicao consegue propor NESTE
        // frame. Eles divergem durante os frames de readback e sempre que o fluxo total for zero.
        // Um manifesto com so o primeiro afirmaria mesh lights participando de um frame em que o
        // DI viu pool vazio.
        u32  ExtractedTriangleCount() const { return NumTriangles; }
        u32  SamplableTriangleCount() const { return LightCount(); }
        bool IsAliasReady()           const { return AliasReady; }
        // Zerada enquanto a tabela esta sendo reconstruida — ver o Record. Publicar a distribuicao
        // ANTERIOR durante o rebuild faria o arquivo descrever uma tabela que nao estava em uso.
        const FDistributionStats& Distribution() const { return DistStats; }

        // --- ORCAMENTO DE MEMORIA, por recurso -----------------------------------------------
        // ⚠️ Sao SEIS buffers, nao dois, e a conta so fecha se todos aparecerem. Um campo unico de
        // "bytes de triangulo" reportava 7,3 MB na Emerald quando a VRAM tinha 14,6 MB: existem
        // DOIS buffers DEFAULT de triangulo — o que a extracao escreve e a copia que o Pass A
        // amostra — mais o staging de upload e o readback.
        //
        // A duplicata em VRAM e o preco de o descritor amostrado nunca mudar: alternar entre os
        // dois exigiria recriar SRV e drenar as filas, porque as tabelas de trace do DI guardam
        // copia do descritor. E uma troca deliberada, e por isso precisa estar visivel.
        //
        // ⚠️ PAYLOAD e nao ALOCACAO. Estes acessores devolvem a LARGURA LOGICA do buffer
        // (elementos x stride). Recurso D3D12 tem alinhamento de alocacao — 64 KiB para buffer —,
        // entao um pool de um triangulo tem payload de 32 B e ocupa 64 KiB. Chamar o payload de
        // "memoria alocada" engana, e engana MAIS em cena pequena, onde o alinhamento domina.
        // O numero com alinhamento e o VramBytes abaixo.
        u64  TrianglePayloadBytes() const;   // DEFAULT, saida da extracao
        u64  TriangleCompactPayloadBytes() const;  // DEFAULT, a copia que o Pass A amostra
        u64  TriangleStagingPayloadBytes() const;  // UPLOAD, staging da compactacao
        u64  TriangleReadbackPayloadBytes() const; // READBACK, de onde a CPU le o fluxo

        // DEFINICAO EXATA, e ela importa: soma dos FOOTPRINTS ALINHADOS ATRIBUIVEIS aos recursos
        // que moram em VRAM — os dois DEFAULT de triangulo mais a alias em DEFAULT. Staging e
        // readback ficam de fora porque moram em system memory.
        //
        // Para buffer o alinhamento e deterministico (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        // 64 KiB), entao nao e preciso guardar o GetResourceAllocationInfo de cada recurso.
        //
        // ⚠️ ISTO NAO E O TOTAL DE HEAPS COMPROMETIDOS. O D3D12MA suballoca dentro de blocos muito
        // maiores que estes recursos, entao o numero daqui:
        //   - NAO mede quanto heap o alocador pediu ao driver — esse total e de OUTRA ordem e nao
        //     se obtem somando recursos;
        //   - SUPERESTIMA o custo marginal quando dois destes caem no mesmo bloco, porque o
        //     alinhamento de 64 KiB e cobrado uma vez por recurso nesta conta e nem sempre na
        //     realidade.
        // E uma ATRIBUICAO por recurso, util para orcamento comparativo entre fases do plano. Quem
        // responde "quanta VRAM a engine comprometeu" e o VramTracker, por categoria.
        u64  VramBytes() const;
        // Heap de UPLOAD: nao aparece no VramTracker (ele ignora upload/readback, que moram em
        // system memory). Entra no manifesto assim mesmo porque o custo existe e o plano pede o
        // orcamento por recurso — so nao e VRAM. PAYLOAD, ver a nota acima.
        u64  AliasUploadPayloadBytes()  const;
        // Copia em DEFAULT heap — esta SIM e VRAM. Separada da de cima no manifesto porque a
        // residencia foi o achado do §2.7. PAYLOAD, ver a nota acima.
        u64  AliasDefaultPayloadBytes() const;

        // --- COMPACTACAO PARA O SUPORTE POSITIVO ---------------------------------------------
        // 85,5% dos triangulos emissivos da Emerald tem fluxo ZERO (§2.4): area valida, radiancia
        // nula. A alias table lhes da probabilidade zero, entao eles nunca sao sorteados — mas
        // ocupam o pool inteiro, e cada candidata paga isso em localidade.
        //
        // A compactacao acontece na CPU, depois do readback, e NAO no shader: o MeshLightExtract
        // escreve em `OutLights[index]` com o indice GLOBAL do triangulo, entao filtrar la exigiria
        // append com contador atomico. No caminho estatico deste projeto — extracao so quando sujo
        // — a CPU ja tem os dados na mao e ja percorre todos eles para somar o fluxo.
        //
        // O que muda: `NumSamplable` <= NumTriangles vira o dominio da alias e do buffer que o Pass
        // A amostra. O indice do reservoir passa a ser no dominio COMPACTO, e o mapa de volta nao e
        // necessario porque o triangulo compacto carrega os mesmos 32 bytes.
        void SetCompactSupport(bool V) { CompactRequested = V; }
        bool GetCompactSupport() const { return CompactRequested; }
        // EFETIVO exige que o pool esteja de fato PUBLICADO, e nao so que a ultima construcao tenha
        // compactado. Sem esse gate, durante um rebuild o manifesto podia sair `requested: false`
        // com `effective: true` e a etiqueta `-cmp`, descrevendo um regime que ninguem amostrou —
        // o `CompactApplied` sobrevive da tabela anterior enquanto a nova nao fica pronta.
        bool IsCompactSupportEffective() const { return CompactApplied && LightCount() > 0u; }
        // Tamanho do dominio que o DI amostra. Igual a NumTriangles sem compactacao.
        u32  SamplableDomainSize() const { return NumSamplable; }
        // Tamanho do DOMINIO em bytes — dominio x stride. NAO e "bytes tocados": nao ha instrumento
        // de trafego aqui, e o numero sai igual com o DI desligado, com zero candidatas ou com a
        // tabela ainda em construcao. E o tamanho do conjunto sobre o qual o sorteio ACONTECERIA,
        // que e o que muda com a compactacao e o que se compara contra o tamanho do cache.
        u64  AliasDomainBytes()    const {
            return static_cast<u64>(NumSamplable) * sizeof(FMeshLightAliasGPU);
        }
        u64  TriangleDomainBytes() const {
            return static_cast<u64>(NumSamplable) * sizeof(FTriangleLightGPU);
        }

        // --- RESIDENCIA da alias table -------------------------------------------------------
        // A alias e lida uma vez POR CANDIDATA dentro do Pass A: na Emerald com 8 candidatas sao
        // ~10 milhoes de leituras aleatorias de 16 B por frame. Num UPLOAD heap elas atendem em
        // system memory, pelo caminho PCIe; num DEFAULT heap, em VRAM.
        //
        // MEDIDO (§2.7 do MESH-LIGHTS-PLAN.md): 31,09 -> 3,77 ms em `Amostragem + temporal` na
        // Emerald, imagem identica ao piso de ruido. O experimento mostra que a RESIDENCIA domina;
        // ele nao separa cache, latencia e largura de banda, que continuam agregados.
        //
        // O upload heap continua existindo como STAGING — e nele que a CPU escreve a tabela de
        // Vose —, mas ninguem o amostra. O A/B que permitia amostra-lo foi removido depois que a
        // decisao fechou: um toggle cujo unico valor correto e um so nao e configuracao, e mante-lo
        // custava um caminho de fallback, um reconcile com drain de fila e dois campos de manifesto.

        void Release(FTextureSRVHeap& SRVHeap);

    private:
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FStats SceneStats{};
        FDistributionStats DistStats{};

        FComputePipeline ExtractPSO;   // 2 SRV, 1 UAV, bindless
        bool Initialized = false;
        bool Ready       = false;
        bool Dirty       = false;

        u32 NumTasks     = 0;
        u32 NumTriangles = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> TaskBuffer;   // upload; escrito uma vez por cena
        Microsoft::WRL::ComPtr<ID3D12Resource> LightBuffer;  // default; saida da extracao
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer; // le o fluxo de volta p/ a CPU
        Microsoft::WRL::ComPtr<ID3D12Resource> AliasBuffer;    // upload; tabela de Vose
        // Copia em DEFAULT heap do MESMO vetor de alias. Existe sempre, mesmo com o toggle
        // desligado, para o A/B nao mexer em pressao de memoria junto com localizacao.
        Microsoft::WRL::ComPtr<ID3D12Resource> AliasDefaultBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedCB    = nullptr;
        u8* MappedAlias = nullptr;

        D3D12_RESOURCE_STATES LightState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AliasDefaultState = D3D12_RESOURCE_STATE_COMMON;

        // Readback DIFERIDO: em vez de travar a fila, conta frames ate a copia estar garantida.
        // So funciona porque a geometria emissiva e estatica — com emissivo dinamico isto vira
        // lag por frame e o caminho certo passa a ser o mipmap de PDF na GPU (ver a memoria).
        bool AliasReady      = false;
        bool ReadbackPending = false;
        u32  ReadbackAge     = 0;

        // `Copied` gateia o LightCount: ate a copia para a VRAM ter sido gravada, o DI ve pool
        // vazio em vez de ler buffer nao inicializado.
        bool AliasDefaultCopied    = false;
        bool AliasCopyPending      = false;

        // Compactacao para o suporte positivo. `NumSamplable` e o dominio que o DI amostra: igual
        // a NumTriangles quando desligada, e a contagem de fluxo > 0 quando aplicada.
        bool CompactRequested = true;
        bool CompactApplied   = false;
        u32  NumSamplable     = 0;
        // Ver o bloco de PUBLICACAO DO DOMINIO acima.
        bool PendingDomainPublish = false;
        bool DomainPublished      = false;
        // Copia COMPACTA dos triangulos, em DEFAULT heap. O buffer original continua existindo —
        // e ele que o extract escreve e o readback le —, mas o Pass A passa a amostrar este.
        Microsoft::WRL::ComPtr<ID3D12Resource> CompactLightBuffer;
        u32 CompactLightsSRV = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES CompactLightState = D3D12_RESOURCE_STATE_COMMON;
        // Staging em upload para subir os triangulos compactados sem passar por command list de
        // copia propria — mesmo padrao da alias.
        Microsoft::WRL::ComPtr<ID3D12Resource> CompactUploadBuffer;
        u8* MappedCompact = nullptr;
        bool CompactCopyPending = false;

        u32 TaskSRV   = 0xFFFFFFFFu;
        u32 LightsSRV = 0xFFFFFFFFu;
        u32 LightsUAV = 0xFFFFFFFFu;
        u32 AliasSRV  = 0xFFFFFFFFu;
        u32 AliasDefaultSRV = 0xFFFFFFFFu;
        u32 ExtractTable = 0xFFFFFFFFu; // t0 = tasks, t1 = instances

        void BuildAliasTable();
    };
}
