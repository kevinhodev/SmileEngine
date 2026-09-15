#pragma once

// Entrada do culling: ranges sobre uma LISTA DE CANDIDATOS (Fase 4 do GPU-DRIVEN-PLAN, decisao D5).
//
// O range endereca um span da lista de indices, e NUNCA um intervalo de slots. Um
// `{FirstSlot, SlotCount}` so descreveria uma celula se a alocacao de slots fosse particionada
// pela hierarquia, e isso conflita com a free list global da Fase 1: assim que houver reuso de
// slot e objetos migrando entre celulas, os membros de uma celula deixam de ser contiguos e o
// produtor teria de emitir milhares de ranges unitarios. A indirecao desacopla os dois — o range
// descreve um span da lista, e a lista carrega slots arbitrarios.
//
// Enquanto a cena for plana, o produtor emite UM range cobrindo tudo. World cells trocam o
// produtor, nunca o consumidor. Nao existe "modo range identidade": uma flag que pulasse a lista
// faria `FirstCandidate` voltar a ser interpretado como slot, que e exatamente o contrato
// rejeitado acima. O custo que ela economizaria nao existe, porque para a cena plana a lista e
// PERSISTENTE — reconstruida em mudanca estrutural, e nao por frame.
//
// Esta fase nao executa culling: todo candidato valido passa. O que ela estabelece e quem entra na
// lista e o que se conta sobre quem nao entra.

#include "Smile/Core/Types.h"
#include "Smile/Scene/GpuSceneHandle.h"

#include <span>
#include <vector>

namespace Smile {
    class FScene;

    struct FInstanceRange {
        u32 FirstCandidate = 0; // indice em CandidateInstanceIds
        u32 CandidateCount = 0;
    };
    static_assert(sizeof(FInstanceRange) == 8);
    static_assert(offsetof(FInstanceRange, FirstCandidate) == 0);
    static_assert(offsetof(FInstanceRange, CandidateCount) == 4);

    // A comparacao que o diff do upload usa: um range so sobe quando mudou.
    inline bool operator==(const FInstanceRange& A, const FInstanceRange& B) {
        return A.FirstCandidate == B.FirstCandidate && A.CandidateCount == B.CandidateCount;
    }
    inline bool operator!=(const FInstanceRange& A, const FInstanceRange& B) { return !(A == B); }

    // Um candidato: slot MAIS a generation esperada.
    //
    // A generation viaja junto porque sem ela nao existe validacao de generation nenhuma — o
    // consumidor so consegue perguntar "esta linha tem ALGUM ocupante?", que e outra pergunta. Com
    // ela, um candidato de uma lista que ficou para tras e RECUSADO em vez de aceitar em silencio o
    // ocupante novo do slot reciclado.
    //
    // Isso hoje e coberto por construcao — reciclar slot passa por Add/Remove, que bumpam a versao
    // de estrutura e reconstroem a lista —, e e exatamente por isso que a checagem vale: ela
    // verifica o invariante em vez de presumi-lo. Custa 4 B por candidato.
    //
    // Espelhado em HLSL como `GpuCandidateId`.
    struct FGpuCandidateId {
        u32 Slot       = FGpuSceneHandle::kInvalidSlot;
        u32 Generation = 0; // 0 = invalido, mesma convencao do FGpuSceneHandle
    };
    static_assert(sizeof(FGpuCandidateId) == 8);
    static_assert(offsetof(FGpuCandidateId, Slot)       == 0);
    static_assert(offsetof(FGpuCandidateId, Generation) == 4);

    inline bool operator==(const FGpuCandidateId& A, const FGpuCandidateId& B) {
        return A.Slot == B.Slot && A.Generation == B.Generation;
    }
    inline bool operator!=(const FGpuCandidateId& A, const FGpuCandidateId& B) { return !(A == B); }

    // Por que cada renderavel ficou de FORA da lista.
    //
    // Publicados separados, e nao somados num "rejeitados": eles tem causas e consequencias
    // diferentes. `RaytracingOnly` crescendo e normal (e o proxy do terreno); `InvalidSlot`
    // crescendo e residencia que nao foi alocada, que e defeito; `Overflow` diferente de zero e
    // geometria que SOME, que a regra 6 do plano proibe.
    struct FGpuCandidateStats {
        // Quantos candidatos a cena TEM, e nao quantos couberam. E o numero que dimensiona o
        // buffer: se fosse "quantos couberam", a capacidade necessaria nunca seria observavel.
        u32 Candidates      = 0;
        u32 InvalidSlot     = 0; // sem residencia GPU, ou generation que nao confere
        u32 RaytracingOnly  = 0; // classe EXCLUIDA do raster; ver abaixo
        u32 NoGeometry      = 0; // sem malha valida: nao ha o que desenhar
        u32 Hidden          = 0; // FRenderable::Visible == false (estado autoral, nao culling)
        u32 Overflow        = 0; // quantos passaram do teto — o unico que tem de ser ZERO
    };

    // Lista persistente de candidatos ao raster, mais os ranges que a enderecam.
    //
    // ⚠️ `FRenderable::RaytracingOnly` e EXCLUSAO, e nao fallback. Ele marca renderaveis que
    // existem so para a TLAS e as tabelas de hit — hoje o proxy do terreno —, e o laco CPU de
    // hoje ja os filtra explicitamente. Sem herdar esse filtro, a expansao de ranges desenharia o
    // terreno DUAS vezes: uma pelo FTerrain e outra pelo proxy. O D9 lista terrain/water como
    // fallback; esta e outra classe.
    class FGpuSceneCandidates {
    public:
        // Reconstroi a lista se a MEMBRESIA pode ter mudado. Transform nao muda quem e candidato,
        // entao a versao de transforms nao entra: arrastar um objeto nao reconstroi lista nenhuma.
        //
        // Devolve true quando reconstruiu.
        bool RefreshIfStale(const FScene& Scene);

        // Reconstroi incondicionalmente. Existe para teste e para quem precisa forcar; o caminho
        // de frame usa o RefreshIfStale.
        void Rebuild(const FScene& Scene);

        void Reset();

        // Teto da lista. Zero significa "sem teto" — e o default, porque a lista e CPU e cresce
        // com a cena. O teto existe para quando ela espelhar um buffer GPU de capacidade fixa.
        //
        // Mudar o teto INVALIDA a lista construida: ela foi montada sob o teto antigo, e nem as
        // versoes da cena nem qualquer outra coisa que o RefreshIfStale observa mudaram. Sem esta
        // invalidacao, aumentar o teto depois de um overflow deixaria a lista truncada para
        // sempre.
        void SetCapacity(u32 Capacity);
        u32  Capacity() const { return Capacity_; }
        // O maior conjunto de candidatos ja visto — inclusive o que nao coube. E com ele que se
        // dimensiona o buffer, e por isso ele acompanha `Stats().Candidates` e nao o tamanho da
        // lista.
        u32  HighWaterMark() const { return HighWater_; }

        // ⚠️ FALSO quando houve overflow, e nesse caso a lista vem VAZIA.
        //
        // Contar o descarte nao o torna conservador: uma lista truncada e utilizavel e geometria
        // sumindo em silencio, que a regra 6 do plano proibe. Ou a lista descreve a cena inteira,
        // ou ela nao existe — e o consumidor cai no caminho CPU (decisao D9). O numero para
        // redimensionar sai de `Stats().Candidates`, que conta todos.
        bool IsUsable() const { return Built_ && Stats_.Overflow == 0; }

        std::span<const FGpuCandidateId> InstanceIds() const { return CandidateIds_; }
        std::span<const FInstanceRange>  Ranges()      const { return Ranges_; }
        const FGpuCandidateStats&        Stats()       const { return Stats_; }

        u64 BuiltStructureVersion()    const { return BuiltStructureVersion_; }
        u64 BuiltStaticCastersVersion() const { return BuiltStaticCastersVersion_; }

    private:
        std::vector<FGpuCandidateId> CandidateIds_;
        std::vector<FInstanceRange>  Ranges_;
        FGpuCandidateStats           Stats_;
        u32                          Capacity_  = 0;
        u32                          HighWater_ = 0;
        bool                         Built_     = false;
        u64                          BuiltStructureVersion_     = ~0ull;
        u64                          BuiltStaticCastersVersion_ = ~0ull;
    };

    // Contadores do dispatch de expansao. ESPELHO de Shaders/GpuScene/ExpandInstanceRanges.cs.hlsl:
    // os indices sao os mesmos SMILE_EXPAND_COUNTER_* de la, e quem os verifica e o teste WARP, que
    // le os quatro de volta. Renumerar um lado sem o outro nao quebra compilacao nenhuma.
    enum EGpuExpandCounter : u32 {
        GpuExpandCounterExpanded    = 0, // slots escritos na saida
        GpuExpandCounterInvalidSlot = 1, // slot fora da tabela, linha sem ocupante, offset incoerente
        GpuExpandCounterRtOnly      = 2, // linha RaytracingOnly alcancada: o produtor CPU falhou
        GpuExpandCounterOverflow    = 3, // nao coube na capacidade da saida
        GpuExpandCounterCount       = 4,
    };

    // Posicao de saida sem candidato. Mesmo valor do SMILE_EXPAND_NO_SLOT do shader; nao e zero,
    // porque zero e um slot legitimo.
    constexpr u32 kGpuExpandNoSlot = 0xFFFFFFFFu;

    // Prefixo EXCLUSIVO das contagens dos ranges: onde a saida de cada range comeca. Devolve o
    // total de saidas.
    //
    // Calculado na CPU e enviado junto: os ranges sao poucos enquanto a cena for plana (um), e um
    // dispatch de prefixo para somar um elemento custaria mais que a copia. Quando o produtor for
    // por celula, este e o primeiro candidato a virar GPU.
    u32 BuildRangeOutputBases(std::span<const FInstanceRange> Ranges, std::vector<u32>& OutBases);

    // Expande ranges em lista de slots. E a referencia CPU do que o dispatch da GPU faz, e o
    // instrumento do gate "range unico e multiplos ranges sinteticos produzem a mesma lista".
    //
    // Range fora dos limites da lista e IGNORADO, e nao truncado: truncar produziria uma lista
    // plausivel e mais curta, que e o modo de falha que a regra 6 do plano proibe — desenhar
    // demais e aceitavel, sumir geometria nao. Ignorar deixa o range aparecer inteiro ou nao
    // aparecer, e o contador de invalidos denuncia.
    u32 ExpandCandidateRanges(std::span<const FInstanceRange> Ranges,
                              std::span<const FGpuCandidateId> CandidateIds,
                              std::vector<u32>& OutSlots);
}
