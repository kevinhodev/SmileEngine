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
        // `BindlessDraws` conta os draws cujo pixel shader leu o material da tabela canonica.
        // O FALLBACK, que o gate da fase manda tornar explicito e contado, e a diferenca:
        // `Draws - BindlessDraws`. Publicado como diferenca em vez de um terceiro contador porque
        // os dois se somam por construcao — um contador proprio poderia divergir dos outros dois.
        //
        // `MaterialRowSets` e o par de `MaterialBinds`: quantas vezes a constante de root com a
        // linha do material foi REESCRITA, depois do filtro do cache. No A/B a leitura e direta —
        // classico deixa MaterialBinds=N e MaterialRowSets=0; bindless inverte, com o mesmo N.
        u32 BindlessDraws   = 0;
        u32 MaterialRowSets = 0;
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
        u32 VisibleItems     = 0; // sobreviventes de frustum + oclusao
        u32 OccludedItems    = 0;
        u32 Materials        = 0;
        u32 Meshes           = 0;
        u64 ObjectConstantBytes = 0; // escritos no CB por frame (DrawItems * sizeof)

        // --- Descriptor heap: ocupacao e high-water. O bindless da Fase 3 consome deste
        // orcamento, entao a folga precisa ser observavel ANTES de se contar com ela.
        u32 SrvHeapUsed      = 0;
        u32 SrvHeapHighWater = 0;
        u32 SrvHeapCapacity  = 0;

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
        u64 MaterialTableBytes       = 0; // VRAM: capacidade x 112 B (64 lidos pelo RT + 48 do raster)
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

        // Frame de onde esta amostra saiu. O FrameIndex geral ja avancou quando o MCP le o
        // ultimo frame FECHADO, entao sem identidade propria uma automacao correlacionaria CPU
        // e GPU de frames diferentes sem perceber.
        u64 SourceFrameIndex = 0;

        FDrawSubmitStats Passes[kPassCount];

        void ResetFrame() {
            BuildDrawListsMs = CullSortMs = BatchSortMs = ShadowListsMs = SubmitMs = 0.0;
            DrawItems = VisibleItems = OccludedItems = 0;
            ObjectConstantBytes = 0;
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
        // Draws de malha de cena que NAO passaram pelo caminho bindless. Mesmo escopo do
        // InstrumentedSceneMeshDraws (so os baldes instrumentados), e por isso o terreno entra:
        // ele e fallback de verdade e o balde dele nunca tem BindlessDraws.
        u32 InstrumentedFallbackDraws() const {
            u32 Sum = 0;
            for (u32 i = 0; i < kPassCount; ++i)
                Sum += Passes[i].Draws - Passes[i].BindlessDraws;
            return Sum;
        }
    };
}
