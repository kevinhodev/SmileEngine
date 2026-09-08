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

    // Por que cada renderavel ficou de FORA da lista.
    //
    // Publicados separados, e nao somados num "rejeitados": eles tem causas e consequencias
    // diferentes. `RaytracingOnly` crescendo e normal (e o proxy do terreno); `InvalidSlot`
    // crescendo e residencia que nao foi alocada, que e defeito; `Overflow` diferente de zero e
    // geometria que SOME, que a regra 6 do plano proibe.
    struct FGpuCandidateStats {
        u32 Candidates      = 0; // entraram na lista
        u32 InvalidSlot     = 0; // sem residencia GPU, ou generation que nao confere
        u32 RaytracingOnly  = 0; // classe EXCLUIDA do raster; ver abaixo
        u32 NoGeometry      = 0; // sem malha valida: nao ha o que desenhar
        u32 Hidden          = 0; // FRenderable::Visible == false (estado autoral, nao culling)
        u32 Overflow        = 0; // nao coube na capacidade — o unico que tem de ser ZERO
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
        // com a cena. O teto existe para o dia em que ela espelhar um buffer GPU de capacidade
        // fixa, e e o que faz `Overflow` poder ser diferente de zero.
        void SetCapacity(u32 Capacity) { Capacity_ = Capacity; }
        u32  Capacity() const { return Capacity_; }
        u32  HighWaterMark() const { return HighWater_; }

        std::span<const u32>            InstanceIds() const { return CandidateIds_; }
        std::span<const FInstanceRange> Ranges()      const { return Ranges_; }
        const FGpuCandidateStats&       Stats()       const { return Stats_; }

        u64 BuiltStructureVersion()    const { return BuiltStructureVersion_; }
        u64 BuiltStaticCastersVersion() const { return BuiltStaticCastersVersion_; }

    private:
        std::vector<u32>            CandidateIds_;
        std::vector<FInstanceRange> Ranges_;
        FGpuCandidateStats          Stats_;
        u32                         Capacity_  = 0;
        u32                         HighWater_ = 0;
        u64                         BuiltStructureVersion_     = ~0ull;
        u64                         BuiltStaticCastersVersion_ = ~0ull;
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
                              std::span<const u32> CandidateIds, std::vector<u32>& OutSlots);
}
