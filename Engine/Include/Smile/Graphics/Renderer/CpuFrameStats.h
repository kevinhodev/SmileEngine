#pragma once

#include "Smile/Core/Types.h"
#include <chrono>

namespace Smile {
    // Telemetria de CPU do frame: tempos de preparacao/submissao e contadores de trabalho.
    // Fase 0 do Docs/GPU-DRIVEN-PLAN.md — sem ela nao ha baseline contra a qual medir o
    // renderer GPU-driven, e "ficou mais rapido" nao seria verificavel.
    //
    // REGRA DE PROCEDENCIA (a mesma do manifesto de captura, CAPTURE-PROTOCOL.md): cada numero
    // vem de ONDE A DECISAO E TOMADA, nunca recomposto por fora. Contagem de draw e incrementada
    // no FDrawSubmitCache, no ponto exato da submissao — recompor a partir de VisibleScratch
    // daria um numero plausivel e ERRADO, porque cada passe filtra o seu (Blend fora do prepass,
    // alpha-test em PSO proprio, caster cullado por cascata) e a divergencia seria silenciosa.

    // Um balde de submissao. Vive por passe e por frame; incrementado no ponto de submissao.
    struct FDrawSubmitStats {
        u32 Considered    = 0; // itens que o laco do passe visitou (antes do filtro dele)
        u32 Draws         = 0; // DrawIndexedInstanced REALMENTE emitidos
        u32 MaterialBinds = 0; // Bind de material que o cache NAO pulou
        u32 MeshBinds     = 0; // IASetVertexBuffers/IASetIndexBuffer que o cache NAO pulou
        u32 SkippedInvalid = 0; // mesh nulo ou sem indices (nao vira draw nem bind)

        // --- Material bindless (Fase 3) -------------------------------------------------------
        //
        // TRES estados, nao dois. Um draw ou leu material pela tabela canonica (`BindlessDraws`),
        // ou pediu material pelo caminho classico (`MaterialFallbackDraws`), ou NAO CONSOME
        // MATERIAL — e o terceiro caso e maioria em passes inteiros.
        //
        // `Draws - BindlessDraws` seria a conta errada, e mentiria para o lado ruim: o z-prepass
        // opaco desenha sem tocar em material nenhum e nunca pode incrementar `BindlessDraws`,
        // entao todo opaco da cena apareceria como "fallback" e contaminaria justamente o numero
        // usado para julgar o A/B. O mesmo vale por DRAW nas sombras e na chuva, onde so os
        // casters alpha-tested amarram material.
        //
        // Por isso o fallback e contado no ponto da SUBMISSAO, como todo o resto deste balde, e
        // so quando aquele item pediu material.
        //
        // `MaterialRowSets` e o par de `MaterialBinds`: quantas vezes a constante de root com a
        // linha do material foi REESCRITA, depois do filtro do cache. No A/B a leitura e direta —
        // classico deixa MaterialBinds=N e MaterialRowSets=0; bindless inverte, com o mesmo N.
        u32 BindlessDraws         = 0;
        u32 MaterialFallbackDraws = 0;
        u32 MaterialRowSets       = 0;
        // Par da Fase 4, e a mesma leitura: `TransformBinds` conta os CBV de 256 B amarrados por
        // item no caminho classico, `TransformSlotSets` conta as constantes de root escritas no
        // bindless. Num passe migrado o primeiro tem de ir a ZERO — e ele que sustenta o
        // `objectConstantBytes == 0` do gate, porque um call site esquecido continuaria amarrando
        // o CBV sem que a imagem mudasse.
        u32 TransformBinds        = 0;
        u32 TransformSlotSets     = 0;
        // Chamadas ExecuteIndirect gravadas para este passe. Draws continua sendo apenas draws
        // diretos do fallback CPU; a contagem de comandos vem atrasada dos contadores da cadeia.
        u32 IndirectCalls         = 0;
        // Material que chegou ao draw sem linha na tabela canonica. TEM de ser zero: `RowOf`
        // devolve a linha 0 para desconhecido, e a linha 0 e "renderavel sem material" — cinza
        // 0,7, sem textura. Sem este contador, um material fora da tabela nao daria erro nenhum:
        // daria um objeto cinza no caminho bindless e correto no classico, que e a diferenca
        // exatamente do tipo que a Fase 3 existe para nao produzir.
        u32 MaterialRowMisses = 0;

        void Reset() { *this = FDrawSubmitStats{}; }
    };

    // Passes que submetem geometria de cena. Fixo (sem alocacao por frame) e exaustivo em
    // relacao aos usuarios de FDrawSubmitCache — se aparecer um passe novo que submete malha,
    // ele entra aqui, senao o total do frame passa a mentir por omissao.
    enum class ESubmitPass : u32 {
        ZPrepassOpaque = 0,
        ZPrepassMasked,
        GBuffer,
        Translucent,
        SunShadows,
        LocalShadows,
        RainOcclusion,
        // Terreno: NAO passa pelo FDrawSubmitCache (tem VB/IB e root proprios) e desenha em
        // varios passes — prepass, G-buffer e como caster extra das sombras. Balde unico de
        // proposito: depois do GPU-driven ele e exatamente o custo residual que sobrou fora
        // do stream indireto, e separa-lo por passe esconderia isso.
        TerrainFallback,
        Count
    };

    // Chave ESTAVEL, em ingles, para consumidores de maquina (MCP). Rotulo traduzido pertence
    // ao QML: se a automacao dependesse do texto da UI, renomear uma linha do painel quebraria
    // benchmark, e teriamos duas fontes de verdade.
    inline const char* SubmitPassKey(ESubmitPass _Pass) {
        switch (_Pass) {
            case ESubmitPass::ZPrepassOpaque:  return "zPrepassOpaque";
            case ESubmitPass::ZPrepassMasked:  return "zPrepassMasked";
            case ESubmitPass::GBuffer:         return "gbuffer";
            case ESubmitPass::Translucent:     return "translucent";
            case ESubmitPass::SunShadows:      return "sunShadows";
            case ESubmitPass::LocalShadows:    return "localShadows";
            case ESubmitPass::RainOcclusion:   return "rainOcclusion";
            case ESubmitPass::TerrainFallback: return "terrainFallback";
            default:                           return "unknown";
        }
    }

    inline const char* SubmitPassName(ESubmitPass _Pass) {
        switch (_Pass) {
            case ESubmitPass::ZPrepassOpaque: return "Z-prepass opacos";
            case ESubmitPass::ZPrepassMasked: return "Z-prepass mascarados";
            case ESubmitPass::GBuffer:        return "G-buffer";
            case ESubmitPass::Translucent:    return "Translucidos";
            case ESubmitPass::SunShadows:     return "CSM (sol)";
            case ESubmitPass::LocalShadows:   return "Sombras locais";
            case ESubmitPass::RainOcclusion:  return "Oclusao de chuva";
            case ESubmitPass::TerrainFallback: return "Terreno (fora do cache)";
            default:                          return "?";
        }
    }

    // Acumula o tempo de vida do escopo num f64 externo (ms). Existe porque SubmitMs e a SOMA de
    // varios lacos espalhados: sem um acumulador, cada laco precisaria de um par de now() a mao e
    // o primeiro esquecido publicaria "custo zero" — que e pior que campo ausente, porque parece
    // uma medicao.
    class FCpuScopeTimer {
    public:
        explicit FCpuScopeTimer(f64& _AccumMs)
            : AccumMs(_AccumMs), Start(std::chrono::steady_clock::now()) {}
        ~FCpuScopeTimer() {
            AccumMs += std::chrono::duration<f64, std::milli>(
                           std::chrono::steady_clock::now() - Start).count();
        }
        FCpuScopeTimer(const FCpuScopeTimer&)            = delete;
        FCpuScopeTimer& operator=(const FCpuScopeTimer&) = delete;
    private:
        f64& AccumMs;
        std::chrono::steady_clock::time_point Start;
    };

    struct FCpuFrameStats {
        static constexpr u32 kPassCount = static_cast<u32>(ESubmitPass::Count);

        // --- Tempos de CPU (ms). Preparacao e submissao separadas de proposito: o GPU-driven
        // ataca a SUBMISSAO, e misturar as duas esconderia exatamente o ganho que se quer medir.
        f64 BuildDrawListsMs = 0.0; // laco da cena: ObjectConstants, selecao, bounds do HiZ
        f64 CullSortMs       = 0.0; // frustum + filtro de oclusao + sort front-to-back
        // Classificacao por lote do G-buffer (filtro de Blend + sort por two-sided/material/mesh).
        // Campo PROPRIO porque e O(N log N) e e exatamente o trabalho que a Fase 5 remove ao
        // agrupar por batch na GPU: diluido em CullSortMs ou em SubmitMs, o ganho ficaria
        // invisivel justamente na metrica que deveria prova-lo.
        f64 BatchSortMs      = 0.0;
        f64 ShadowListsMs    = 0.0; // montagem das listas de caster (CSM + locais)
        f64 SubmitMs         = 0.0; // soma dos lacos de submissao instrumentados

        // --- Cena (o denominador de tudo acima)
        u32 Renderables      = 0; // total na cena
        u32 DrawItems        = 0; // aceitos no frame (visivel, malha valida, nao so-RT)
        u32 VisibleItems     = 0; // itens da lista CPU; no culling GPU, so fallback (+ picking sob demanda)
        u32 OccludedItems    = 0;
        u32 Materials        = 0;
        u32 Meshes           = 0;
        u64 ObjectConstantBytes = 0; // escritos no CB por frame (DrawItems * sizeof)
        // RESERVADOS pelo ObjectCB, e nao escritos. Duas provas separadas do mesmo gate: o de
        // cima cai a zero quando ninguem mais ESCREVE, este quando o buffer deixa de EXISTIR. Um
        // caminho pode parar de escrever com a memoria ainda alocada, e foi assim durante seis
        // das sete migracoes desta fase. Upload heap nao e itemizado pelo VramTracker (que cobre
        // VRAM/DEFAULT), entao o numero vem do proprio renderer.
        u64 ObjectConstantBytesReserved = 0;

        // --- Descriptor heap: ocupacao e high-water. O bindless da Fase 3 consome deste
        // orcamento, entao a folga precisa ser observavel ANTES de se contar com ela.
        u32 SrvHeapUsed      = 0;
        u32 SrvHeapHighWater = 0;
        u32 SrvHeapCapacity  = 0;
        // Presos em ranges aposentados esperando as duas fences (Fase 3), e alocacoes
        // RECUSADAS por falta de espaco. `Used + Retired` e o que de fato nao esta disponivel;
        // os dois separados porque so um deles cresce por vazamento de coleta.
        u32 SrvHeapRetired   = 0;
        u32 SrvHeapOverflows = 0;

        // Pool de residencia GPU (FGpuSceneSlots). Publicado porque o modo de falha dele e
        // CRESCIMENTO SILENCIOSO: um Collect que deixasse de rodar faria o pool subir a cada
        // ciclo remover->adicionar sem nada quebrar, e so apareceria como memoria a mais meses
        // depois. Aposentados presos e capacidade acima dos vivos sao o sintoma.
        u32 GpuSlotsCapacity  = 0;
        u32 GpuSlotsLive      = 0;
        u32 GpuSlotsRetired   = 0;
        u32 GpuSlotsExhausted = 0;

        // Tabelas canonicas residentes (Fase 2). Duas grandezas com perguntas diferentes:
        //
        //  - PERSISTENTE responde "quanto a cena custa em tabela"; e o eixo que a Fase 4 vai
        //    multiplicar quando instancias e meshes tambem virarem tabela.
        //  - SUJO POR FRAME responde "o caminho estavel esta reescrevendo tabela?". Tem de ser
        //    ZERO num frame sem edicao, e esse zero e o gate da fase. O modo de falha e um
        //    caminho que suje a tabela por frame — o que devolveria, disfarcado de upload
        //    incremental, o custo por frame que a fase existe para remover.
        //
        // O acumulado existe porque o instantaneo e quase sempre zero: um vazamento de um punhado
        // de linhas por frame nao aparece no instantaneo e aparece na soma.
        u64 MaterialTableBytes       = 0; // VRAM: capacidade x 116 B (64 do RT + 48 do raster + classe)
        u64 MaterialStagingBytes     = 0; // host: os slices de staging, um por frame em voo
        u32 MaterialTableRows        = 0; // linhas em uso (1 default + materiais da cena)
        u32 MaterialRowsDirty        = 0; // linhas subidas NESTE frame
        u32 MaterialCopyRegions      = 0; // CopyBufferRegion emitidos neste frame
        u64 MaterialRowsUploadedTotal = 0; // desde o ultimo setup da tabela

        // Tabela canonica de meshes. Sem contador de sujas por frame: geometria nao e editavel em
        // runtime, entao as linhas dela so mudam num Setup — e o dia em que passarem a mudar por
        // frame e o dia em que este par de numeros precisa crescer.
        // Rebake de micro-map em runtime (Fase 2). Retidos = UV/indices que a cena segura para
        // poder rebakear; refeitos = VRAM que os BLAS reconstruidos FORA do pool acrescentaram.
        // Os dois sao custos novos e permanentes dentro da sessao, e crescimento silencioso e o
        // modo de falha dos dois.
        u64 OmmRetainedBytes  = 0;
        u64 OmmRebuiltBlasBytes = 0;
        u32 OmmRebuiltBlasCount = 0;

        u64 MeshTableBytes    = 0;
        u64 MeshStagingBytes  = 0;
        u32 MeshTableRows     = 0;

        // Tabela canonica de instancias, indexada por slot estavel. Diferente da tabela de
        // materiais, uma transformacao legitima suja uma linha; num frame estavel os tres
        // contadores instantaneos abaixo tem de ser zero. BytesDirty soma as quatro colunas,
        // enquanto RowsDirty conta cada slot no maximo uma vez.
        u64 InstanceTableBytes        = 0;
        u64 InstanceStagingBytes      = 0;
        u32 InstanceTableRows         = 0;
        u32 InstanceRowsDirty         = 0;
        u64 InstanceBytesDirty        = 0;
        u32 InstanceCopyRegions       = 0;
        u64 InstanceBytesUploadedTotal = 0;
        // Slots que o diff COMPAROU para chegar aos sujos acima (Fase 4). Separa "upload
        // incremental" de "trabalho incremental": antes da Fase 4 as linhas sujas ja eram poucas,
        // mas descobri-las custava a CAPACIDADE da tabela — quatro colunas x 1,06M linhas na
        // sintetica de 1M, por frame, enquanto o gizmo arrastasse. O modo de falha e este voltar a
        // acompanhar `InstanceTableRows` em vez de `InstanceRowsDirty`.
        u32 InstanceRowsScanned       = 0;

        // Fase 5. `Active` e a decisao CPU tomada antes da cadeia; os contadores `Gpu*` chegam
        // atrasados do readback e nunca decidem draw. `Commands` e a capacidade estavel do layout,
        // enquanto `GpuIndirectCommands` e quantos candidatos o classify aceitou na ultima cadeia
        // concluida — separar os dois impede chamar capacidade de trabalho executado.
        bool GpuIndirectMainViewActive = false;
        u64  GpuIndirectChainsRecorded = 0;
        u32  GpuIndirectLastReason     = 0;
        u32  GpuIndirectLastDetail     = 0;
        u32  GpuIndirectOutputs        = 0;
        u32  GpuIndirectCommands       = 0;
        bool GpuIndirectCountersValid  = false;
        u32  GpuIndirectStatus         = 0;
        u32  GpuIndirectGpuReason      = 0;
        u32  GpuIndirectGpuCommands    = 0;
        u32  GpuFrustumRejected        = 0;
        u32  GpuOcclusionRejected      = 0;
        u64  GpuIndirectResidentBytes  = 0;
        u64  GpuIndirectStagingBytes   = 0;
        u64  GpuIndirectUploadBytes    = 0;
        u64  GpuIndirectReadbackBytes  = 0;

        // Frame de onde esta amostra saiu. O FrameIndex geral ja avancou quando o MCP le o
        // ultimo frame FECHADO, entao sem identidade propria uma automacao correlacionaria CPU
        // e GPU de frames diferentes sem perceber.
        u64 SourceFrameIndex = 0;

        FDrawSubmitStats Passes[kPassCount];

        void ResetFrame() {
            BuildDrawListsMs = CullSortMs = BatchSortMs = ShadowListsMs = SubmitMs = 0.0;
            DrawItems = VisibleItems = OccludedItems = 0;
            ObjectConstantBytes = 0;
            GpuIndirectMainViewActive = false;
            for (u32 i = 0; i < kPassCount; ++i) Passes[i].Reset();
        }

        FDrawSubmitStats& Pass(ESubmitPass _P) { return Passes[static_cast<u32>(_P)]; }
        const FDrawSubmitStats& Pass(ESubmitPass _P) const { return Passes[static_cast<u32>(_P)]; }

        // ⚠️ NAO e "draws do frame": e a soma dos baldes INSTRUMENTADOS. Cobre a geometria de
        // malha de cena (via FDrawSubmitCache) e o terreno. Ficam DE FORA: agua, particulas,
        // ceu/atmosfera, helpers do editor, picking e contorno de selecao. Chamar isso de total
        // faria a UI publicar um numero menor que a realidade e a Fase 5 mediria contra uma
        // referencia errada. O nome e longo de proposito.
        u32 InstrumentedSceneMeshDraws() const {
            u32 Sum = 0;
            for (u32 i = 0; i < kPassCount; ++i) Sum += Passes[i].Draws;
            return Sum;
        }
        u32 InstrumentedMaterialBinds() const {
            u32 Sum = 0;
            for (u32 i = 0; i < kPassCount; ++i) Sum += Passes[i].MaterialBinds;
            return Sum;
        }
        // Draws que CONSOMEM MATERIAL e ainda o amarram por item. E o numero do gate da fase, e
        // nao "todo draw que nao foi bindless": draw sem material — o z-prepass opaco inteiro —
        // nao e fallback de material e nao entra.
        //
        // O terreno tambem nao entra, e por outro motivo: ele nem passa pelo FDrawSubmitCache. O
        // fallback DELE e outro eixo (submissao, nao material) e ja tem balde proprio,
        // TerrainFallback.
        u32 InstrumentedMaterialFallbackDraws() const {
            u32 Sum = 0;
            for (u32 i = 0; i < kPassCount; ++i) Sum += Passes[i].MaterialFallbackDraws;
            return Sum;
        }
    };
}
