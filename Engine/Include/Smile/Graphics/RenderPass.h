#pragma once

#include <d3d12.h>
#include <string_view>
#include <vector>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/HistoryDomain.h"

// O CONTRATO DE PASSE.
//
// A convencao de subsistema da engine sempre foi real, mas nunca foi enforcada — e por isso
// divergiu. Contagem sobre os 73 headers de Graphics/ no dia em que este arquivo nasceu:
//
//   prontidao      IsReady (16)   x  IsInitialized (24)   x  IsLoaded (1)
//   gravacao       Record  (25)   x  Render        (12)   x  Execute  (9)
//   resize         Resize  (13)   x  SetupForResize (8)
//   historico      Invalidate (12) x InvalidateHistory (7) x ResetHistory (3)
//                  x ResetHistoryOnce (1) x InvalidateResults (1)
//   pipelines      Recreate x RecreatePSO x RecreatePSOs x RecreatePipelines
//
// O custo NAO e a feiura. E que a orquestracao precisa saber o nome exato de cada subsistema,
// entao ela nao pode ser generica, entao ela e 2450 linhas de cola escrita a mao — e cada
// lista dessas e mantida a dedo. Duas ja divergiram, e o efeito e observavel hoje:
//
//   - RecreateAllPSOs()  chama MeshLights.RecreatePSO(), a tabela do ReloadShaders nao mapeia
//     nenhum stem dele  -> editar MeshLightExtract.cs.hlsl loga "sem pipeline mapeado" apesar
//     de a capacidade existir.
//   - A tabela chama RainWetness.Recreate(), o RecreateAllPSOs nao  -> um reload COMPLETO
//     (mudanca em .hlsli, ou stem nao mapeado) deixa a chuva com o PSO velho.
//   - A GTAO nao esta em lugar nenhum dos dois e nem tem funcao de recriar: os 3 shaders dela
//     simplesmente nao recarregam a quente.
//
// A cura nao e "revisar as duas listas". E o stem morar JUNTO do passe que o compila, e a
// lista virar uma varredura sobre o registro. Nasce impossivel de divergir.
namespace Smile {

    class FTextureSRVHeap;
    struct FSceneTargets;
    struct FFrameModes;

    // O que um passe precisa para (re)criar recurso. Existe porque as assinaturas de ciclo de
    // vida divergiam pelo MOTIVO ERRADO: `AO.SetupForResize(Device, SRVHeap, DepthSRVSlot,
    // NormalSRVSlot, W, H)` obrigava o orquestrador a saber que a GTAO quer justamente esses
    // dois slots. Passando os alvos da cena inteiros, quem sabe disso e o passe — que e quem
    // deveria saber. Trocar de insumo deixa de ser uma mudanca no call site.
    struct FPassInitContext {
        ID3D12Device*    Device   = nullptr;
        FTextureSRVHeap* SRVHeap  = nullptr;
        FSceneTargets*   Targets  = nullptr;  // alvos em render-res; nulo antes de existirem
        u32              RenderWidth  = 0;
        u32              RenderHeight = 0;

        // Formatos com que o pipeline monta PSO grafica. Sao propriedade do PIPELINE, nao de
        // cada passe: eram duas constantes locais (`RT`/`DS`) repetidas no RecreateAllPSOs e na
        // tabela do ReloadShaders, e passar por aqui evita que a terceira copia nasca dentro de
        // cada passe migrado — que e como um deles acabaria divergindo.
        DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        DXGI_FORMAT SceneDepthFormat = DXGI_FORMAT_D32_FLOAT;
        DXGI_FORMAT VelocityFormat   = DXGI_FORMAT_R16G16_FLOAT;
    };

    // Os .cso que este passe compila, para o hot reload casar o arquivo salvo com quem o usa.
    // Span cru sobre um array estatico do proprio passe: sem alocacao, sem dono, e o vetor de
    // stems fica visivel ao lado do Initialize que os carrega.
    struct FPassShaderStems {
        const char* const* Data  = nullptr;
        u32                Count = 0;

        const char* const* begin() const { return Data; }
        const char* const* end()   const { return Data + Count; }
    };

    // === DUAS BASES, E A DIFERENCA IMPORTA ==============================================
    //
    // Havia uma so, e nela entrou o FPipelineState — que e um SACO DE PSOs com nove getters, sem
    // Record, sem Execute e sem alvo proprio. Os outros passes o BINDAM; ele nao executa nada.
    // Idem FHDREnvironment e FCloudNoise, que sao BAKERS: rodam ao carregar um HDRI ou ao trocar a
    // seed do clima, nao por frame. Dar um Name() a eles e prometer uma linha no profiler que
    // nunca vai existir.
    //
    // A causa era o registro juntar duas responsabilidades que se sobrepoem mas nao coincidem:
    // "quem tem pipeline para recompilar" e "quem roda no frame". As referencias separam — no Cry
    // o reload e do CShaderMan e nao da lista de stages; na Unreal e do sistema de PSO, fora do
    // FSceneRenderer. (O Flax junta, mas la todo passe E passe: o estado compartilhado nao existe
    // como objeto.)
    //
    // Aqui a separacao e por TIPO, nao por flag: as duas sobrecargas de FPassRegistry::Register
    // classificam na resolucao de sobrecarga, entao nao da para registrar na lista errada sem o
    // compilador reclamar, e ninguem precisa lembrar de marcar um bool.

    // Possui shader e PSO. E o que o hot reload precisa saber, e o teto do que um baker ou um
    // saco de estado compartilhado tem a oferecer.
    class FPipelineOwner {
    public:
        virtual ~FPipelineOwner() = default;

        // Rotulo unico, para log de reload e para a UI de debug.
        virtual const char* Name() const = 0;

        // Os RECURSOS existem. Nao confundir com IsActive (que so faz sentido para um passe de
        // frame): estar inteiramente construido nao implica rodar neste frame.
        virtual bool IsInitialized() const = 0;

        // Recompila os PSOs deste dono. Unifica Recreate/RecreatePSO/RecreatePSOs/
        // RecreatePipelines. Contrato: no-op se ainda nao foi inicializado.
        virtual void OnRecreatePipelines(const FPassInitContext& Ctx) { (void)Ctx; }

        // Stems (nome do .cso sem perfil nem extensao) que disparam o OnRecreatePipelines.
        // Vazio = nao participa do hot reload. Permutacoes contam: os alvos gerados por
        // OUTPUT_NAME no Shaders/CMakeLists.txt nao tem .hlsl proprio e ainda assim recarregam.
        virtual FPassShaderStems ShaderStems() const { return {}; }
    };

    // Um passe que RODA NO FRAME. Acrescenta o que so tem sentido para quem grava: participar de
    // um frame, ser redimensionado com os alvos de cena e ter historico temporal a invalidar.
    //
    // So CICLO DE VIDA — deliberadamente. NAO existe `virtual void Execute()`. A tentacao e obvia
    // (um vetor de passes, um laco, um frame graph), e as tres engines de referencia recusaram a
    // mesma coisa:
    //
    //   - Cry: CGraphicsPipelineStage tem Init/Resize/Update/IsStageActive virtuais, mas o
    //     Execute de cada stage tem a assinatura que precisar, e CStandardGraphicsPipeline::
    //     Execute() e uma LISTA LEGIVEL de chamadas explicitas com o gate IsStageActive.
    //   - Flax: RendererPassBase tem Init/Dispose/setupResources; render e ShadowsPass::Render
    //     (contexto, luz, ...), fora da base.
    //   - Unreal: so o RDG uniformiza, e ao preco de todo parametro virar struct declarativa
    //     com macro de reflexao. La isso se paga porque o grafo CULLA e faz barreira sozinho.
    //
    // Uniformizar o Execute aqui obrigaria o FPassContext a carregar tudo que qualquer passe
    // pode querer — ou seja, moveria o God object em vez de desmonta-lo. A ordem dos passes
    // da Smile e quase estatica e os alvos sao longevos (FSceneTargets), entao o que o RDG
    // vende (culling de passe, aliasing de transiente) nao tem comprador aqui.
    //
    // O que se ganha uniformizando o RESTO e concreto: um registro que recria pipeline, faz
    // resize e invalida historico sem saber o nome de ninguem.
    class FRenderPass : public FPipelineOwner {
    public:
        // Roda NESTE frame. O default cobre o caso comum ("pronto = roda"); sobrescreva quando
        // a decisao depender do frame. O TOGGLE do usuario continua fora daqui de proposito:
        // ele mora no FRenderSettings e ja chega resolvido no FFrameModes, e nao ha ganho em
        // duplicar essa fonte de verdade dentro de cada passe.
        virtual bool IsActive(const FFrameModes& Modes) const {
            (void)Modes;
            return IsInitialized();
        }

        // Resolucao de render mudou (resize da janela, render scale, modo do upscaler).
        // Unifica Resize e SetupForResize.
        virtual void OnResize(const FPassInitContext& Ctx) { (void)Ctx; }

        // Historicos temporais que este passe possui. O registro so o notifica quando o
        // dominio invalidado intersecta esta mascara — o grafo em si ja e dado (HistoryDomain.h).
        virtual EHistoryTarget HistoryTargets() const { return EHistoryTarget::None; }

        // Unifica Invalidate/InvalidateHistory/ResetHistory/ResetHistoryOnce/InvalidateResults.
        // Recebe o dominio inteiro: um passe com mais de um historico decide quais derrubar.
        virtual void OnInvalidateHistory(EHistoryTarget Domain) { (void)Domain; }
    };

    // O registro. Ponteiros NAO-donos — os passes continuam sendo membros por VALOR do Renderer,
    // com a ordem de construcao e destruicao que sempre tiveram. Isto aqui e um indice sobre eles,
    // nao uma mudanca de posse: e o que torna a adocao incremental, um por vez, com a engine verde
    // no meio do caminho.
    //
    // DUAS listas. `Owners` e o universo do hot reload (inclui os bakers e o estado compartilhado);
    // `Passes` e o subconjunto que roda no frame, e e ele que resize, invalidacao de historico e
    // qualquer futura UI de custo/toggle enxergam. Registrar um FRenderPass* alimenta as duas, um
    // FPipelineOwner* so a primeira — pela sobrecarga, sem flag e sem cast.
    class FPassRegistry {
    public:
        // Passe de frame: entra nas duas listas.
        void Register(FRenderPass* Pass) {
            if (!Pass) return;
            Passes.push_back(Pass);
            Owners.push_back(Pass);
        }

        // Baker ou estado compartilhado: participa do reload e nada mais. A sobrecarga acima
        // ganha para qualquer FRenderPass* (conversao mais derivada), entao passar um passe aqui
        // por engano nao compila silenciosamente errado — ele simplesmente vai pra sobrecarga certa.
        void Register(FPipelineOwner* Owner) {
            if (Owner) Owners.push_back(Owner);
        }

        void Clear() { Passes.clear(); Owners.clear(); }

        // So os que RODAM no frame. E a lista que a UI de custo/toggle deve consumir.
        const std::vector<FRenderPass*>& FramePasses() const { return Passes; }
        // Tudo que tem pipeline, passe ou nao.
        const std::vector<FPipelineOwner*>& PipelineOwners() const { return Owners; }

        // Reload COMPLETO: todo dono de pipeline, incluindo os que nao gravam frame.
        void RecreateAllPipelines(const FPassInitContext& Ctx) const {
            for (FPipelineOwner* O : Owners) O->OnRecreatePipelines(Ctx);
        }

        // Recria os pipelines de TODOS os passes que reivindicam este stem. Devolve quantos
        // casaram; 0 = ninguem, e o chamador reporta "sem pipeline mapeado".
        //
        // Casa TODOS, e nao o primeiro, porque um mesmo shader pode ter mais de um dono legitimo:
        // as tres cascatas de FFT do oceano sao tres instancias do mesmo passe compilando os
        // mesmos .cso. Parar no primeiro deixaria duas cascatas com o PSO velho — um bug que so
        // apareceria como uma escala de onda se comportando diferente das outras.
        u32 RecreatePipelinesForStem(std::string_view Stem, const FPassInitContext& Ctx) const {
            u32 Matched = 0;
            for (FPipelineOwner* O : Owners) {
                for (const char* S : O->ShaderStems()) {
                    if (Stem != S) continue;
                    O->OnRecreatePipelines(Ctx);
                    ++Matched;
                    break; // um dono conta uma vez, mesmo que liste o stem duas
                }
            }
            return Matched;
        }

        // Nome do primeiro dono do stem, so para o log. nullptr quando ninguem reivindica.
        const char* OwnerOfShaderStem(std::string_view Stem) const {
            for (FPipelineOwner* O : Owners)
                for (const char* S : O->ShaderStems())
                    if (Stem == S) return O->Name();
            return nullptr;
        }

        // Resize e invalidacao de historico so fazem sentido para quem grava frame.
        void ResizeAll(const FPassInitContext& Ctx) const {
            for (FRenderPass* P : Passes) P->OnResize(Ctx);
        }

        void InvalidateHistory(EHistoryTarget Domain) const {
            for (FRenderPass* P : Passes)
                if (HasTarget(Domain, P->HistoryTargets())) P->OnInvalidateHistory(Domain);
        }

    private:
        std::vector<FRenderPass*>    Passes; // subconjunto que grava frame
        std::vector<FPipelineOwner*> Owners; // universo do hot reload
    };
}
