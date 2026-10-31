#pragma once

// Classificacao, regioes, scatter e finalizacao do main view (Fase 5 do GPU-DRIVEN-PLAN, passo 4
// da secao 6.2).
//
// Entrada: a lista de slots que o `ExpandInstanceRanges` produziu. Saida: os argumentos indiretos de
// cada LOTE, escritos dentro da REGIAO que a CPU reservou para ele, o count buffer que diz quantos
// deles o frame desenha, e a contabilidade de todo candidato que nao virou comando.
//
// Nada aqui tem consumidor de producao ainda: a gravacao do `ExecuteIndirect` e a proxima fatia.
//
// ==============================================================================================
// QUEM DESENHA O QUE, que e a decisao que este arquivo carrega
// ==============================================================================================
//
// A regra 7 do plano proibe readback novo decidindo draw, entao a GPU NAO pode dizer a CPU quem ela
// recusou — nem neste frame nem no seguinte. Logo a divisao "indireto x fallback" e decidida pela
// CPU, com o MESMO predicado (`GpuIndirectEligible`), sobre dados que ela tambem possui: a classe do
// material e a flag de offset da malha. A CPU desenha o complemento.
//
// E por isso as PRE-CONDICOES do dispatch sao validadas ANTES dele, pela CPU, com dados que sao
// dela: ver `GpuClassifyPreflight`. Preflight reprovado tira o frame INTEIRO do caminho indireto,
// antes de qualquer comando ser gravado. Nao ha decisao de roteamento depois do dispatch.
//
// Isso parte os desfechos em duas naturezas, e confundi-las e o erro que custa geometria:
//
//   POLITICA  — `NotEligible` e `MeshUnusable`. Recusas previstas, e a CPU desenha esses
//               renderaveis pelo caminho classico. Crescem normalmente (masked, two-sided e Blend
//               existem em toda cena). Os dois lados avaliam o MESMO predicado sobre a MESMA linha,
//               entao a recusa da GPU e o desenho da CPU sao o mesmo fato visto de dois lugares.
//
//   CANARIO   — `NoSlot`, `InvalidSlot`, `InvalidMaterialId`, `InvalidMeshId`, `InvalidPoolId` e
//               `Overflow`. Significam invariante quebrado: as tabelas discordam do que a CPU acha
//               que existe. Nunca sao entrada de roteamento — "voltar para a CPU no frame seguinte"
//               seria justamente a decisao por readback que a regra 7 proibe —, e por isso a
//               FINALIZACAO invalida o frame inteiro quando qualquer um aparece, no mesmo frame e
//               dentro da GPU: todo o `DrawCounts` sai zerado. Os canarios do EXPAND (slot invalido,
//               RaytracingOnly alcancado, overflow da saida) tambem.
//
//               A politica e uma lista FECHADA (`GpuClassifyOutcomeIsCanary`). Um desfecho novo nasce
//               canario ate alguem declara-lo politica: o erro por omissao zera o frame, e nao some
//               com geometria.
//
//               `Overflow` e o unico que nao deveria ser alcancavel: o preflight exige
//               `BatchCapacity >= GpuBatchSlotCount(PoolCount)`, e com isso o indice do lote esta
//               limitado por construcao. Ele continua no shader como defesa de ultima instancia, e
//               nao como politica — descobrir falta de espaco depois do dispatch ja e geometria
//               perdida.
//
// ==============================================================================================
// O QUE A SOMA DOS DESFECHOS PROVA, E O QUE ELA NAO PROVA
// ==============================================================================================
//
// A soma dos contadores e igual ao numero de candidatos. Isso prova UM RESULTADO DE
// CLASSIFICACAO por candidato — nenhum caminho esquece de contar, nenhum conta duas vezes.
//
// NAO prova `draw indireto + draw CPU == 1`. Canario e "quem desenha" nao sao a mesma particao, e o
// contraexemplo e curto:
//
//   - malha com `GeometryPoolId` invalido sob material MASKED: a GPU registra `InvalidPoolId` e nao
//     emite comando; a CPU desenha assim mesmo, porque a classe nao e elegivel. Desenhado uma vez —
//     por acaso, e nao porque as duas contas fecharam;
//
//   - a MESMA malha sob material opaco ELEGIVEL: a GPU registra `InvalidPoolId` e nao emite; a CPU
//     tambem nao desenha, porque a classe e elegivel. Desenhado ZERO vezes.
//
// Generalizando: a CPU desenha `!GpuIndirectEligible(...)` e a GPU emite
// `GpuIndirectEligible(...) E ids validos`. O conjunto exposto e portanto `elegivel E canario`, e
// ninguem o desenha. Pior: nos canarios de id (`InvalidMaterialId`, `InvalidSlot`) a classe nem
// chega a ser conhecida, entao nao da para saber se o candidato estava exposto — o que obriga a
// tratar QUALQUER canario como possivel geometria perdida. E e o que a finalizacao faz: um frame que
// fechasse com os outros lotes desenhando esconderia a perda parcial; zerado, o defeito aparece.
//
// A prova de "desenhado exatamente uma vez" so pode existir quando o fallback e o scatter
// existirem, e exige que os dois bracos leiam a MESMA versao das linhas canonicas. Ela pertence
// aquela slice; aqui ela seria encenacao.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Scene/GpuIndirectDraw.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Smile {
    // Um desfecho por candidato, exatamente um. Espelhado em
    // Shaders/GpuScene/ClassifyCandidates.cs.hlsl como SMILE_CLASSIFY_*; renumerar um lado sem o
    // outro nao quebra compilacao nenhuma, e o teste WARP le os indices de volta.
    //
    // Separados, e nao somados num "rejeitados", pela mesma razao do FGpuCandidateStats: as causas
    // tem consequencias diferentes. `NotEligible` crescendo e a cena ter folhagem; `InvalidMeshId`
    // crescendo e defeito.
    enum EGpuClassifyOutcome : u32 {
        GpuClassifyIndirect          = 0, // virou comando; contado tambem no lote
        GpuClassifyNoSlot            = 1, // posicao sem candidato (kGpuExpandNoSlot) — CANARIO
        GpuClassifyInvalidSlot       = 2, // slot fora da tabela, ou linha sem ocupante — CANARIO
        GpuClassifyInvalidMaterialId = 3, // CANARIO
        GpuClassifyInvalidMeshId     = 4, // CANARIO
        GpuClassifyInvalidPoolId     = 5, // pool fora da lista publicada — CANARIO
        GpuClassifyNotEligible       = 6, // classe fora do indireto nesta fase — POLITICA
        // A linha da malha nao serve ao indireto: qualquer bit de kGpuMeshIndirectBlocked. Cobre
        // offset irrepresentavel E IndexCount que nao descreve triangulos inteiros — os dois saem
        // pelo mesmo lugar porque a porta e uma mascara so. POLITICA.
        GpuClassifyMeshUnusable   = 7,
        GpuClassifyOverflow          = 8, // lote fora da capacidade de BatchCounts — CANARIO
        GpuClassifyFrustumRejected   = 9,
        GpuClassifyOcclusionRejected = 10,
        GpuClassifyOutcomeCount      = 11,
    };

    // CANARIO e tudo o que NAO esta na lista fechada dos desfechos que mantem o frame: o comando e as
    // recusas de politica e de visibilidade. Um desfecho acrescentado sem
    // passar por aqui nasce canario. Espelhado na finalizacao (FinalizeDrawCounts.cs.hlsl).
    inline bool GpuClassifyOutcomeIsCanary(u32 _Outcome) {
        return _Outcome != GpuClassifyIndirect && _Outcome != GpuClassifyNotEligible &&
               _Outcome != GpuClassifyMeshUnusable && _Outcome != GpuClassifyFrustumRejected &&
               _Outcome != GpuClassifyOcclusionRejected;
    }

    // O lote a que um comando pertence.
    //
    // `PassClass` NAO entra: ele e eixo do PASSE, e o passe e quem faz o dispatch — carrega-lo no
    // lote daria o produto cartesiano das duas familias dentro de um buffer que ja e por passe.
    //
    // `GeometryPoolId` entra porque um `ExecuteIndirect` amarra UM par VB/IB: lote nao cruza pool.
    struct FGpuBatchKey {
        u32 PipelineClass  = 0;
        u32 GeometryPoolId = 0;
    };

    // O que um candidato INDIRETO produz: o lote e o COMANDO INTEIRO.
    //
    // O comando inteiro, e nao ids, pela mesma razao do espelho HLSL: com so `MeshId` na mao, o
    // scatter precisaria reler a linha de malha para achar contagem e offsets, e essa leitura e
    // exatamente onde uma recusa a mais caberia depois de o classify ja ter contado o candidato.
    struct FGpuClassifyCommand {
        u32                  Batch = 0;
        FGpuIndirectDrawArgs Args{};
    };

    // Indice denso do lote em BatchCounts. CLASS-MAJOR: os lotes de uma classe ficam contiguos, que
    // e a ordem em que os `ExecuteIndirect` de uma classe vao ler os argumentos.
    //
    // As quatro classes ocupam espaco mesmo com so uma elegivel nesta fase. Custa unidades de dwords
    // (pools sao chunks de 256 MB, nao milhares), poupa uma troca de formato na Fase 7, e da ao
    // teste uma asseracao util: as faixas das classes 1..3 tem de ficar em ZERO, o que prova que a
    // elegibilidade barrou de fato, em vez de o dado nao ter aparecido.
    //
    // ⚠️ A aritmetica e u32 e so e segura DEPOIS de o preflight aprovar. Sob preflight aprovado o
    // maior indice possivel e `(kGpuPipelineClassCount - 1) * PoolCount + (PoolCount - 1)`, que e
    // exatamente `GpuBatchSlotCount(PoolCount) - 1` — e o preflight ja garantiu que aquele total
    // cabe em u32. Chamada com um `PoolCount` que nao passou por ele, esta conta pode dar a volta.
    inline u32 GpuBatchIndex(const FGpuBatchKey& _Key, u32 _PoolCount) {
        return _Key.PipelineClass * _PoolCount + _Key.GeometryPoolId;
    }

    // Quantos slots `BatchCounts` precisa ter, em u64 e sem truncar.
    //
    // O u64 nao e zelo. `kGpuPipelineClassCount * PoolCount` em u32 da a volta a partir de
    // `PoolCount == 2^30`, e da a volta para ZERO — que passa por QUALQUER capacidade. O preflight
    // compararia contra o produto truncado e prometeria overflow inalcancavel enquanto o tornava
    // alcancavel, que e o oposto do que ele existe para fazer.
    //
    // Essa quantidade de pool e fisicamente inviavel hoje (cada pool e um chunk de ate 256 MB), e e
    // justamente por isso que a checagem tem de morar aqui: esta funcao e o CONTRATO DE
    // DIMENSIONAMENTO, e quem aloca o buffer vai chama-la sem revisar a aritmetica.
    inline u64 GpuBatchSlotCount(u32 _PoolCount) {
        return static_cast<u64>(kGpuPipelineClassCount) * _PoolCount;
    }

    // Desfecho de UM candidato.
    //
    // Recebe as tabelas por SPAN de proposito: a validacao de indice pertence a esta funcao, e nao
    // ao chamador. Se ela recebesse as linhas ja lidas, a leitura que ela existe para impedir ja
    // teria acontecido no `operator[]` de quem chama.
    //
    // A ordem e slot -> meta -> ids -> linhas -> pool -> elegibilidade, e cada passo valida antes de
    // usar o resultado do anterior.
    //
    // `_Out` so e escrito no desfecho `Indirect`, e ai sai COMPLETO: o lote e o comando inteiro.
    // Espelha o `GpuClassifyOne` do GpuClassifyShared.hlsli, que devolve o mesmo par — a razao de ser
    // o comando inteiro, e nao ids, esta escrita la.
    inline u32 GpuClassifyCandidate(u32 _Slot,
                                    std::span<const FGpuSceneInstanceMeta> _Meta,
                                    std::span<const FGpuMaterialRecord> _Materials,
                                    std::span<const FGpuMeshRecord> _Meshes,
                                    u32 _PoolCount,
                                    FGpuClassifyCommand& _Out) {
        if (_Slot == 0xFFFFFFFEu) return GpuClassifyFrustumRejected;
        if (_Slot == 0xFFFFFFFDu) return GpuClassifyOcclusionRejected;
        if (_Slot == kGpuExpandNoSlot) return GpuClassifyNoSlot;
        if (_Slot >= _Meta.size())     return GpuClassifyInvalidSlot;

        const FGpuSceneInstanceMeta& Meta = _Meta[_Slot];
        // Generation zero e linha VAZIA. O expand ja comparou a generation ESPERADA do candidato;
        // aqui so resta perguntar se a linha tem ocupante, que e o invariante que sobra depois dele.
        if (Meta.Generation == 0u) return GpuClassifyInvalidSlot;

        if (Meta.MaterialId >= _Materials.size()) return GpuClassifyInvalidMaterialId;
        if (Meta.MeshId >= _Meshes.size())        return GpuClassifyInvalidMeshId;

        const FGpuMaterialRecord& Material = _Materials[Meta.MaterialId];
        const FGpuMeshRecord&     Mesh     = _Meshes[Meta.MeshId];

        // O pool e validado ANTES da elegibilidade: um pool fora da lista e defeito de tabela, e
        // reporta-lo como "nao elegivel" o esconderia numa contagem que cresce por bons motivos.
        if (Mesh.GeometryPoolId >= _PoolCount) return GpuClassifyInvalidPoolId;

        // A PORTA e o predicado completo, e so ele decide. A decomposicao abaixo nunca decide nada:
        // ela apenas ATRIBUI o motivo do que ja foi recusado. Escrever `if (!classe) ... else if
        // (!flag) ...` deixaria a decisao espalhada em dois testes, e no dia em que um deles sumisse
        // a malha irrepresentavel receberia comando com offset poison.
        if (!GpuIndirectEligible(Material.PipelineClass, Mesh.Flags)) {
            // Com os dois termos fechados, a CLASSE tem precedencia no relato. O desfecho e o mesmo
            // (fallback CPU); o que isto garante e que o motivo seja deterministico.
            return GpuPipelineClassIndirectEligible(Material.PipelineClass)
                       ? GpuClassifyMeshUnusable
                       : GpuClassifyNotEligible;
        }

        _Out.Batch = GpuBatchIndex({ Material.PipelineClass, Mesh.GeometryPoolId }, _PoolCount);
        _Out.Args.MaterialRow           = Meta.MaterialId;
        _Out.Args.InstanceSlot          = _Slot;
        _Out.Args.IndexCountPerInstance = Mesh.IndexCount;
        _Out.Args.InstanceCount         = 1u;
        _Out.Args.StartIndexLocation    = Mesh.IndexOffset;
        _Out.Args.BaseVertexLocation    = Mesh.VertexOffset;
        _Out.Args.StartInstanceLocation = 0u;
        return GpuClassifyIndirect;
    }


    // Por que o dispatch NAO pode ser feito.
    //
    // Tudo aqui e conhecido pela CPU antes de qualquer comando ser gravado: capacidade de buffer,
    // numero de pools e tamanho das tabelas sao dela. Nenhum destes motivos exige olhar um contador
    // que a GPU escreveu — se exigisse, seria readback decidindo draw.
    enum EGpuClassifyPreflight : u32 {
        GpuClassifyPreflightOk = 0,
        // Sem pool publicado nao existe lote possivel: TODO candidato viraria `InvalidPoolId`.
        GpuClassifyPreflightNoPools = 1,
        // `BatchCounts` menor que `GpuBatchSlotCount(PoolCount)`. E este motivo que torna o
        // `Overflow` do shader inalcancavel: com a capacidade certa, o indice do lote ja esta
        // limitado pelo `PoolCount` que o proprio shader valida.
        GpuClassifyPreflightSmallBatchBuffer = 2,
        // Tabela canonica vazia com candidatos na fila. Meta vazia faz todo candidato virar
        // `InvalidSlot`; material ou malha vazios fazem todo candidato virar id invalido. Em
        // qualquer dos tres o dispatch so produziria canario.
        GpuClassifyPreflightEmptyTable = 3,
        // Mais pools do que `BatchCounts` pode enderecar: `kGpuPipelineClassCount * PoolCount`
        // passa de UINT32_MAX. Nao existe `BatchCapacity` capaz de cobrir — ela propria e u32 —, e o
        // `GpuBatchIndex` passaria a dar a volta. Motivo SEPARADO do buffer pequeno porque a acao e
        // outra: aqui nao ha o que alocar, e o que quebrou foi a cena.
        GpuClassifyPreflightTooManyPools = 4,
    };

    // O que o dispatch recebe. Espelha o ClassifyCB do shader.
    struct FGpuClassifyInput {
        std::span<const u32>                   Slots; // saida do ExpandInstanceRanges
        std::span<const FGpuSceneInstanceMeta> Meta;
        std::span<const FGpuMaterialRecord>    Materials;
        std::span<const FGpuMeshRecord>        Meshes;
        u32 PoolCount     = 0;
        u32 BatchCapacity = 0; // slots de BatchCounts; ver GpuBatchSlotCount
    };


    // A porta do DIMENSIONAMENTO: quantos slots alocar para `BatchCounts`, `BatchCursors`,
    // `DrawCounts` e a tabela de regioes.
    //
    // Existe para que a conversao para u32 nao seja escrita em lugar nenhum fora daqui. A sequencia
    // correta e u64 -> porta -> converte, e um `static_cast<u32>(GpuBatchSlotCount(...))` no chamador
    // a inverte em silencio: converte primeiro e so depois pergunta. Com a conversao dentro da porta,
    // a sequencia certa e a unica disponivel.
    //
    // `_OutSlots` so e escrito quando o retorno e `Ok` — um dimensionamento recusado nao pode deixar
    // um numero plausivel na variavel do chamador.
    inline u32 GpuClassifyBatchBufferSlots(u32 _PoolCount, u32& _OutSlots) {
        if (_PoolCount == 0u) return GpuClassifyPreflightNoPools;
        const u64 Necessario = GpuBatchSlotCount(_PoolCount);
        if (Necessario > UINT32_MAX) return GpuClassifyPreflightTooManyPools;
        _OutSlots = static_cast<u32>(Necessario);
        return GpuClassifyPreflightOk;
    }
    // A porta do FRAME. Chamada pela CPU ANTES do dispatch; reprovada, o frame inteiro desenha pelo
    // caminho classico e nenhum comando indireto e gravado.
    //
    // Pura e sobre a MESMA struct que o dispatch recebe, de proposito: uma pre-condicao verificada
    // sobre outra copia dos numeros e uma pre-condicao sobre outra coisa.
    //
    // `GpuClassifyPreflightFor` e a mesma porta sobre a CONTAGEM de candidatos, para quem nao tem a
    // lista na CPU: a cadeia do main view expande os ranges na GPU, e a CPU so sabe quantas saidas
    // pediu — o mesmo numero que ela escreve no cbuffer. A versao da struct delega para ela: e uma porta
    // so, e nao duas copias.
    inline u32 GpuClassifyPreflightFor(u64 _CandidateCount,
                                       std::span<const FGpuSceneInstanceMeta> _Meta,
                                       std::span<const FGpuMaterialRecord> _Materials,
                                       std::span<const FGpuMeshRecord> _Meshes, u32 _PoolCount,
                                       u32 _BatchCapacity) {
        // Sem candidato nao ha o que reprovar: um dispatch de zero threads e valido e nao produz
        // desfecho nenhum. Reprovar aqui obrigaria o chamador a tratar "cena vazia" como falha.
        if (_CandidateCount == 0u) return GpuClassifyPreflightOk;
        if (_Meta.empty() || _Materials.empty() || _Meshes.empty())
            return GpuClassifyPreflightEmptyTable;
        // A mesma porta do dimensionamento, e nao uma segunda copia da aritmetica: se as duas
        // divergissem, o buffer seria alocado por um criterio e aprovado por outro.
        u32 Necessario = 0;
        const u32 Motivo = GpuClassifyBatchBufferSlots(_PoolCount, Necessario);
        if (Motivo != GpuClassifyPreflightOk) return Motivo;
        // Aqui `Necessario` ja passou pela porta, entao a comparacao e u32 contra u32 sem risco.
        if (_BatchCapacity < Necessario) return GpuClassifyPreflightSmallBatchBuffer;
        return GpuClassifyPreflightOk;
    }

    inline u32 GpuClassifyPreflight(const FGpuClassifyInput& _In) {
        return GpuClassifyPreflightFor(_In.Slots.size(), _In.Meta, _In.Materials, _In.Meshes,
                                       _In.PoolCount, _In.BatchCapacity);
    }


    // === Regioes: ONDE cada lote escreve =======================================================
    //
    // O `ExecuteIndirect` so deixa a GPU decidir a CONTAGEM. O offset do argumento e o
    // `MaxCommandCount` sao parametros da CPU, e cada lote precisa de uma chamada propria, porque
    // amarra o par VB/IB do seu pool. Logo a CPU tem de saber onde cada lote comeca sem ler nada que a
    // GPU escreveu — e isso descarta um prefixo calculado na GPU: o offset que ele produzisse so
    // chegaria ao `ExecuteIndirect` por readback, que a regra 7 proibe.
    //
    // A REGIAO do lote b e `[Offset, Offset + Capacity)` em `IndirectArgs`, e so o lote b escreve
    // nela. A CPU decide o espaco maximo; a GPU decide quantos comandos realmente existem no frame, e o
    // `DrawCounts[b]` publica isso. A gravacao le a MESMA regiao:
    //
    //     ExecuteIndirect(Signature, MaxCommandCount      = Capacity,
    //                     IndirectArgs, ArgumentBufferOffset = Offset * 28,
    //                     DrawCounts,   CountBufferOffset    = b * 4)
    //
    // ⚠️ A capacidade vem de um limite ESTAVEL da cena, e nao do frame: instancias VIVAS por
    // {PipelineClass, GeometryPoolId} que o indireto pode emitir. Ela so muda quando muda a associacao
    // de uma instancia (material, malha), a classe de um material ou o pool de uma malha — nunca com
    // camera, visibilidade ou culling. Calcular sobre a lista frustum-visible do frame custaria duas
    // vezes:
    //
    //   - manteria O(candidatos) na CPU por frame, que e o custo que esta fase existe para tirar;
    //   - a Fase 6 poe o culling na GPU, e ai a CPU nao tem lista visivel nenhuma para contar. Com o
    //     limite estavel, o culling so REDUZ o `DrawCounts` dentro de regioes que nao mudam.
    //
    // Por isso a contagem recebe as TABELAS, e nao a lista de candidatos: nao ha como alimenta-la com
    // o frame.
    //
    // "Viva" e linha com ocupante (generation != 0) que pode virar candidata ao raster. Linhas
    // `RaytracingOnly` ficam de fora porque sao EXCLUSAO permanente — o proxy do terreno nunca entra na
    // lista. `Visible` NAO exclui: esconder um objeto no editor nao pode mudar o layout, e a instancia
    // escondida so reserva espaco que o count buffer nao alcanca.
    //
    // O limite pressupoe que cada slot apareca NO MAXIMO UMA VEZ na lista expandida, que e o contrato
    // do produtor de candidatos. Se ele quebrar, ou a contagem passa da capacidade e a finalizacao
    // invalida o frame, ou cabe e o objeto e desenhado duas vezes — desenhar demais, que a regra 6
    // aceita. Em nenhum dos dois casos um comando cai na regiao de outro lote.

    // === A VERSAO do layout ===================================================================
    //
    // A regiao cria um risco SILENCIOSO: se a tabela que a GPU le e os offsets que a gravacao usa vierem
    // de layouts diferentes, cada comando e escrito numa posicao que o `ExecuteIndirect` do seu lote nao
    // le — ou que o de OUTRO lote le, com o par VB/IB do pool errado. Nenhuma API reclama. Tabela
    // atrasada, frame slot trocado e upload esquecido produzem exatamente isso.
    //
    // A defesa e a ideia da ancora: um numero que a gravacao escreve do zero no `ScatterCB`, contra um
    // numero que viaja DENTRO da tabela. Cada regiao publicada carrega a versao do layout que a
    // produziu; o scatter recebe a versao do layout que vai gravar os `ExecuteIndirect`, e, se as duas
    // divergem, registra falha sem tocar em argumento nenhum. A finalizacao zera o frame inteiro.
    //
    // Por REGIAO, e nao num cabecalho da tabela: cada comando confere a regiao que ele mesmo vai usar,
    // e uma tabela parcialmente atualizada tambem e pega.
    //
    // Versao ZERO e invalida: e o que um buffer zerado, ou nunca escrito, contem. Um upload esquecido
    // sobre memoria zerada nao coincide com versao nenhuma.

    // Uma regiao. Espelhada em HLSL como `GpuIndirectRegion` (GpuBatchCounters.hlsli).
    struct FGpuIndirectRegion {
        u32 Offset   = 0; // primeiro comando do lote em `IndirectArgs`
        u32 Capacity = 0; // quantos comandos cabem; e o `MaxCommandCount` da chamada do lote
        u32 Version  = 0; // versao do layout que produziu esta regiao; 0 = invalida
    };
    static_assert(sizeof(FGpuIndirectRegion) == 12, "FGpuIndirectRegion deve casar com o HLSL (12 B)");
    static_assert(offsetof(FGpuIndirectRegion, Offset)   == 0);
    static_assert(offsetof(FGpuIndirectRegion, Capacity) == 4);
    static_assert(offsetof(FGpuIndirectRegion, Version)  == 8);

    // A FONTE das versoes: monotonica e PERSISTENTE — vive mais que a cena, e mais que o layout.
    //
    // Um contador por cena voltaria a 1 na troca de cena, e a tabela da cena anterior, ainda num frame
    // slot, coincidiria com o primeiro layout da seguinte.
    //
    // Esgota em UINT32_MAX e fica esgotada: dali em diante devolve 0, e layout com versao 0 e recusado.
    // Dar a volta para 1 reabriria exatamente a coincidencia que a versao existe para impedir. Sao
    // quatro bilhoes de mudancas de associacao, classe ou pool.
    class FGpuIndirectLayoutVersions {
    public:
        // `_Last` e a ultima versao ja entregue; a proxima e `_Last + 1`. O parametro existe para o
        // teste alcancar a exaustao sem quatro bilhoes de chamadas.
        explicit FGpuIndirectLayoutVersions(u32 _Last = 0u) : Last_(_Last) {}

        u32 Next() {
            if (Last_ == UINT32_MAX) return 0u;
            return ++Last_;
        }

    private:
        u32 Last_ = 0;
    };

    // Capacidade ESTAVEL de cada lote. Ver acima o que conta como viva.
    //
    // Usa o MESMO `GpuClassifyCandidate` do dispatch, e e isso que faz da capacidade um limite de
    // verdade: uma instancia so conta aqui se a GPU, recebendo-a como candidata, a emitiria — e no
    // mesmo lote. Um criterio paralelo ("material opaco e malha com pool valido") divergiria no
    // primeiro termo novo da porta, e a GPU passaria a contar comandos que nenhuma regiao previu.
    //
    // Devolve o motivo da porta de lotes (`GpuClassifyBatchBufferSlots`): sem pool, ou pools demais,
    // nao ha tabela de lotes e `_OutCapacities` nao e escrita. Sob `Ok` ela sai com um slot por lote e
    // zerada antes de contar.
    //
    // Em u64, porque a pergunta seguinte e a SOMA delas, e cada parcela ja nasce no tipo em que vai ser
    // somada.
    u32 GpuCountRegionCapacities(std::span<const FGpuSceneInstanceMeta> Meta,
                                 std::span<const FGpuMaterialRecord> Materials,
                                 std::span<const FGpuMeshRecord> Meshes, u32 PoolCount,
                                 std::vector<u64>& OutCapacities);

    enum EGpuRegionLayout : u32 {
        GpuRegionLayoutOk = 0,
        // A soma das capacidades passa de UINT32_MAX: nao existe `Offset` u32 que enderece a ultima
        // regiao, nem `IndirectArgs` que a contenha.
        GpuRegionLayoutTooManyCommands = 1,
        // Versao 0: o contador esgotou, ou ninguem atribuiu versao. Uma tabela assim coincidiria com
        // memoria zerada.
        GpuRegionLayoutInvalidVersion = 2,
    };

    class FGpuIndirectLayout;

    // Regioes DENSAS, na ordem dos lotes: `Offset[b]` = soma das capacidades antes de b. Toda regiao
    // sai carimbada com `Version`.
    //
    // A porta vem ANTES de cada parcela, e nao depois da soma: cada capacidade tem de caber em u32, e o
    // acumulado tem de caber junto com ela. Dar a volta aqui nao produz um numero absurdo, produz um
    // `Offset` pequeno e plausivel, dentro da regiao de outro lote — e isso vale para u64 tambem:
    // `{UINT64_MAX, 1}` somaria zero e passaria como layout vazio. Verificada antes de somar, a soma
    // nunca sai de u32, e todo offset e toda capacidade cabem por construcao.
    //
    // `OutLayout` so e escrito sob `Ok`: um layout recusado nao pode deixar regioes plausiveis — nem
    // uma versao — na mao do chamador.
    //
    // As regioes sao escritas NO LUGAR, depois das portas: com a capacidade de `OutLayout` ja reservada
    // (`FGpuIndirectLayoutState::Reserve`), a montagem nao aloca. Sem ela, o `resize` aloca, e se falhar
    // falha antes de mudar qualquer campo.
    u32 GpuBuildIndirectLayout(std::span<const u64> Capacities, u32 Version,
                               FGpuIndirectLayout& OutLayout);

    // O layout PUBLICADO: regioes, total e versao, construidos juntos e so leitura depois.
    //
    // Imutavel porque e o objeto que amarra os dois lados. A tabela que sobe para a GPU e os
    // `ExecuteIndirect` da gravacao saem DELE — as regioes de um lado, a versao esperada e os offsets do
    // outro. Um layout editavel depois de publicado deixaria a GPU com uma copia e a gravacao com outra,
    // que e o defeito que a versao existe para pegar. So `GpuBuildIndirectLayout` o preenche.
    class FGpuIndirectLayout {
    public:
        u32 Version() const { return Version_; }
        u32 TotalCommands() const { return Total_; }
        std::span<const FGpuIndirectRegion> Regions() const { return Regions_; }

    private:
        friend u32 GpuBuildIndirectLayout(std::span<const u64>, u32, FGpuIndirectLayout&);
        // O estado vivo nao PREENCHE o layout — isso continua so do `GpuBuildIndirectLayout` —, mas o
        // invalida e reserva a memoria da proxima montagem.
        friend class FGpuIndirectLayoutState;

        // Desfaz o layout SEM soltar a memoria das regioes. Atribuir um layout vazio tambem invalidaria,
        // mas levaria a reserva junto, e a montagem seguinte voltaria a alocar.
        void Invalidate() noexcept {
            Version_ = 0;
            Total_   = 0;
            Regions_.clear();
        }
        void   Reserve(size_t Regions) { Regions_.reserve(Regions); }
        size_t ReservedRegions() const noexcept { return Regions_.capacity(); }

        u32                             Version_ = 0;
        u32                             Total_   = 0;
        std::vector<FGpuIndirectRegion> Regions_;
    };

    // O `ArgumentBufferOffset` da chamada do lote. Em u64: `Offset * 28` em u32 da a volta a partir de
    // ~153 milhoes de comandos, e o `ExecuteIndirect` leria os argumentos de outro lote.
    inline u64 GpuRegionArgumentOffset(const FGpuIndirectRegion& _Region) {
        return static_cast<u64>(_Region.Offset) * sizeof(FGpuIndirectDrawArgs);
    }

    // O `CountBufferOffset` da chamada do lote: a contagem do lote b mora em `DrawCounts[b]`.
    inline u64 GpuRegionCountOffset(u32 _Batch) {
        return static_cast<u64>(_Batch) * sizeof(u32);
    }

    // Por que a tabela de regioes NAO pode ir para o dispatch. Tudo aqui e da CPU.
    enum EGpuRegionPreflight : u32 {
        GpuRegionPreflightOk = 0,
        // Menos regioes que slots de lote: o scatter e a finalizacao indexam `Regions[lote]` para todo
        // lote que `BatchCounts` enderaca.
        GpuRegionPreflightSmallRegionTable = 1,
        // Alguma regiao termina alem de `IndirectArgs`. Em u64: `Offset + Capacity` em u32 daria a
        // volta e aprovaria uma regiao que comeca no fim do buffer.
        GpuRegionPreflightSmallArgsBuffer = 2,
        // Duas regioes se sobrepoem, ou estao fora da ordem dos lotes. E a unica falha de regiao que a
        // GPU NAO enxerga: os dois lotes escreveriam as mesmas posicoes com contagens validas, e o
        // `ExecuteIndirect` de um desenharia comandos do outro com o par VB/IB errado.
        GpuRegionPreflightOverlap = 3,
        // A versao esperada e 0, ou alguma regiao traz outra versao. E a mesma falha que o scatter
        // registra na GPU; aqui ela e pega antes de qualquer comando ser gravado.
        GpuRegionPreflightVersion = 4,
    };

    // A porta das regioes, sobre a MESMA tabela, a MESMA versao esperada e a MESMA capacidade que o
    // scatter recebe.
    inline u32 GpuRegionPreflight(std::span<const FGpuIndirectRegion> _Regions, u32 _ExpectedVersion,
                                  u32 _BatchSlots, u32 _ArgsCapacity) {
        if (_Regions.size() < _BatchSlots) return GpuRegionPreflightSmallRegionTable;
        if (_ExpectedVersion == 0u) return GpuRegionPreflightVersion;
        u64 FimAnterior = 0;
        for (u32 B = 0; B < _BatchSlots; ++B) {
            const FGpuIndirectRegion& R = _Regions[B];
            // A versao antes de tudo: numa regiao de outro layout, offset e capacidade nao significam
            // nada, e reprova-la por sobreposicao esconderia a causa.
            if (R.Version != _ExpectedVersion) return GpuRegionPreflightVersion;
            if (R.Offset < FimAnterior) return GpuRegionPreflightOverlap;
            const u64 Fim = static_cast<u64>(R.Offset) + R.Capacity;
            if (Fim > _ArgsCapacity) return GpuRegionPreflightSmallArgsBuffer;
            FimAnterior = Fim;
        }
        return GpuRegionPreflightOk;
    }


    // O COUNT BUFFER do `ExecuteIndirect`, publicado pela FINALIZACAO (FinalizeDrawCounts.cs.hlsl).
    //
    // `DrawCounts[b]` = `BatchCounts[b]` quando o frame fechou inteiro, e ZERO em todas as entradas
    // quando QUALQUER falha foi registrada — contagem alem da regiao, cursor alem da contagem, regiao de
    // outro layout, regiao fora do recurso, um elo da verificacao de reset, ou um CANARIO do expand ou do
    // classify. Count buffer zerado faz o `ExecuteIndirect` executar zero comandos, e essa e a unica
    // forma de suprimir o draw sem readback.
    //
    // ⚠️ Publicado DEPOIS do scatter. O count buffer tem de ser a ultima palavra, escrita pelo ultimo
    // passo capaz de descobrir uma falha: publicado antes, ele ja estaria no ar quando o scatter
    // falhasse, e o command processor executaria a contagem sobre uma posicao que ninguem escreveu.
    //
    // ⚠️ `BatchCounts` NAO e o count buffer. Ele e escrito pelo classify, que nao sabe se o scatter vai
    // fechar, e uma contagem nao zero sob falha faria o draw executar argumentos velhos.
    //
    // Buffer SEPARADO de `BatchCounts`: as contagens sao o diagnostico que se quer olhar justamente
    // quando o frame foi invalidado. Custa um dword por slot de lote.

    // Referencia CPU do dispatch. E o oraculo do teste WARP e, depois, o braco A/B.
    //
    // NAO consulta o `GpuClassifyPreflight`, e isso e deliberado: ela espelha o SHADER, que executa o
    // que recebe. Quem decide se o dispatch acontece e o chamador, antes. Se ela reprovasse sozinha,
    // o teste perderia a unica forma de exercitar as defesas do shader sob entrada que o preflight
    // proibe — e uma defesa que nenhum teste alcanca e um comentario.
    //
    // As duas saidas sao ZERADAS aqui, e nao acumuladas: o shader tambem parte de contadores zerados,
    // e comparar acumulados de origens diferentes esconderia um desfecho que nunca incrementa.
    //
    // Devolve quantos candidatos viraram comando (== OutCounters[GpuClassifyIndirect]).
    u32 ClassifyCandidates(const FGpuClassifyInput& Input, std::vector<u32>& OutBatchCounts,
                           std::vector<u32>& OutCounters);

    // === A cadeia, e o RESET que a abre =========================================================
    //
    //     reset -> expand -> classify -> scatter -> finalizacao -> ExecuteIndirect(DrawCounts), um por lote
    //
    // Cinco buffers ACUMULAM por `InterlockedAdd` e sao lidos pela finalizacao: os contadores do expand,
    // `BatchCounts` e os contadores do classify, `BatchCursors` e os contadores do scatter. Os cinco sao
    // zerados por UM passo, o primeiro da cadeia — um dono, um invariante: "todo acumulador comeca a
    // execucao em zero". A tabela de regioes nao entra no reset: ela nao acumula, e e da CPU.
    //
    // ⚠️ Nao e arrumacao. Sem o reset, a segunda execucao comeca das contagens da primeira:
    //
    //   - `BatchCounts` INFLADO e o pior caso, porque nenhuma falha do scatter o enxerga — o cursor
    //     nunca passa de uma contagem grande demais — e o count buffer publicaria posicoes que
    //     ninguem escreveu;
    //   - cursor SUJO desloca comandos para posicoes que a contagem nao cobre;
    //   - contador do scatter velho carregaria a falha de uma execucao para a seguinte.
    //
    // A finalizacao e a rede de seguranca, e ela so funciona com uma ANCORA. Comparar os acumuladores
    // entre si nao pega o caso realista — esquecer o reset INTEIRO —, porque os quatro crescem em
    // sincronia: as contagens dobram, os cursores continuam de onde pararam e terminam iguais a elas.
    // A ancora e o numero de candidatos, escrito do zero no cbuffer a cada execucao; a partir dela a
    // finalizacao encadeia cinco igualdades, uma por acumulador (ver EGpuFinalizeReason). Com isso,
    // esquecer o reset nao desenha comando velho — desenha NADA. O reset continua obrigatorio porque um
    // frame que nao desenha nada tambem e defeito, so que visivel.
    //
    // A capacidade das regioes tambem nao acumula, e uma contagem dobrada alem dela invalida o frame.
    // Mas ela NAO substitui a ancora: com folga na regiao — instancia escondida hoje, culling na Fase
    // 6 —, a contagem dobrada cabe, e so a ancora a enxerga.

    // === Scatter ================================================================================
    //
    // Segunda passada: cada candidato INDIRECT recebe `Regions[lote].Offset + cursor` e o comando que
    // o `GpuClassifyCandidate` ja montou.
    //
    // ⚠️ Depois de `Indirect`, nenhum ramo deixa de escrever SEM AVISAR. A contagem ja reservou a
    // posicao; um candidato contado e nao escrito deixaria ali o comando do frame passado. Os quatro
    // ramos que nao escrevem registram FALHA, e a finalizacao zera o `DrawCounts` do frame inteiro.
    enum EGpuScatterCounter : u32 {
        GpuScatterCounterEmitted       = 0, // comandos escritos
        // FALHA: cursor alem da contagem do lote. Pega cursor sujo — todo inicio nao zero num lote com
        // candidatos leva ao menos o ultimo alem da contagem — e contagem divergente.
        GpuScatterCounterFaultCursor   = 1,
        // FALHA: a regiao do lote veio de OUTRO layout — versao diferente da esperada, ou zero. Nada da
        // regiao e usado: nem offset, nem capacidade, nem a posicao em `IndirectArgs`. E a falha que
        // tabela atrasada, frame slot trocado e upload esquecido produzem.
        GpuScatterCounterFaultLayoutVersion = 2,
        // FALHA: cursor alem da capacidade da REGIAO — o frame tem mais comandos no lote do que a CPU
        // reservou. Nao escrever e o que impede o comando excedente de cair na primeira posicao da
        // regiao SEGUINTE, que o `ExecuteIndirect` do outro lote desenharia com o pool errado.
        GpuScatterCounterFaultRegion   = 3,
        // FALHA: a regiao nao cabe em `IndirectArgs`. Inalcancavel sob `GpuRegionPreflight`; se
        // alcancada, a tabela que a GPU recebeu nao e a que a CPU aprovou.
        GpuScatterCounterFaultCapacity = 4,
        GpuScatterCounterCount         = 5,
    };

    // Bytes de `IndirectArgs` para `_Commands` comandos — o `TotalCommands()` do layout.
    //
    // Em u64, e nao por zelo: `_Commands * 28` em u32 da a volta a partir de ~153 milhoes de
    // comandos, e o recurso seria alocado pequeno para as regioes que o shader recebe.
    //
    // Sem modo de falha, e isso e consequencia do tipo: a unica multiplicacao fica em u64, e a
    // contagem de comandos ENTRA como u32 — nada aqui converte um produto de volta para u32. O piso de
    // um comando e porque recurso de zero bytes e ilegal, e uma cena sem regiao ainda amarra a view.
    inline u64 GpuScatterArgsBytes(u32 _Commands) {
        const u32 Piso = _Commands == 0u ? 1u : _Commands;
        return static_cast<u64>(Piso) * sizeof(FGpuIndirectDrawArgs);
    }

    // Referencia CPU do scatter.
    //
    // `InOutArgs` vem DIMENSIONADO e SEMEADO pelo chamador, e so as posicoes emitidas sao escritas —
    // exatamente como na GPU. Seu tamanho e a capacidade de `IndirectArgs` que o shader recebe. E isso
    // que permite ao teste semear os dois lados com o mesmo poison e comparar tambem o que ficou
    // intocado: um buraco dentro de uma regiao aparece como poison entre os comandos, e uma escrita
    // fora dela aparece como comando onde deveria haver poison.
    //
    // A ORDEM dentro de um lote aqui e a dos candidatos; na GPU e a de chegada ao cursor, que nao e
    // deterministica. Nada no draw depende dela (o lote inteiro compartilha PSO e pool), e por isso o
    // teste compara cada regiao como MULTICONJUNTO.
    //
    // `InOutCursors` e o estado dos cursores ANTES do scatter — normalmente o que o reset zerou. Ele e
    // parametro, e nao zerado aqui dentro, para que o teste consiga reproduzir o cursor sujo que a
    // falha de cursor existe para pegar.
    //
    // `Regions` e a tabela que a GPU LE; `ExpectedVersion` e a versao do layout que GRAVA. Sao dois
    // parametros de proposito: e a divergencia entre eles que a falha de versao existe para pegar.
    void ScatterIndirectArgs(const FGpuClassifyInput& Input,
                             std::span<const FGpuIndirectRegion> Regions, u32 ExpectedVersion,
                             std::span<const u32> BatchCounts, std::vector<u32>& InOutCursors,
                             std::vector<FGpuIndirectDrawArgs>& InOutArgs,
                             std::vector<u32>& OutCounters);

    // Por que a finalizacao invalidou o frame. BITS, espelhando SMILE_FINALIZE_* do
    // GpuBatchCounters.hlsli. Telemetria: nenhum draw depende deles.
    enum EGpuFinalizeReason : u32 {
        // A REGRA DE PUBLICACAO das regioes: contagem de algum lote alem da capacidade invalida o frame
        // inteiro, e nao so o lote.
        //
        // O estouro aparece tambem como `FaultRegion`, e os dois nao sao a mesma defesa. O do scatter
        // protege a MEMORIA — impede o comando excedente de ser escrito na regiao seguinte. Este
        // protege a PUBLICACAO — compara a contagem com a regiao que a CPU publicou, sem depender de qual
        // thread recebeu qual cursor. O teste os separa pelo motivo exato.
        GpuFinalizeRegionOverflow      = 1u << 0,
        GpuFinalizeFaultCursor         = 1u << 1, // falha de cursor registrada pelo scatter
        GpuFinalizeFaultLayoutVersion  = 1u << 2, // regiao de outro layout, registrada pelo scatter
        GpuFinalizeFaultRegion         = 1u << 3, // falha de regiao registrada pelo scatter
        GpuFinalizeFaultCapacity       = 1u << 4, // regiao fora de `IndirectArgs`, registrada pelo scatter
        // Os quatro elos da verificacao de RESET. O primeiro e a ANCORA: o numero de candidatos vem
        // do cbuffer e nao acumula. Sem ele, esquecer o reset inteiro passaria sempre que a regiao
        // tivesse folga — os acumuladores crescem juntos, e toda igualdade entre eles continua valendo.
        GpuFinalizeClassifyAnchor      = 1u << 5, // soma dos contadores do classify != candidatos
        GpuFinalizeBatchSum            = 1u << 6, // soma dos lotes != indiretos
        GpuFinalizeCursorMismatch      = 1u << 7, // cursor != contagem em algum lote
        GpuFinalizeScatterSum          = 1u << 8, // emitidos + falhas do scatter != indiretos
        // CANARIOS: um candidato que nenhum dos dois caminhos desenha. A soma dos desfechos prova que
        // cada candidato foi CONTADO, e nao separa politica de corrupcao — com um canario, os outros lotes
        // desenhariam num frame que fecha, e a perda seria parcial e silenciosa.
        GpuFinalizeClassifyCanary      = 1u << 9,  // algum desfecho `GpuClassifyOutcomeIsCanary`
        GpuFinalizeExpandCanary        = 1u << 10, // slot invalido, RaytracingOnly ou overflow no expand
        // O quinto elo do reset: os contadores do expand tambem somam os candidatos desta execucao.
        GpuFinalizeExpandAnchor        = 1u << 11,
    };

    // Referencia CPU da FINALIZACAO. Publica o count buffer a partir das contagens e de TODA
    // verificacao descrita no FinalizeDrawCounts.cs.hlsl. Devolve 1 quando o frame fechou e 0 quando
    // foi invalidado — o mesmo `Status[0]` do shader —, e `OutReason` recebe o `Status[1]`.
    //
    // `ExpandCounters` tem `GpuExpandCounterCount` entradas e `ClassifyCounters`,
    // `GpuClassifyOutcomeCount`: as mesmas leituras do shader.
    u32 FinalizeDrawCounts(std::span<const u32> BatchCounts,
                           std::span<const FGpuIndirectRegion> Regions,
                           std::span<const u32> ScatterCounters, std::span<const u32> BatchCursors,
                           std::span<const u32> ClassifyCounters, std::span<const u32> ExpandCounters,
                           u32 CandidateCount, std::vector<u32>& OutDrawCounts, u32& OutReason);
}
