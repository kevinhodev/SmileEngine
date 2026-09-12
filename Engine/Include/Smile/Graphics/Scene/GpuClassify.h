#pragma once

// Classificacao dos candidatos do main view (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Entrada: a lista de slots que o `ExpandInstanceRanges` produziu. Saida: quantos comandos cada
// LOTE vai ter, mais a contabilidade de todo candidato que nao virou comando.
//
// Esta fatia so CONTA. O prefixo, a compactacao e a escrita dos argumentos vem depois; nada aqui
// tem consumidor ainda.
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
//   POLITICA  — `NotEligible` e `Unrepresentable`. Recusas previstas, e a CPU desenha esses
//               renderaveis pelo caminho classico. Crescem normalmente (masked, two-sided e Blend
//               existem em toda cena). Os dois lados avaliam o MESMO predicado sobre a MESMA linha,
//               entao a recusa da GPU e o desenho da CPU sao o mesmo fato visto de dois lugares.
//
//   CANARIO   — `NoSlot`, `InvalidSlot`, `InvalidMaterialId`, `InvalidMeshId`, `InvalidPoolId` e
//               `Overflow`. Significam invariante quebrado: as tabelas discordam do que a CPU acha
//               que existe. Sao DIAGNOSTICO, nunca entrada de roteamento — quando eles aparecem, o
//               estrago daquele frame ja aconteceu, e "voltar para a CPU no frame seguinte" seria
//               justamente a decisao por readback que a regra 7 proibe.
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
// A soma dos nove contadores e igual ao numero de candidatos. Isso prova UM RESULTADO DE
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
// tratar QUALQUER canario como possivel geometria perdida.
//
// A prova de "desenhado exatamente uma vez" so pode existir quando o fallback e o scatter
// existirem, e exige que os dois bracos leiam a MESMA versao das linhas canonicas. Ela pertence
// aquela slice; aqui ela seria encenacao.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"

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
        GpuClassifyUnrepresentable   = 7, // offset da malha fora do contrato do argumento — POLITICA
        GpuClassifyOverflow          = 8, // lote fora da capacidade de BatchCounts — CANARIO
        GpuClassifyOutcomeCount      = 9,
    };

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
    // `_OutKey` so e escrita no desfecho `Indirect`.
    inline u32 GpuClassifyCandidate(u32 _Slot,
                                    std::span<const FGpuSceneInstanceMeta> _Meta,
                                    std::span<const FGpuMaterialRecord> _Materials,
                                    std::span<const FGpuMeshRecord> _Meshes,
                                    u32 _PoolCount,
                                    FGpuBatchKey& _OutKey) {
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
                       ? GpuClassifyUnrepresentable
                       : GpuClassifyNotEligible;
        }

        _OutKey.PipelineClass  = Material.PipelineClass;
        _OutKey.GeometryPoolId = Mesh.GeometryPoolId;
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


    // A porta do DIMENSIONAMENTO: quantos slots alocar para `BatchCounts` e `BatchOffsets`.
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
    inline u32 GpuClassifyPreflight(const FGpuClassifyInput& _In) {
        // Sem candidato nao ha o que reprovar: um dispatch de zero threads e valido e nao produz
        // desfecho nenhum. Reprovar aqui obrigaria o chamador a tratar "cena vazia" como falha.
        if (_In.Slots.empty()) return GpuClassifyPreflightOk;
        if (_In.Meta.empty() || _In.Materials.empty() || _In.Meshes.empty())
            return GpuClassifyPreflightEmptyTable;
        // A mesma porta do dimensionamento, e nao uma segunda copia da aritmetica: se as duas
        // divergissem, o buffer seria alocado por um criterio e aprovado por outro.
        u32 Necessario = 0;
        const u32 Motivo = GpuClassifyBatchBufferSlots(_In.PoolCount, Necessario);
        if (Motivo != GpuClassifyPreflightOk) return Motivo;
        // Aqui `Necessario` ja passou pela porta, entao a comparacao e u32 contra u32 sem risco.
        if (_In.BatchCapacity < Necessario) return GpuClassifyPreflightSmallBatchBuffer;
        return GpuClassifyPreflightOk;
    }


    // === Prefixo dos lotes ======================================================================
    //
    // `BatchOffsets[b]` = onde os comandos do lote b comecam em `IndirectArgs`. Prefixo EXCLUSIVO
    // sobre `BatchCounts`, que e o que o classify acabou de produzir.
    //
    // Na GPU, e nao na CPU, porque `BatchCounts` e escrito pela GPU: soma-lo na CPU exigiria
    // readback, e readback nao entra no caminho do frame (regra 7). Nao e a mesma situacao do
    // `BuildRangeOutputBases` da Fase 4, que prefixa dados que a CPU produziu.
    //
    // ⚠️ `BatchCounts` serve tambem de COUNT BUFFER do `ExecuteIndirect`: ele le um uint32 em
    // `CountBufferOffset`, entao o lote b usa offset `b * 4`. Por isso o prefixo escreve num buffer
    // SEPARADO em vez de acumular no lugar — um prefixo in-place destruiria as contagens de que os
    // draws dependem.

    // Offset POISON, gravado em toda entrada que o prefixo nao conseguiu calcular.
    //
    // Nao e zero: zero e o offset LEGITIMO do primeiro lote, e um consumidor que ignorasse a flag
    // passaria a escrever os comandos de um lote por cima dos do lote 0. Com 0xFFFFFFFF ele erra
    // alto — qualquer verificacao de limite contra a capacidade de `IndirectArgs` o rejeita.
    //
    // Mesma decisao, e pelo mesmo motivo, do `kGpuMeshPoisonIndexOffset`.
    inline constexpr u32 kGpuBatchOffsetPoison = 0xFFFFFFFFu;
    enum EGpuPrefixCounter : u32 {
        // Total de comandos. Nao e `BatchOffsets[Slots]`: manter o total num contador, e nao numa
        // entrada extra do array, evita que o array de offsets precise de `Slots + 1` entradas —
        // e `Slots` ja e o numero que a porta de dimensionamento devolveu.
        GpuPrefixCounterTotal = 0,
        // A soma passou de u32. NAO e alcancavel a partir de um `BatchCounts` produzido pelo
        // classify — cada candidato contribui no maximo 1, e os candidatos sao contados em u32 —,
        // mas o shader nao tem como saber quem escreveu o buffer que ele esta somando.
        GpuPrefixCounterOverflow = 1,
        GpuPrefixCounterCount    = 2,
    };


    // O COUNT BUFFER do `ExecuteIndirect`, publicado pelo prefixo junto dos offsets.
    //
    // `DrawCounts[b]` = `BatchCounts[b]` quando o prefixo fechou, e ZERO em todas as entradas quando
    // `PrefixOverflow != 0`. Um count buffer zerado faz o `ExecuteIndirect` executar zero comandos —
    // que e a unica forma de suprimir o draw sem readback, porque a decisao acontece na GPU, dentro
    // do buffer que o command processor le.
    //
    // Buffer SEPARADO em vez de zerar `BatchCounts` no lugar: as contagens sao o diagnostico que se
    // quer olhar justamente quando houve estouro, e destrui-las apagaria a evidencia. Custa
    // `4 * PoolCount` dwords — dezenas de bytes.
    //
    // ⚠️ A versao anterior deste header dizia que `BatchCounts` seria o count buffer. Nao pode ser:
    // ele e escrito pelo classify, que nao sabe se o prefixo vai fechar.

    // Referencia CPU do prefixo. Devolve o total; `_OutOffsets` fica com o prefixo exclusivo.
    //
    // `_OutOverflow` recebe 1 quando a soma daria a volta. NESSE CASO, o contrato e:
    //
    //   - a saida inteira e INUTILIZAVEL, e suprimi-la NAO e trabalho do scatter sozinho. Um
    //     `return` no shader nao alcanca o `ExecuteIndirect`: ele e do command processor, le o COUNT
    //     BUFFER direto e executaria a contagem de antes sobre argumentos que o scatter deixou
    //     intactos — comandos do frame passado, ou memoria nunca inicializada. Por isso o prefixo
    //     publica um count buffer SANITIZADO (ver abaixo), e e ele, e nao `BatchCounts`, que vai
    //     para o `ExecuteIndirect`;
    //   - da entrada que falhou em diante, todas recebem `kGpuBatchOffsetPoison`. Deterministico e
    //     IGUAL nos dois lados, entao o teste compara elemento a elemento inclusive no caso de
    //     estouro. A entrada que falhou tambem entra: o `Acc` ali ainda e valido, mas o lote nao
    //     cabe inteiro, e publicar um inicio para um lote que nao cabe e publicar um comeco sem fim;
    //   - as entradas ANTERIORES continuam corretas, e ficam assim de proposito — e por elas que se
    //     enxerga onde a conta quebrou.
    //
    // ⚠️ A versao anterior deste comentario dizia que as entradas seguintes ficavam "com o acumulado
    // antes do estouro". Nao ficavam: a CPU deixava os zeros do `assign` e a GPU deixava o conteudo
    // anterior do recurso — duas coisas diferentes, e nenhuma das duas a descrita.
    u32 PrefixBatchCounts(std::span<const u32> Counts, std::vector<u32>& OutOffsets,
                          std::vector<u32>& OutDrawCounts, u32& OutOverflow);
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
}
