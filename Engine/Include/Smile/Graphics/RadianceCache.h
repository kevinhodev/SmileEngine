#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include "Smile/Graphics/CommandQueue.h"
// Insumos do passe dedicado de update (Fase 3). Os quatro sao headers de contrato, sem
// dependencia de volta para este — nenhum ciclo. O DDGI.h, que TEM ciclo (ele inclui este
// arquivo), fica de fora de proposito: ver a nota na cauda de cascatas do
// RadianceCacheUpdateConstants.
#include "Smile/Graphics/GIFallback.h"
#include "Smile/Graphics/GIHitSampling.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/ReGIR.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace Smile {
    class FTextureSRVHeap;

    // Contrato publicado nos CBs dos consumidores do HitShading.hlsli, no mesmo padrao do
    // FReGIRShaderParams: linhas Vec4 que o shader desempacota em FRadianceCacheParams.
    // A documentacao do algoritmo (e as fontes) esta em Shaders/GI/RadianceCache.hlsli.
    struct FRadianceCacheShaderParams {
        Vec4 CameraPosCell;    // xyz = camera; w = aresta da celula no nivel 0, em metros
        // w = PISO DE CONFIANCA (amostras minimas para a celula encerrar um caminho). Ele coube no
        // campo que sobrava desta linha de proposito: os cinco traces, o visualizador e o passe de
        // update ja declaram estes tres Vec4 no b0 deles, entao um piso configuravel nao custou
        // mudanca de layout de cbuffer em lugar nenhum.
        Vec4 LodCapacityFlags; // x = distancia do LOD; y = capacidade; z = flags; w = piso
        Vec4 Resources;        // x = Entries UAV; y = Accum UAV; z = Resolved UAV; w = Stats UAV
    };

    struct alignas(256) RadianceCacheConstants {
        Vec4 CacheParams; // x = capacidade; y = maxAccumSamples; z = staleFrameMax; w = reset
        // y = piso de confianca. O resolve conta as celulas CONFIAVEIS separadas das que so tem
        // amostra, e para isso precisa do mesmo numero que a consulta usa.
        Vec4 StatsParams; // x = modo do dispatch (0 resolve, 1 clear dos contadores); y = piso
    };

    // b0 do passe dedicado de update (Fase 3). Casa campo-a-campo com o cbuffer
    // RadianceCacheUpdateCB de Shaders/GI/RadianceCacheUpdate.cs.hlsl.
    //
    // Boa parte do bloco existe pelo CONTRATO DO HitShading.hlsli, que e por NOME: o header
    // declara o gather de fallback e os cinco traces que o incluem tem de trazer os campos dele no
    // b0 para o arquivo compilar. Este passe nao chama esse gather (e a ausencia de chamador e o
    // que garante "o update nunca le DDGI"), entao a cauda de cascatas viaja zerada.
    struct alignas(256) RadianceCacheUpdateConstants {
        Mat44 InvViewProj;
        Vec4  CameraPos;
        Vec4  ScreenParams;    // W, H, 1/W, 1/H
        Vec4  GridMinSpacing;  // DDGI grosso — contrato, nao consumido
        Vec4  GridCount;
        Vec4  AtlasParams;
        Vec4  SunDirIntensity;
        Vec4  SunColor;        // w = mask dos shadow rays (a MESMA do ReSTIR GI; ver RecordUpdate)
        Vec4  TraceParams;     // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
        Vec4  ShadeParams;     // x=nº de luzes puntuais, y=albedoLOD
        Vec4  PolicyParams;    // x = politica de backface (o MESMO toggle do ReSTIR GI)
        Vec4  UpdateParams;    // x=celulas/frame do tile 5x5, y=vertices, z=terminal no cache
        Vec4  RayEpsA;
        Vec4  RayEpsB;
        Vec4  GIDistParams;
        Vec4  GIBiasParams;    // .w = piso de roughness do secundario (este SIM e lido)
        Vec4  ReGIRGridMinSlots;
        Vec4  ReGIRInvCellEnabled;
        Vec4  ReGIRGridCountSamples;
        Vec4  ReGIRResources;
        Vec4  SkyParams;
        Vec4  RadianceCacheCamCell;
        Vec4  RadianceCacheLodCapFlags;
        Vec4  RadianceCacheResources;
        // Espelha o FDDGICascadeConstants campo a campo, sem SER ele: DDGI.h inclui este header,
        // entao incluir DDGI.h aqui fecharia um ciclo. Quem precisar preencher isto de verdade
        // (o UseDDGIBootstrap de diagnostico do plano) tira a struct do DDGI.h para um header
        // proprio primeiro — em change set separado, porque e refactor e nao estimator.
        Vec4  GICascadeParams;
        Vec4  GICascadeGridMinSpacing[4];
        Vec4  GICascadeScrollOffset[4];
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
        Converged,   // N/kMaxAccumSamples — quanto a MEDIA convergiu
        Age,         // frames desde a ultima amostra, com o limiar de refresh como cor
        // N contra o PISO, e nao contra o teto: a fronteira que decide se a celula encerra um
        // caminho. E o modo que mostra a frente de aquecimento andando depois de um reset — o
        // mesmo sinal que o estado global Filling le em forma de numero.
        Confidence,
        Count
    };

    // Aquecimento GLOBAL da tabela (Fase 4). O piso de confianca decide CELULA A CELULA; isto
    // decide pelo cache INTEIRO, e responde outra pergunta: "ja vale a pena o render consultar?".
    // Sem ele, o primeiro frame depois de uma troca de cena abre a consulta numa tabela vazia —
    // todo raio paga a busca, erra, e cai no fallback assim mesmo.
    enum class ERadianceCacheWarmup : u32 {
        // Tabela sendo limpa. NAO e comportamento novo: e o `ResetPending`, que ja fechava os dois
        // lados. Ele entra no enum porque o painel e o manifesto precisam distinguir "fechado
        // porque esta resetando" de "fechado porque esta enchendo" — sao esperas de duracao
        // completamente diferente (um frame contra dezenas).
        Resetting = 0,
        // O produtor enche. O RENDER ainda nao consulta; o TERMINAL DO UPDATER sim, e e isso que
        // faz o cache encher com multi-bounce em vez de so o primeiro bounce. Fechar os dois
        // deixaria Filling produzindo um conteudo que Active teria de reconverter.
        Filling,
        Active
    };

    // O que o painel mostra. Os cinco primeiros vem da varredura da tabela no resolve; os dois
    // seguintes, do RC_Query dentro dos traces (so com a instrumentacao ligada); o resto, do
    // regime de DETALHE (ver kFlagStatsDetail).
    struct FRadianceCacheStats {
        u32 Occupied   = 0; // celulas com chave valida
        // TEM AMOSTRA nao e CONFIAVEL, e o nome antigo (`Valid`) dizia a segunda coisa. Entre 1
        // amostra e o piso a celula existe, tem radiancia, e a consulta ainda assim devolve
        // WARMING. Sao dois numeros; o segundo e o que descreve o cache utilizavel, e e ele que o
        // futuro Filling/Active vai ler.
        u32 HasSamples = 0; // celulas com N >= 1
        u32 Confident  = 0; // celulas com N >= piso de confianca
        u32 Samples    = 0; // soma de N; / HasSamples = convergencia media
        u32 Evicted    = 0;
        u32 Queries    = 0;
        u32 Hits       = 0;
        // Misses por MOTIVO. Eles pedem coisas opostas — capacidade, cobertura, tempo, ou nada
        // (limite geometrico do cache) —, e somados num numero so nao respondem nenhuma delas.
        // A ordem espelha RC_STAT_MISS_* e o static_assert abaixo trava o par com os RC_QUERY_*.
        u32 MissShort   = 0; // segmento menor que a celula
        u32 MissCone    = 0; // cone especular menor que a celula
        u32 MissNoEntry = 0; // chave ausente ou balde cheio  -> capacidade
        u32 MissEmpty   = 0; // celula sem amostra            -> cobertura do produtor
        u32 MissWarming = 0; // abaixo do piso de confianca   -> so tempo
        u32 MissStale   = 0; // vai refrescar                 -> resposta a luz dinamica
        // Saude do hash, medida na insercao. A meta do plano (InsertFull < 0,1% de InsertTries) e
        // sobre CAPACIDADE, e por isso `Contended` e contador separado: perder 32 disputas de CAS
        // acontece com a tabela longe de cheia e nao se conserta com tabela maior. Somados, o gate
        // mandaria crescer a tabela por um problema de concorrencia.
        u32 InsertTries = 0;
        u32 InsertFull  = 0; // cadeia inteira ocupada  -> capacidade
        u32 Contended   = 0; // 32 CAS perdidos         -> concorrencia no mesmo balde
        u32 Retries     = 0; // disputas perdidas (soma), inclusive as que venceram depois
        // Amostra descartada por a celula ja ter 64 reservas NESTE frame. A chave existe e a
        // insercao deu certo — sem este contador, `Tries - Full - Contended` se leria como
        // "aceitos" e estaria errado. Aceitos = Tries - Full - Contended - Capped.
        u32 Capped      = 0;
        u32 ProbeSum    = 0; // / InsertTries = comprimento medio da CADEIA (1a varredura so)
        u32 ProbeMax    = 0; // pior caso DO FRAME (o resolve zera todo frame)
        // O PRODUTOR visto de dentro. `Paths` contra a fracao pedida fecha o gate da Fase 3 que
        // nunca foi medido; o terminal por tipo diz de onde vem a energia que entra no cache —
        // tudo em SKY quer dizer que a realimentacao ainda nao comecou.
        u32 Paths      = 0;
        u32 PathVerts  = 0; // / Paths = profundidade media
        u32 PathDepth  = 0; // maxima do frame
        // Os quatro ultimos entregam a mesma radiancia (zero pela frente) e por isso pareciam um
        // contador so — mas pedem acoes opostas: miss e cache frio, noquery e o operador tendo
        // desligado o terminal, lobe e material, e other e o teto de vertices.
        u32 TermSky     = 0;
        u32 TermCache   = 0;
        u32 TermKilled  = 0; // segmento morto pela politica de backface
        u32 TermMiss    = 0; // consultou o terminal e nao achou
        u32 TermNoQuery = 0; // chegou ao terminal com a consulta desligada
        u32 TermLobe    = 0; // amostragem de BSDF sem direcao valida
        u32 TermOther   = 0; // residual: teto de vertices sem passar pelo terminal
        // FONTE do terminal do render (quarto regime, `Sf`). Os de miss dizem por que a consulta
        // errou; estes dizem o que respondeu DEPOIS dela, que e a curva do gate da Fase 5.
        //
        // DOIS EIXOS: `Cache + DDGI + Zero == Total` responde QUEM FORNECEU a radiancia, e
        // `Ineligible` responde POR QUE nao foi o cache — ele CRUZA com os tres, nao compete.
        // A primeira igualdade e o teste do instrumento; a segunda cruza com `MissShort+MissCone`.
        u32 SrcTotal      = 0; // todo hit sombreado; o denominador
        u32 SrcCache      = 0;
        u32 SrcDDGI       = 0; // o cache errou e o volume respondeu
        u32 SrcZero       = 0; // o cache errou e nao havia volume: zero explicito
        // Nem chegou a consultar: segmento menor que a celula ou cone mais estreito que ela. Estes
        // raios estao TAMBEM em `SrcDDGI` ou `SrcZero` — nunca em `SrcCache`, por construcao.
        //
        // Ele mede quanto dos hits o cache NAO PODE atender por geometria, e so isso. Quanto disso
        // caiu especificamente no DDGI e interseccao, e interseccao nao sai de totais marginais:
        // precisaria de um contador de `ineligible && ddgiAnswered`, que nao existe e que esta
        // fase nao exige.
        u32 SrcIneligible = 0;
    };

    // O que um snapshot de contadores PRECISA carregar junto para significar alguma coisa: se ele
    // ainda descreve o presente, e sob qual regime foi medido.
    //
    // Sem isto o painel comparava numeros com os TOGGLES PEDIDOS, que e outra coisa: o readback
    // vem kFramesInFlight frames atras, e entre a medida e a leitura o operador pode ter trocado
    // de regime, resetado a tabela ou desligado o cache. O sintoma era um numero do regime
    // anterior exibido como se fosse de agora — e, com o cache desligado, exibido para sempre,
    // porque nada mais o reescreve.
    struct FRadianceCacheStatsMeta {
        bool Valid  = false; // ha snapshot e ele nao foi invalidado depois de tirado
        // Regime EFETIVO do frame que o produziu — o que o shader recebeu, nao o que o knob pedia.
        bool Stats  = false;
        bool Detail = false;
        bool Source = false;
    };

    // Contadores + meta como UM valor. Existe para quem le de fora de outra thread: pedir os dois
    // em chamadas separadas solta e retoma o lock do renderer no meio, e o par pode vir de frames
    // diferentes — meta novo com contador velho, que e exatamente o que o meta veio impedir.
    // Copia por valor: sao ~150 bytes num painel de diagnostico.
    struct FRadianceCacheSnapshot {
        FRadianceCacheStats     Stats;
        FRadianceCacheStatsMeta Meta;
        u32                     Capacity = 0; // junto, pela mesma razao: a ocupacao e Occupied/Cap
        u64                     Bytes    = 0;
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
        // TERCEIRO regime de medicao, e nao um detalhe do segundo: a contagem de atomicos do
        // kFlagStats e congelada para a serie ja tirada nela continuar comparavel. Ver o
        // RC_FLAG_STATS_DETAIL no shader.
        static constexpr u32 kFlagStatsDetail = 8u;
        // QUARTO regime (`Sf`): a FONTE do terminal de cada hit de render. Separado do detalhe pelo
        // mesmo argumento que separou o detalhe do basico, com uma ressalva propria: esta
        // telemetria e neutra para o CONTEUDO do cache (com o produtor dedicado, os traces de
        // render nao inserem), e mesmo assim nao cabe no `Sd` — ela nao e neutra para o custo do
        // trace, para a contencao no UAV de estatisticas, nem para o overlap com o updater. "Nao
        // muda a imagem" e menos que "e comparavel".
        static constexpr u32 kFlagStatsSource = 16u;

        // Espelham RC_SAMPLE_NUM_MAX / RC_STALE_FRAME_MAX do shader. Mudar aqui sem mudar la
        // deixa o teto de amostras incoerente com o teto do ponto fixo do acumulador.
        static constexpr u32 kMaxAccumSamples = 64;
        static constexpr u32 kStaleFrameMax   = 64;

        // 2^17 = 131.072 celulas = 4,5 MB (4 + 16 + 16 bytes por celula). Isto e RESULTADO DE
        // MEDIDA, e nao a estimativa que estava aqui: com o produtor dedicado a tabela de 2^20
        // ocupava 2,40% (25.176 celulas, Bistro exterior, V1, C4, N=128). A conta antiga foi feita
        // quando quem alimentava o cache eram os hits do RENDER — todo raio secundario inseria, e
        // 2^20 fazia sentido. O passe dedicado escreve por 4% dos pixels e derrubou a populacao
        // em quase duas ordens de grandeza; a capacidade nao tinha sido refeita contra ele.
        //
        // 2^17 e o melhor COMPROMISSO contra a faixa de 20-70% do gate da Fase 4 — nao um encaixe
        // perfeito, e vale ser exato sobre isso: V1 fica em 19,2%, oito decimos ABAIXO do piso da
        // faixa. Nenhuma potencia de dois poe os dois defaults dentro dela, porque V4 e o dobro de
        // V1 e a faixa e menos de quatro vezes:
        //
        //     2^16 -> V1 38,4%  V4 77,4%   (V4 estoura o teto por 7 pp)
        //     2^17 -> V1 19,2%  V4 38,7%   (V1 rente ao piso, por 0,8 pp)
        //     2^18 -> V1  9,6%  V4 19,4%   (os DOIS abaixo do piso)
        //
        // 2^17 e o unico em que nenhum lado erra por mais de um ponto, e erra para o lado barato:
        // ficar sob o piso desperdicia memoria, estourar o teto perde amostra.
        //
        // ⚠️ RESSALVA DE ESCOPO, a mesma do UpdateMaxVertices: a medida e de UMA pose estatica na
        // Bistro exterior. Camera andando mantem vivas as celulas dos ultimos kStaleFrameMax
        // frames e sobe a ocupacao; interior tambem. Quem descobre isso e `InsertFull` — o gate de
        // 0,1% existe exatamente para esta capacidade nao virar decisao de fe. Subir de volta e um
        // argumento no SetupForScene, sem recompilar nada.
        //
        // A SHaRC do Cyberpunk usa 2^22 (~160 MB) para Night City; as cenas da Smile sao uma ordem
        // de grandeza menores em extensao, e a categoria "GI e reflexos" do VramTracker ja e a
        // segunda maior da engine.
        static constexpr u32 kDefaultCapacityLog2 = 17;
        static constexpr u32 kMaxCapacityLog2     = 22;

        void Initialize(ID3D12Device* Device);

        // Capacidade e potencia de 2 porque o modulo do hash vira mascara (Capacity - 1).
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           u32 CapacityLog2 = kDefaultCapacityLog2);
        void Release(FTextureSRVHeap& SRVHeap);

        void UpdatePerFrame(u32 InFrameSlot, const Vec3& InCameraPos);
        void RecordResolve(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // === Passe dedicado de update (Fase 3) ===========================================
        // O produtor PROPRIO do cache. Ate ele existir, quem alimentava a tabela eram os hits do
        // render, cujo terminador era o DDGI — o cache guardava um sinal cuja origem continuava
        // sendo DDGI. Com este passe ativo o Renderer para de armar o bit de update dos traces
        // (ver AllowUpdate em ShaderParams) e a fonte passa a ser este dispatch.
        //
        // Os slots seguem o padrao do FReSTIRGI: o fallback chega como CONTRATO porque as
        // posicoes t3/t4/t5 do shader existem so para o gather do HitShading compilar — este
        // passe nunca as le, mas descriptor table nao aceita buraco.
        static constexpr u32 kUpdateSrvCount = 9; // TLAS, sky, inst, irrad, dist, probe, depth,
                                                  // gbuffer, luzes
        void SetupUpdatePass(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                             u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot,
                             const FGIFallbackBindings& Fallback,
                             u32 DepthSlot, u32 GBufferSlot, u32 Width, u32 Height);
        // t8 reescrito por frame no heap shader-visible: uma tabela por frame em voo, escrita na
        // do FrameSlot corrente (padrao do FReflections). Sem isso o frame N-1, ainda em voo,
        // leria o buffer de luzes que o N acabou de publicar.
        void SetUpdatePunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                        u32 StagingSlot, u32 InFrameSlot);
        // CB do passe. Chamar no PrepareIndirectLighting (onde as luzes e o sol ja estao
        // resolvidos); a gravacao acontece depois do G-buffer.
        // BackfacePolicy vem de FORA (o toggle do ReSTIR GI) e nao e knob desta classe: o cache
        // alimenta o ReSTIR GI, e duas politicas de geometria diferentes fariam a mesma superficie
        // ter duas radiancias conforme o caminho. Mesmo criterio do ShadowRayMask.
        void UpdatePassPerFrame(u32 InFrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                                const Vec3& SunDir, f32 SunIntensity, const Vec3& SunColor,
                                u32 ShadowRayMask, u32 FrameIndex, f32 SkyIntensity,
                                f32 MaxRayDist, u32 PunctualLightCount, bool BackfacePolicy);
        void RecordUpdate(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Recursos do passe montados (tabelas validas). Nao diz que ele roda neste frame.
        bool IsUpdatePassReady() const { return UpdateReady; }
        // Roda neste frame: recursos prontos, cache ligado e o produtor dedicado escolhido. O
        // ResetPending entra porque ate o resolve de reset terminar a tabela e do regime anterior
        // — alimenta-la seria trabalho descartado alguns passes depois.
        bool UpdatePassActive() const {
            return UpdateReady && Enabled && Ready && DedicatedUpdate && !ResetPending;
        }

        // Perfil de epsilons e parametros do hit, no mesmo regime dos outros passes de RT: dono e
        // o Renderer, que empurra a copia todo frame. Ver FRayEpsilonProfile / FGIHitSampling.
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        void SetGIHitSampling(const FGIHitSampling& S)   { GIHit = S; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }

        // O produtor do cache e o passe dedicado (true) ou os hits do render (false). Invalida a
        // tabela nas DUAS bordas: as duas fontes produzem estatisticas diferentes para a mesma
        // celula, e uma media que mistura as duas nao descreve nenhuma das duas.
        void SetDedicatedUpdate(bool V) { if (V != DedicatedUpdate) ResetOnce(); DedicatedUpdate = V; }
        bool GetDedicatedUpdate() const { return DedicatedUpdate; }
        // Fracao dos pixels que lanca caminho por frame. 0,04 = o valor publicado do Cyberpunk.
        // Quantizada em celulas do tile 5x5, entao o passo util e 1/25.
        void SetUpdateFraction(f32 V) { UpdateFraction = V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }
        f32  GetUpdateFraction() const { return UpdateFraction; }
        // Terminal do caminho de update no cache RESOLVIDO (frames anteriores). E o que transforma
        // um bounce por frame em multi-bounce no tempo; desligar deixa o cache aprender so o
        // primeiro bounce, que e o A/B que separa "o cache funciona" de "o cache realimenta".
        void SetUsePrevCacheAtTerminal(bool V) { UsePrevCacheAtTerminal = V; }
        bool GetUsePrevCacheAtTerminal() const { return UsePrevCacheAtTerminal; }
        // Vertices SOMBREADOS por caminho. Custo em raios = V + 1 (o ultimo e o terminal). 4 e o
        // numero do Cyberpunk; 1 reproduz exatamente o produtor de um bounce, e e esse o A/B que
        // separa "o multi-bounce do frame vale o custo" de "o do tempo ja bastava".
        //
        // Invalida: o que a tabela guarda em cada regime e diferente — com um vertice a celula
        // acumula a serie de ponto fixo entre frames, com quatro ela ja chega resolvida.
        void SetUpdateMaxVertices(u32 V) {
            const u32 C = V < 1u ? 1u : (V > kMaxUpdateVertices ? kMaxUpdateVertices : V);
            if (C != UpdateMaxVertices) ResetOnce();
            UpdateMaxVertices = C;
        }
        u32  GetUpdateMaxVertices() const { return UpdateMaxVertices; }
        // Piso de roughness do lobo que CHEGA para o vertice poder ser gravado. Nasce igual ao
        // FGIHitSampling::SecondaryRoughnessMin de proposito: aquele e o piso que o material recebe
        // no hit, entao qualquer lobo especular deste passe ja e pelo menos assim largo — os dois
        // numeros descrevem a mesma decisao ("o que o cache nao-direcional consegue representar")
        // vista de dois lados. Subir este acima daquele e que passa a rejeitar de verdade.
        void SetMinCacheableRoughness(f32 V) {
            MinCacheableRoughness = V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V);
        }
        f32  GetMinCacheableRoughness() const { return MinCacheableRoughness; }
        // PISO DE CONFIANCA — amostras minimas para a celula poder ENCERRAR um caminho.
        //
        // Ate a Fase 4 o unico teste era `sampleNum == 0`: uma celula de UMA amostra devolvia
        // RC_QUERY_HIT e o chamador parava de sombrear, adotando uma unica amostra de path tracer
        // como radiancia convergida — e servindo-a a todos os raios que caissem ali pelos frames
        // seguintes. Este e o "celula fria nao pode terminar um path" do plano.
        //
        // Vale para os DOIS lados (consulta do render e terminal do updater), como a politica de
        // backface e pelo mesmo motivo: dois pisos fariam a mesma celula ser confiavel num caminho
        // e nao no outro.
        //
        // Invalida a tabela: o terminal do updater consulta com este piso, entao o que a celula
        // GUARDA depende dele — mesmo criterio dos vertices e do piso de roughness.
        void SetMinSampleCount(u32 V) {
            const u32 C = V < 1u ? 1u : (V > kMaxMinSamples ? kMaxMinSamples : V);
            if (C != MinSampleCount) ResetOnce();
            MinSampleCount = C;
        }
        u32  GetMinSampleCount() const { return MinSampleCount; }
        // Teto do knob, e nao do formato: `sampleNum` cabe em 16 bits e o acumulador para em
        // kMaxAccumSamples. Acima de uns poucos a celula so ficaria inalcancavel — com 64 nenhuma
        // celula jamais seria consultavel antes de estar no teto, e o cache viraria decoracao.
        static constexpr u32 kMaxMinSamples = 16;
        // Espelha o RCU_MAX_VERTS do shader, que e constante de COMPILACAO la (o array de vertices
        // precisa viver em registrador). Pedir mais que isto no CB nao alocaria nada — o shader
        // clampa —, entao o teto vale dos dois lados.
        static constexpr u32 kMaxUpdateVertices = 4;

        // Segunda copia dos contadores, para buffer PROPRIO, a ser gravada DEPOIS do
        // RecordResolve. Existe porque as duas metades da telemetria ficam prontas em momentos
        // opostos do frame:
        //
        //   - QUERY/HITS sao escritos pelos traces e ZERADOS pelo resolve. So a copia que o
        //     RecordResolve faz antes do dispatch os pega deste frame.
        //   - OCUPACAO/VALID/SAMPLES sao escritos pela varredura DENTRO do resolve. Aquela mesma
        //     copia os pega do frame ANTERIOR.
        //
        // Um frame de defasagem e invisivel no painel, e por isso o caminho normal tem uma copia
        // so. No manifesto muda: ocupacao em funcao do N e exatamente o que a calibracao vai
        // medir. A captura le as duas e monta o quadro do frame que ela gravou.
        //
        // Buffer separado do anel de proposito: escrever no anel poria zeros de query no painel
        // por um frame a cada captura.
        void RecordStatsCopy(ID3D12GraphicsCommandList* CL);
        // Le a copia acima (exige a fila ja sincronizada). Falso se nao houver copia pendente.
        bool CollectCaptureStats(FRadianceCacheStats& Out);

        // O que os consumidores REALMENTE receberam neste frame, gravado pelo ShaderParams — que e
        // por onde todos passam. Ler Enabled/QueryEnabled/ResetPending no fim do frame daria outra
        // resposta: o RecordResolve limpa o ResetPending no meio do caminho, entao um frame que
        // entregou flags ZERADAS aos traces (o primeiro depois de um reset, que e exatamente o
        // caso N=0 da captura) terminaria se declarando ativo.
        bool PublishedUpdateThisFrame() const { return PublishedUpdate; }
        bool PublishedQueryThisFrame() const  { return PublishedQuery; }
        // A instrumentacao NAO e neutra: os atomicos disputados por wave mudam o escalonamento e,
        // com ele, quais threads vencem as insercoes do cache. Medido — 73.218 celulas sem, 73.195
        // com, e PSNR de 48 dB entre os dois regimes. Entao ela e parte da CONFIGURACAO, nao um
        // observador invisivel, e por isso entra no manifesto e na etiqueta do arquivo.
        bool PublishedStatsThisFrame() const  { return PublishedStats; }
        // Idem para o regime de detalhe, que soma atomicos por cima do anterior — inclusive no
        // produtor, onde eles mudam quem vence as insercoes.
        bool PublishedStatsDetailThisFrame() const { return PublishedDetail; }
        // Idem para o regime de FONTE, que soma mais um atomico por hit sombreado.
        bool PublishedStatsSourceThisFrame() const { return PublishedSource; }
        // QUEM escreveu neste frame — o passe dedicado, ou os hits do render. Publicado pelo mesmo
        // mecanismo dos anteriores (o ShaderParams do produtor que rodou) em vez de reconstituido
        // pelo manifesto a partir dos knobs: o `GetDedicatedUpdate` diz o que foi PEDIDO, e um
        // frame com reset pendente ou tabelas nao montadas nao escreveu nada apesar do pedido.
        bool PublishedDedicatedThisFrame() const { return PublishedDedicated; }
        // Fracao EFETIVA deste frame, ja quantizada em celulas do tile 5x5 — o numero que o shader
        // usou, nao o do slider. 0 = o passe nao rodou.
        f32  PublishedUpdateFraction() const {
            return static_cast<f32>(PublishedUpdateCells) / 25.0f;
        }
        // Os outros dois parametros que decidem o que a tabela guarda. Publicados pelo mesmo
        // criterio: sao o que o SHADER recebeu neste frame, e 0 quando o passe nao rodou.
        u32  PublishedUpdateVertices() const { return PublishedVertices; }
        f32  PublishedMinCacheableRoughness() const { return PublishedMinRoughness; }
        // Piso de confianca EFETIVO. Zero quer dizer "ninguem consultou o cache neste frame" — e
        // nao "piso zero": ele so descreve alguma coisa quando houve consulta, e as consultas vem
        // de dois lugares independentes (os traces de render e o terminal do updater).
        u32  PublishedMinSampleCount() const { return PublishedMinSamples; }

        // ConsumerRuns = este consumidor vai realmente tracar neste frame. Os params sao montados
        // para todos (custa nada, e o passe pode nem rodar), mas so quem RODA conta para o
        // registro acima — senao o manifesto diria "cache ativo" num frame em que nenhum trace o
        // consultou, que e o mesmo tipo de mentira que o registro veio corrigir.

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
        // Validade e regime DO SNAPSHOT acima. Quem exibe tem de ler os dois juntos — o `Stats()`
        // sozinho nao diz de quando e nem de qual regime ele veio.
        const FRadianceCacheStatsMeta& StatsMetaCPU() const { return StatsMeta; }
        // Os quatro juntos, por VALOR. E este que a UI usa; os acessores por referencia acima
        // continuam para quem ja esta dentro do renderer (o capturador), onde nao ha lock a soltar.
        FRadianceCacheSnapshot Snapshot() const {
            return { StatsCPU, StatsMeta, CapacityV, MemoryBytes() };
        }

        void SetDebugMode(ERadianceCacheDebugMode M) { DebugMode = M; }
        ERadianceCacheDebugMode GetDebugMode() const { return DebugMode; }
        // Instrumentacao de acerto/erro do query. Custa dois atomicos disputados por wave em todo
        // trace do frame, entao e knob proprio e nao vem junto do Enabled.
        void SetStatsEnabled(bool V) { StatsEnabled = V; InvalidateStatsSnapshot(); }
        bool GetStatsEnabled() const { return StatsEnabled; }
        // Detalhe: misses por motivo (nos traces) e saude da insercao (no produtor). Exige a
        // instrumentacao ligada — sozinho ele nao teria denominador para o hit rate — e e um
        // regime PROPRIO, com etiqueta e manifesto: os atomicos extras mudam o escalonamento das
        // waves, e sem separa-lo a serie ja medida em `S` deixaria de ser comparavel.
        void SetStatsDetailEnabled(bool V) { StatsDetail = V; InvalidateStatsSnapshot(); }
        bool GetStatsDetailEnabled() const { return StatsDetail; }
        // Fonte do terminal por hit de render. Exige a instrumentacao ligada pelo mesmo motivo do
        // detalhe — sem ela o regime seria um quinto, e regime que ninguem compara nao serve.
        void SetStatsSourceEnabled(bool V) { StatsSource = V; InvalidateStatsSnapshot(); }
        bool GetStatsSourceEnabled() const { return StatsSource; }

        // Os passes de trace escrevem por atomico bindless e leem o Resolved; ambos precisam do
        // recurso em UNORDERED_ACCESS / NON_PIXEL. Chamar antes de gravar os traces do frame.
        void TransitionForTrace(ID3D12GraphicsCommandList* CL);

        // Limpa a tabela inteira no proximo resolve. Necessario quando a cena troca ou a camera
        // teleporta: o conteudo descreve outro lugar do mundo e envelhecer celula a celula
        // levaria kStaleFrameMax frames de fantasma.
        //
        // O estado do aquecimento e escrito AQUI, e nao amostrado do `ResetPending` pelo tick. Este
        // metodo e chamado de fora do frame — knob da UI, carga de cena — e portanto tambem DEPOIS
        // do tick: nesse caso o resolve do fim do frame limparia o pending sem que ninguem tivesse
        // observado, e o frame seguinte encontraria `Active` sobre uma tabela recem-zerada. O
        // estado tem de sair do EVENTO, nao de uma flag que outro passe apaga.
        void ResetOnce() {
            ResetPending = true;
            Warmup       = ERadianceCacheWarmup::Resetting;
            FillFrames   = 0;
            // Os snapshots em voo descrevem a tabela ANTERIOR. Eles nao passam a estar errados —
            // mediram bem o frame deles —, mas param de descrever o presente, e e isso que um
            // painel afirma ao exibi-los.
            InvalidateStatsSnapshot();
        }

        u32  Capacity() const { return CapacityV; }
        u64  MemoryBytes() const;

        // Desligar INVALIDA o snapshot, e este e o caso que mais enganava: sem o cache o resolve
        // nao roda, nenhuma copia nova e gravada, e os ultimos numeros ficariam na tela
        // indefinidamente — nao por um frame, para sempre.
        void SetEnabled(bool V)       { Enabled = V; if (!V) InvalidateStatsSnapshot(); }
        bool GetEnabled() const       { return Enabled; }
        // Query e update sao knobs separados de proposito: o A/B util e ligar o update, deixar o
        // cache encher por alguns segundos e SO entao ligar a leitura. Ligando os dois de uma vez
        // os primeiros frames leem uma tabela vazia e o resultado parece um bug.
        //
        // Isso descreve o A/B; o AutoWarmup abaixo e o mesmo gesto feito pela maquina, em
        // producao, sem depender de alguem contar os segundos.
        // Todo knob que muda O QUE OS CONTADORES SIGNIFICAM invalida o snapshot — este e os quatro
        // de telemetria abaixo. Nao porque a medida antiga esteja errada (ela nao esta: o meta diz
        // sob qual regime ela saiu), mas porque durante a latencia do anel o painel exibiria o
        // regime ANTERIOR ao lado de um toggle que ja diz outra coisa. Dois frames de contradicao
        // visivel valem menos que dois frames de linha ausente.
        void SetQueryEnabled(bool V)  { QueryEnabled = V; InvalidateStatsSnapshot(); }
        bool GetQueryEnabled() const  { return QueryEnabled; }

        // === Aquecimento global (Fase 4) ==================================================
        // Modo de PRODUCAO: a consulta de render so abre quando a tabela vale a pena. Desligar
        // reproduz EXATAMENTE o regime anterior — a consulta segue o QueryEnabled e mais nada —,
        // e e esse o outro braco do A/B, como `MinSampleCount = 1` e para o piso de confianca.
        //
        // Nao invalida a tabela, e a diferenca com os outros knobs desta fase e o motivo: este nao
        // muda o que ENTRA na celula, so quem le. Mesmo criterio do QueryEnabled, que tambem nao
        // invalida.
        void SetAutoWarmup(bool V) { AutoWarmup = V; InvalidateStatsSnapshot(); }
        bool GetAutoWarmup() const { return AutoWarmup; }
        // O `ResetPending` entra na LEITURA, e nao so no tick: quem arma o reset pode ser um knob
        // no meio do frame, depois de o tick ja ter passado. Nesse frame os consumidores ja
        // recebem flags zeradas, e o estado tem de dizer o mesmo — senao o manifesto sairia com
        // "active" num frame que nao consultou nada.
        ERadianceCacheWarmup WarmupState() const {
            return ResetPending ? ERadianceCacheWarmup::Resetting : Warmup;
        }
        const char* WarmupStateName() const;
        // Frames em Filling (0 fora dele). E o que o painel mostra para o operador saber se a
        // espera acabou ou travou.
        u32  WarmupFillFrames() const { return FillFrames; }
        // Anda a maquina de estados. Chamar UMA vez por frame, NO TOPO — antes de qualquer
        // consumidor publicar o cbuffer dele, e nao junto do UpdatePerFrame, que roda no meio do
        // caminho. A borda derruba historico, e quem ja resolveu o proprio `UseHistory` naquele
        // frame nao ve o reset: a nevoa volumetrica publica o dela no ResolveFrameLighting, bem
        // antes do bloco de GI, e reprojetaria um frame de historia que devia ter sido descartada.
        //
        // Depende so do readback dos contadores (CollectStats) e das flags da classe — nao da
        // camera —, entao subir na ordem do frame nao custa nada.
        void TickWarmup();

        // A consulta do render MUDOU por conta propria — sem passar por setter nenhum. Quem
        // consome tem de derrubar o historico dos consumidores: o terminador do raio secundario
        // trocou, e o que eles acumularam foi medido com o outro. E a mesma razao pela qual o
        // toggle manual de leitura invalida (ver FRenderSettings).
        //
        // Duas bordas, um latch, porque a acao e a mesma nos dois sentidos:
        //   ABRIU  — `Filling -> Active` com o aquecimento automatico governando a consulta;
        //   FECHOU — o reload de shader, que reseta a tabela sem passar pelo funil.
        // Os demais resets chegam por setter, e o setter ja invalida.
        //
        // Latch em vez de callback porque esta classe nao conhece o FRenderSettings — e o mesmo
        // padrao do `ConsumeRestore` do capturador.
        bool ConsumeQueryChange() {
            const bool V = QueryChanged;
            QueryChanged = false;
            return V;
        }
        // A consulta do RENDER esta liberada neste frame? Com AutoWarmup desligado, sempre.
        bool WarmupAllowsQuery() const {
            return !AutoWarmup || WarmupState() == ERadianceCacheWarmup::Active;
        }
        // A consulta do render esta EFETIVAMENTE aberta agora — a condicao inteira, e nao so o
        // portao do aquecimento. E a mesma que o ShaderParams usa para armar o kFlagQuery, e mora
        // aqui para os dois nao poderem divergir: quem decide invalidar historico tem de estar
        // olhando exatamente o que os traces receberam.
        bool QueryEffective() const {
            return Enabled && Ready && !ResetPending && QueryEnabled && WarmupAllowsQuery();
        }

        // Fracao das celulas COM AMOSTRA que precisa estar confiavel (N >= piso) para Filling
        // terminar. Medido na Bistro exterior (V1, piso 4, N=128): o regime permanente e 80,8% —
        // 20.340 confiaveis de 25.176 com amostra. O piso fica ABAIXO disso de proposito: o
        // produtor cria celulas novas todo frame, entao essa fracao tem um TETO menor que 1 que
        // depende da cena, e um gate colado no teto medido de uma cena nao dispararia em outra
        // com mais churn — viraria decoracao, e quem decidiria de fato seria sempre o teto de
        // frames abaixo.
        static constexpr f32 kWarmupConfidentRatio = 0.70f;
        // Teto de espera, para Filling nao ficar preso numa cena cujo regime permanente fica
        // abaixo da fracao acima. 96 = 1,5x o kStaleFrameMax: a tabela ja deu mais de uma volta
        // completa de despejo, entao o que estiver la e o regime permanente e nao o transiente.
        //
        // O teto NAO dispensa a tabela ter conteudo — ver o TickWarmup. Ele existe para cobrir a
        // cena que enche devagar, nao a que nao enche: com o produtor parado, abrir a consulta
        // depois de 96 frames recriaria exatamente o custo (busca em toda consulta, miss em todas)
        // que este estado existe para evitar, e sem prazo para acabar.
        static constexpr u32 kWarmupMaxFrames = 96;

        // Aresta da celula no nivel 0. 0,25 m e o valor do idTech 8.
        void SetBaseCellSize(f32 V)   { BaseCellSize = V < 0.01f ? 0.01f : V; }
        f32  GetBaseCellSize() const  { return BaseCellSize; }
        // Distancia em que o nivel sobe de 0 para 1 (e depois dobra). Controla quao rapido as
        // celulas crescem com o afastamento da camera.
        void SetLodDistance(f32 V)    { LodDistance = V < 0.1f ? 0.1f : V; }
        f32  GetLodDistance() const   { return LodDistance; }

        // Update = false para consumidor DIRECIONAL (reflexoes): a radiancia que ele produz vale
        // para uma direcao de espelho e o cache nao guarda direcao. Ele ainda pode CONSULTAR.
        //
        // Com o produtor dedicado ativo, o Renderer passa AllowUpdate = false para TODOS os
        // consumidores de render: eles voltam a so consultar, que e a divisao que a Fase 3 existe
        // para estabelecer.
        FRadianceCacheShaderParams ShaderParams(bool AllowUpdate, bool ConsumerRuns = true) const;

    private:
        // Flags do proprio passe de update — nao saem do ShaderParams porque as regras sao outras:
        //
        //   UPDATE: sempre (e a razao de ele existir).
        //   QUERY:  segue o UsePrevCacheAtTerminal, e NAO o QueryEnabled do render. Sao perguntas
        //           diferentes: "os traces aproveitam o cache?" e "o caminho de update termina
        //           nele?". Amarra-las faria o A/B de query desligar o multi-bounce sem avisar.
        //   STATS:  NUNCA. Os contadores de acerto/erro descrevem os RAIOS DE RENDER, e o proprio
        //           plano registra que a instrumentacao nao e neutra (os atomicos mudam o
        //           escalonamento das waves e, com ele, quais threads vencem as insercoes).
        //           Somar as consultas do updater mudaria a serie e a medida ao mesmo tempo.
        FRadianceCacheShaderParams UpdatePassParams() const;

        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateUpdatePipeline(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void ReleaseUpdatePass(FTextureSRVHeap& SRVHeap);
        D3D12_GPU_VIRTUAL_ADDRESS UpdateCBAddr() const;
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        // Variant 0 = resolve, 1 = clear dos contadores. Duas entradas por frame em voo porque os
        // dois dispatches acontecem no MESMO frame e precisam de StatsParams.x diferentes — um CB
        // por slot so daria um valor.
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr(u32 Variant) const;
        D3D12_GPU_VIRTUAL_ADDRESS DebugCBAddr() const;

        void ReleaseDebug(FTextureSRVHeap& SRVHeap);

        FComputePipeline ResolvePSO;
        FComputePipeline DebugPSO;
        FComputePipeline UpdatePSO;

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
        // Metadados POR SLOT, gravados junto da copia: o regime e uma propriedade do frame que
        // mediu, e ele nao sobrevive a viagem pelo anel de outra forma.
        FRadianceCacheStatsMeta StatsSlotMeta[FCommandQueue::kFramesInFlight] = {};
        FRadianceCacheStats StatsCPU{};
        FRadianceCacheStatsMeta StatsMeta{};

        // Fora do anel: a captura sincroniza a fila e le no mesmo frame, entao um buffer basta —
        // e nao pode ser o do anel, sob pena de publicar zeros de query no painel. Ver
        // RecordStatsCopy.
        Microsoft::WRL::ComPtr<ID3D12Resource> CaptureStatsReadback;
        bool CaptureStatsPending = false;

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
        bool StatsDetail  = false;
        bool StatsSource  = false;

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

        // --- passe dedicado de update -----------------------------------------------------
        Microsoft::WRL::ComPtr<ID3D12Resource> UpdateCB;
        u8* MappedUpdateCB = nullptr;
        RadianceCacheUpdateConstants UpdateCPU{};
        // Uma tabela por frame em voo: o t8 (luzes) e reescrito por frame e as outras oito
        // entradas sao estaveis. Ver SetUpdatePunctualLightsSRV.
        u32 UpdateSrvTable[FCommandQueue::kFramesInFlight] = { kInvalidSlot, kInvalidSlot };
        static_assert(FCommandQueue::kFramesInFlight == 2,
                      "o inicializador acima e escrito na unha: com mais frames em voo as tabelas "
                      "extras nasceriam com 0, que e um slot VALIDO do heap — o Free devolveria "
                      "descritor alheio em vez de acusar");
        u32 UpdateWidth = 0, UpdateHeight = 0;
        u32 UpdateFrameSlot = 0;
        bool UpdateReady = false;

        FRayEpsilonProfile RayEps;
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        Vec4               SkyLutParams{};

        // Produtor dedicado (true) x hits do render (false). Nasce LIGADO: e o desenho da Fase 3,
        // e o caminho antigo fica como controle de A/B, nao como default de producao.
        bool DedicatedUpdate = true;
        // 4% dos pixels por frame — o numero publicado do Cyberpunk 2077 para o update do SHaRC.
        f32  UpdateFraction  = 0.04f;
        bool UsePrevCacheAtTerminal = true;
        // 1 VERTICE por default, e nao os 4 do SHaRC do Cyberpunk. Isto e resultado de MEDIDA, e a
        // medida foi na Bistro EXTERIOR — ver a ressalva no fim.
        //
        // V1 -> V4, 3060 Ti, preset gameplay com FSR: updater de 3,75 -> 7,73 ms (2,06x), frame de
        // 7,45 -> 11,36 ms. O resto do frame custa ~3,7 ms nos dois, ou seja o passe soma quase
        // integralmente, sem sobreposicao. O que os 3,98 ms compram: +0,45% de luminancia media e
        // 44,79 dB entre as duas imagens (88,4% dos pixels dentro de 1 nivel). Real, e pequeno
        // demais para dobrar o custo do passe.
        //
        // O numero que explica o porque: V4 acumula 2,00x as amostras de V1 — exatamente o dobro.
        // Como V1 grava 1 vertice por caminho por construcao, V4 grava em media DOIS, de quatro
        // possiveis. Os vertices 3 e 4 quase nao sao alcancados: o caminho escapa para o ceu ou
        // cai na elegibilidade antes. Pagar cinco raios para gravar dois vertices e o que a conta
        // diz que esta acontecendo.
        //
        // ⚠️ RESSALVA DE ESCOPO: a Bistro exterior e uma cena em que o caminho escapa cedo, e e
        // por isso que a media da 2,00. Em INTERIOR o caminho bate mais, a media sobe na direcao de
        // 4, e V4 passa a custar mais E entregar mais. Este default esta calibrado para uma classe
        // de cena, nao para todas — reabrir com medida de interior antes de trata-lo como geral.
        u32  UpdateMaxVertices     = 1;
        f32  MinCacheableRoughness = 0.5f;
        // 4 amostras. E um ponto de partida a MEDIR, nao um numero herdado de fonte nenhuma — o
        // que a serie tem para calibra-lo e o A/B, e `1` reproduz exatamente o comportamento
        // anterior ao piso, que e o outro braco dele.
        //
        // A ordem de grandeza vem do que a Fase 3 mediu: com N = 128 frames de aquecimento a
        // celula media tinha 39 das 64 amostras, ou seja ~0,3 amostra por frame. Um piso de 4 e
        // alcancado em ~13 frames — pequeno diante dos 64 do despejo, e grande o bastante para a
        // media ja nao ser uma amostra unica de path tracer.
        u32  MinSampleCount        = 4;
        // LOD do albedo nos hits, igual ao do ReSTIR GI. Um numero, nao um knob: divergir dele
        // faria o cache aprender um albedo e o render consumir outro.
        static constexpr f32 kUpdateAlbedoLOD = 2.0f;

        Vec3 CameraPos{ 0.0f, 0.0f, 0.0f };
        u32  CapacityV    = 0;
        // Ponto de operacao medido no Bistro em 2026-08-08: 22,8% de ocupacao. O par antigo
        // 0,25 m / 12 m saturava 95,2% da tabela e fazia qualquer medida de custo perder valor.
        f32  BaseCellSize = 0.50f;
        f32  LodDistance  = 6.0f;
        // LIGADOS por default desde a Fase 5 — o cache e a fonte PRIMARIA do indireto nos hits
        // elegiveis, e o DDGI cobre o que sobra (miss, celula fria, geometria inelegivel). Ate
        // aqui os dois nasciam desligados e toda a serie foi medida com alguem os ligando a mao;
        // era o "opt-in ate fechar o A/B visual do segundo bounce", e esse A/B fechou.
        //
        // O que a promocao custa, para nao ser descoberto depois: o passe de update passa a rodar
        // em toda cena (3,75 ms no V1 medido na 3060 Ti) e o cache ocupa 4,5 MiB. O que ela paga e
        // 68% dos raios secundarios terminando numa consulta de tabela em vez de material,
        // sombra e gather — que e a economia que a Fase 3 mediu e a Fase 4 confirmou.
        //
        // Os dois continuam knobs, e continuam SEPARADOS: o A/B de "escrita sem leitura" segue
        // valendo, e desligar os dois reproduz o pipeline pre-Fase-5 sem recompilar nada.
        bool Enabled      = true;
        bool QueryEnabled = true;
        bool ResetPending = false;
        // Aquecimento global. Nasce em Resetting porque e onde o SetupForScene poe a tabela: os
        // buffers vem com lixo do CreateCommittedResource e o primeiro resolve TEM de ser reset.
        bool AutoWarmup   = true;
        ERadianceCacheWarmup Warmup = ERadianceCacheWarmup::Resetting;
        // Frame de Filling que ESTA COMECANDO — o tick roda antes do trabalho do frame. Por isso o
        // teto compara com `>`: em `FillFrames == kWarmupMaxFrames` faltava rodar o proprio frame
        // 96, e trocar de estado ali entregaria 95 frames de preenchimento com o nome de 96.
        u32  FillFrames = 0;
        // A consulta do render abriu ou fechou sozinha. Consumido no topo do frame pelo Renderer.
        bool QueryChanged = false;
        // Defasagem do readback dos contadores, em frames, e o motivo de o gate nao poder olhar
        // o StatsCPU nos primeiros frames de Filling: o resolve do frame F copia a varredura de
        // F-1, e o anel entrega essa copia kFramesInFlight frames depois. Antes disso o StatsCPU
        // ainda descreve a tabela do regime ANTERIOR — cheia e confiavel —, e o gate pularia
        // direto para Active no primeiro frame depois de um reset, que e exatamente o caso que
        // ele existe para cobrir.
        static constexpr u32 kWarmupStatsLag = FCommandQueue::kFramesInFlight + 1u;

        // Os snapshots deixam de descrever o presente. Nao apaga os NUMEROS de proposito: quem os
        // le passa pelo `Valid`, e manter os valores ajuda a depurar o proprio painel.
        void InvalidateStatsSnapshot() {
            StatsMeta.Valid = false;
            for (auto& M : StatsSlotMeta) M.Valid = false;
        }
        bool Initialized  = false;
        bool Ready        = false;
        // Zeradas no UpdatePerFrame, escritas pelo ShaderParams. Mutaveis porque o ShaderParams e
        // const e continua sendo: ele nao muda o que o cache FAZ, so registra o que entregou.
        mutable bool PublishedUpdate = false;
        mutable bool PublishedQuery  = false;
        mutable bool PublishedStats  = false;
        mutable bool PublishedDetail = false;
        mutable bool PublishedSource = false;
        mutable bool PublishedDedicated = false;
        // O que o shader REALMENTE recebeu neste frame. Zerados no UpdatePerFrame para que "0"
        // signifique "o passe nao rodou" em vez de guardar o valor de um frame antigo.
        u32 PublishedUpdateCells = 0;
        u32 PublishedVertices    = 0;
        f32 PublishedMinRoughness = 0.0f;
        // Escrito pelos DOIS construtores de params, e por isso mutavel como os de cima: quem
        // consulta o cache pode ser um trace de render ou o terminal do updater, e o piso descreve
        // o frame se qualquer um dos dois rodou com a flag de query.
        mutable u32 PublishedMinSamples = 0;
    };
}
