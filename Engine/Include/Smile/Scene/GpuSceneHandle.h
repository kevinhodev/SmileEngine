#pragma once

// Identidade da RESIDENCIA GPU de um renderavel, separada da ordem autoral (posicao na
// RenderableList) e da identidade de editor (FRenderable::Id).
//
// Por que existe: o indice na lista e endereco de GPU em quatro sistemas ao mesmo tempo —
// InstanceID da TLAS, slot de transform do TemporalMotion, entrada do readback do HiZ e o ID que o
// passe de picking grava. Enquanto a cena so crescia, o indice servia de identidade; com o editor
// criando e removendo objeto, nao serve mais. Hoje os quatro estao PROTEGIDOS por descarte global
// (o dominio SceneStructure derruba os historicos, e o OnSceneStructureChanged limpa o
// PreviousModels e cancela picks), e nao por identidade — o que funciona e cobra o preco de jogar
// fora tudo a cada edicao. Este handle e o que permite parar de jogar fora.
//
// Um erro aqui nao aparece como excecao: aparece como o objeto errado selecionado, motion vector de
// outro objeto ou uma instancia fantasma na TLAS, muitos frames depois.
//
// CPU PURO: nao inclui d3d12.h e nao conhece fila nenhuma. O renderer entrega numeros (ver
// FGpuFenceSet), e e o que permite testar geracao, handle obsoleto, wrap e reuso sem device.

#include "Smile/Core/Types.h"

#include <vector>

namespace Smile {
    struct FGpuSceneHandle {
        // Escopada na struct, e nao no namespace: `kInvalidSlot` ja existe duas vezes na engine
        // (FTextureSRVHeap, FBvhDebugView) significando SLOT DE DESCRIPTOR HEAP, que e outro
        // conceito. Uma terceira em escopo de Smile seria o mesmo nome para duas coisas.
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        // COM inicializador. Sem ele, `FGpuSceneHandle{}` produz {0,0} — que identificaria o
        // primeiro ocupante do slot 0 — e `FGpuSceneHandle H;` deixa os dois indeterminados.
        u32 Slot       = kInvalidSlot;
        // 0 e RESERVADA para "nunca alocado / invalido"; o primeiro ocupante de qualquer slot usa
        // 1. Assim IsAlive({slot, 0}) e falso para todo slot, em qualquer estado, sem caso
        // especial — e e a unica guarda que sobra quando alguem zera a struct com memset ou
        // calloc, que contornam o inicializador de Slot.
        u32 Generation = 0;

        friend bool operator==(const FGpuSceneHandle& A, const FGpuSceneHandle& B) {
            return A.Slot == B.Slot && A.Generation == B.Generation;
        }
        friend bool operator!=(const FGpuSceneHandle& A, const FGpuSceneHandle& B) {
            return !(A == B);
        }
    };

    // Fences exigidas para reusar um slot, uma por FILA.
    //
    // Duas, e nao uma: FCommandQueue e FAsyncComputeQueue sao independentes, cada uma com o seu
    // FenceValue, e nao existe fence agregada que prove a conclusao das duas. Uma fila ter passado
    // e a outra nao mantem o slot Retired.
    struct FGpuFenceSet {
        u64 Direct       = 0;
        u64 AsyncCompute = 0;

        bool Covers(const FGpuFenceSet& Required) const {
            return Direct >= Required.Direct && AsyncCompute >= Required.AsyncCompute;
        }
    };

    enum class EGpuSlotState : u8 {
        Free,      // disponivel para alocacao
        Live,      // ocupado por um renderavel existente
        Retired,   // o objeto morreu; indisponivel ATE AS FENCES PASSAREM
        Exhausted, // generation esgotada; indisponivel PARA SEMPRE
    };

    // Allocator de slots com deferred reuse.
    //
    // O ciclo e Free -> Live -> Retired -> Free, com a generation incrementando na alocacao. O
    // quarto estado existe porque Retired significa "temporario ate as fences": sem ele, aposentar
    // no wrap carregaria dois significados no mesmo estado, o Collect ficaria ambiguo e o mesmo
    // slot esgotado poderia ser contado varias vezes.
    class FGpuSceneSlots {
    public:
        // Aloca um slot para um objeto novo. A generation do slot incrementa, entao um handle
        // anterior daquele slot nunca volta a casar.
        FGpuSceneHandle Allocate();

        // O objeto morreu. O slot vai para Retired IMEDIATAMENTE — a mutacao nao espera a GPU —,
        // guardando ate onde cada fila precisa chegar antes de ele poder ser reusado.
        //
        // Required tem de ser capturado numa FRONTEIRA DE SUBMISSAO, com os ultimos valores
        // realmente submetidos nas duas filas. Capturado durante a gravacao de uma list, ele nao
        // cobriria essa list, e o slot voltaria a Free cedo demais.
        void Retire(FGpuSceneHandle Handle, const FGpuFenceSet& Required);

        // Devolve a Free todo slot Retired cujas DUAS fences ja passaram. Slot cuja generation
        // esgotou vai para Exhausted em vez de Free, e o contador sobe uma vez so.
        // Retorna quantos voltaram a Free.
        u32 Collect(const FGpuFenceSet& Completed);

        // Vivo = estado Live E generation igual. As duas coisas: a generation protege contra o
        // ocupante SEGUINTE, o estado protege contra a janela entre a morte e o reuso — nela o
        // slot esta Retired e a generation AINDA NAO MUDOU, entao comparar so a generation
        // aceitaria o resultado atrasado de um objeto ja removido.
        bool IsAlive(FGpuSceneHandle Handle) const;

        // Aposenta todo slot Live. Usado na troca de cena: nao ha atalho devolvendo slots direto a
        // Free, porque isso seria reuso sem incremento de generation.
        //
        // A sequencia do reload e DRENADA e a ordem importa:
        //     RetireAll(required) -> Collect(completed >= required) -> alocar a nova cena
        // Alocar antes do Collect nao quebra identidade, mas faz o pool DOBRAR: os slots antigos
        // ainda estao em Retired, a cena nova nao acha nada em Free e cria outro tanto.
        void RetireAll(const FGpuFenceSet& Required);

        u32 Capacity()       const { return static_cast<u32>(Slots.size()); }
        u32 LiveCount()      const { return LiveCount_; }
        u32 RetiredCount()   const { return RetiredCount_; }
        // Slots perdidos por overflow de generation. Exposto de proposito: a politica e segura mas
        // reduz a capacidade, e reducao silenciosa vira mistério meses depois.
        u32 ExhaustedCount() const { return ExhaustedCount_; }

    private:
        friend struct FGpuSceneSlotsTestAccess;

        struct FSlot {
            u32           Generation = 0;
            EGpuSlotState State      = EGpuSlotState::Free;
            FGpuFenceSet  Required;
        };

        std::vector<FSlot> Slots;
        std::vector<u32>   FreeList;
        u32                LiveCount_      = 0;
        u32                RetiredCount_   = 0;
        u32                ExhaustedCount_ = 0;
    };
}
