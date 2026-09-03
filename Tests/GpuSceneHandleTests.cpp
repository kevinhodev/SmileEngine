// Ciclo de vida do FGpuSceneHandle: invalido, generation inicial, wrap, reset e reuso por fence.
//
// Cada caso aqui corresponde a uma politica escrita no GPU-DRIVEN-PLAN (Fase 1) ANTES da
// implementacao, e existe para ela nao virar decisao escondida no codigo. Um erro nestas regras
// nao aparece como excecao: aparece como o objeto errado selecionado, motion vector de outro
// objeto ou instancia fantasma na TLAS, muitos frames depois.
//
// CPU puro, sem device: e o que o allocator receber numeros do renderer (FGpuFenceSet) compra.

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#include "Smile/Scene/GpuSceneHandle.h"

namespace Smile {
    // Semeia a generation de um slot. Testar o wrap gastando as generations de verdade seriam 2^32
    // alocacoes POR SLOT — horas de execucao para provar uma transicao de borda. O que interessa e
    // o comportamento EM UINT32_MAX, e ele se alcanca semeando.
    struct FGpuSceneSlotsTestAccess {
        static void SeedGeneration(FGpuSceneSlots& Slots, u32 Slot, u32 Generation) {
            Slots.Slots[Slot].Generation = Generation;
        }
        static EGpuSlotState StateOf(const FGpuSceneSlots& Slots, u32 Slot) {
            return Slots.Slots[Slot].State;
        }
        // Teto de slots injetavel: a borda de crescimento fica em 0xFFFFFFFF, e alcanca-la de
        // verdade exigiria alocar bilhoes de entradas.
        static void SetMaxSlots(FGpuSceneSlots& Slots, u32 Max) { Slots.MaxSlots = Max; }
        // Tamanho da lista de aposentados: e o que prova que o Collect percorre O(aposentados) e
        // nao O(capacidade).
        static size_t RetiredListSize(const FGpuSceneSlots& Slots) {
            return Slots.RetiredList.size();
        }
        static u32 LastCollectVisits(const FGpuSceneSlots& Slots) {
            return Slots.LastCollectVisits_;
        }
    };
}

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    using namespace Smile;

    const FGpuFenceSet kZero{ 0, 0 };
    const FGpuFenceSet kFar { 1000, 1000 };
}

int main() {
    // 1. FGpuSceneHandle{} nunca fica vivo, em nenhum estado do pool.
    {
        FGpuSceneSlots Pool;
        Check(!Pool.IsAlive(FGpuSceneHandle{}), "handle default nao vive num pool vazio");

        const FGpuSceneHandle A = Pool.Allocate();
        Check(!Pool.IsAlive(FGpuSceneHandle{}), "handle default nao vive com o slot 0 ocupado");

        // E o caso do memset, que contorna o inicializador de Slot: {0,0}. Aqui quem protege e
        // SO a generation reservada, nao o kInvalidSlot.
        FGpuSceneHandle Zerado{};
        Zerado.Slot       = 0;
        Zerado.Generation = 0;
        Check(!Pool.IsAlive(Zerado), "handle zerado {0,0} nao vive nem com o slot 0 ocupado");

        Pool.Retire(A, kZero);
        Check(!Pool.IsAlive(FGpuSceneHandle{}), "handle default nao vive com o slot Retired");
    }

    // 2. O primeiro handle do slot 0 nao coincide com o invalido.
    {
        FGpuSceneSlots Pool;
        const FGpuSceneHandle Primeiro = Pool.Allocate();
        Check(Primeiro.Slot == 0, "o primeiro handle ocupa o slot 0");
        Check(Primeiro.Generation == 1, "o primeiro ocupante usa generation 1, nao 0");
        Check(Primeiro != FGpuSceneHandle{}, "o primeiro handle difere do invalido");
        Check(Pool.IsAlive(Primeiro), "o primeiro handle esta vivo");
    }

    // 3 e 4. Reuso incrementa a generation, e o handle anterior continua invalido.
    {
        FGpuSceneSlots Pool;
        const FGpuSceneHandle Antigo = Pool.Allocate();
        Pool.Retire(Antigo, FGpuFenceSet{ 10, 20 });

        // Enquanto Retired, o antigo JA nao vale: e a janela entre a morte e o reuso, em que a
        // generation ainda nao mudou. So o estado protege aqui.
        Check(!Pool.IsAlive(Antigo), "handle de slot Retired nao vive (generation ainda igual)");

        // Fence parcial nao libera (teste 7 tambem cobre isso explicitamente).
        Check(Pool.Collect(FGpuFenceSet{ 10, 19 }) == 0, "collect parcial nao libera");
        Check(Pool.Collect(FGpuFenceSet{ 10, 20 }) == 1, "collect com as duas fences libera");

        const FGpuSceneHandle Novo = Pool.Allocate();
        Check(Novo.Slot == Antigo.Slot, "o reuso pega o mesmo slot");
        Check(Novo.Generation == Antigo.Generation + 1, "o reuso incrementa a generation");
        Check(Pool.IsAlive(Novo), "o novo handle vive");
        Check(!Pool.IsAlive(Antigo), "o handle anterior continua invalido depois do reuso");
    }

    // 5 e 8. Wrap: o slot vai para Exhausted DEPOIS das fences, e o contador sobe uma vez so.
    {
        FGpuSceneSlots Pool;
        const FGpuSceneHandle Vitima = Pool.Allocate();
        const u32 Capacidade = Pool.Capacity();

        // Handle "muito antigo" que casaria se a generation desse a volta.
        const FGpuSceneHandle Ancestral{ Vitima.Slot, 1 };

        FGpuSceneSlotsTestAccess::SeedGeneration(Pool, Vitima.Slot,
                                                 std::numeric_limits<u32>::max());
        const FGpuSceneHandle NoLimite{ Vitima.Slot, std::numeric_limits<u32>::max() };
        Check(Pool.IsAlive(NoLimite), "o slot semeado continua vivo com a generation no limite");

        Pool.Retire(NoLimite, FGpuFenceSet{ 5, 5 });
        Check(FGpuSceneSlotsTestAccess::StateOf(Pool, Vitima.Slot) == EGpuSlotState::Retired,
              "no wrap o slot ainda passa por Retired: a GPU nao soltou o ocupante");
        Check(Pool.ExhaustedCount() == 0, "o contador so sobe depois das fences");

        Check(Pool.Collect(FGpuFenceSet{ 5, 5 }) == 0, "slot esgotado nao volta a Free");
        Check(FGpuSceneSlotsTestAccess::StateOf(Pool, Vitima.Slot) == EGpuSlotState::Exhausted,
              "depois das fences o slot vai para Exhausted");
        Check(Pool.ExhaustedCount() == 1, "o contador de esgotados sobe uma vez");

        // Collect repetido nao pode contar de novo.
        Pool.Collect(FGpuFenceSet{ 9, 9 });
        Pool.Collect(FGpuFenceSet{ 99, 99 });
        Check(Pool.ExhaustedCount() == 1, "collects repetidos nao recontam o mesmo slot esgotado");

        Check(!Pool.IsAlive(Ancestral), "wrap nao ressuscita um handle antigo");
        Check(!Pool.IsAlive(NoLimite), "nem o do proprio limite");

        // O slot esgotado sai do pool util: a proxima alocacao cria outro.
        const FGpuSceneHandle Depois = Pool.Allocate();
        Check(Depois.Slot != Vitima.Slot, "o slot esgotado nunca mais e alocado");
        Check(Pool.Capacity() == Capacidade + 1, "a capacidade cresce porque o esgotado saiu");
    }

    // 6. Troca de cena: sequencia DRENADA, mesmo slot com generation maior e capacidade
    //    inalterada. So "o handle antigo continua invalido" deixaria passar o pool dobrando.
    {
        FGpuSceneSlots Pool;
        FGpuSceneHandle CenaAntiga[8];
        for (FGpuSceneHandle& H : CenaAntiga) H = Pool.Allocate();
        const u32 CapacidadeAntes = Pool.Capacity();
        Check(CapacidadeAntes == 8, "oito slots para a cena antiga");

        const FGpuFenceSet Required{ 42, 7 };
        Pool.RetireAll(Required);
        Check(Pool.LiveCount() == 0, "RetireAll esvazia os vivos");
        Check(Pool.RetiredCount() == 8, "e os oito ficam Retired");

        Check(Pool.Collect(Required) == 8, "o Collect drenado devolve os oito");

        FGpuSceneHandle CenaNova[8];
        for (FGpuSceneHandle& H : CenaNova) H = Pool.Allocate();

        Check(Pool.Capacity() == CapacidadeAntes,
              "a capacidade fica inalterada: alocar depois do Collect reusa, nao dobra");
        for (const FGpuSceneHandle& H : CenaAntiga)
            Check(!Pool.IsAlive(H), "nenhum handle da cena anterior ressuscita");

        // Cada slot reaparece com generation MAIOR.
        bool TodasMaiores = true;
        for (const FGpuSceneHandle& Novo : CenaNova) {
            for (const FGpuSceneHandle& Velho : CenaAntiga) {
                if (Novo.Slot == Velho.Slot && Novo.Generation <= Velho.Generation)
                    TodasMaiores = false;
            }
        }
        Check(TodasMaiores, "todo slot reusado voltou com generation maior");
    }

    // 6b. A ordem ERRADA (alocar antes do Collect) dobra o pool. Documenta o custo que a
    //     sequencia drenada evita, e falha se algum dia o allocator passar a reusar Retired.
    {
        FGpuSceneSlots Pool;
        for (int i = 0; i < 8; ++i) Pool.Allocate();
        Pool.RetireAll(FGpuFenceSet{ 1, 1 });
        for (int i = 0; i < 8; ++i) Pool.Allocate();
        Check(Pool.Capacity() == 16,
              "alocar antes do Collect dobra o pool (e por isso a sequencia e drenada)");
    }

    // 7. Collect parcial: uma fila passou, a outra nao.
    {
        FGpuSceneSlots Pool;
        const FGpuSceneHandle H = Pool.Allocate();
        Pool.Retire(H, FGpuFenceSet{ 100, 200 });

        Check(Pool.Collect(FGpuFenceSet{ 100, 199 }) == 0, "direct passou, compute nao: segura");
        Check(Pool.Collect(FGpuFenceSet{  99, 200 }) == 0, "compute passou, direct nao: segura");
        Check(Pool.Collect(FGpuFenceSet{  99, 199 }) == 0, "nenhuma passou: segura");
        Check(Pool.RetiredCount() == 1, "o slot continua Retired em todos os parciais");
        Check(Pool.Collect(FGpuFenceSet{ 100, 200 }) == 1, "as duas passaram: libera");
        Check(Pool.RetiredCount() == 0, "e sai de Retired");
    }

    // Retire com handle obsoleto nao pode derrubar o ocupante atual.
    {
        FGpuSceneSlots Pool;
        const FGpuSceneHandle Antigo = Pool.Allocate();
        Pool.Retire(Antigo, kZero);
        Pool.Collect(kFar);
        const FGpuSceneHandle Atual = Pool.Allocate();
        Check(Atual.Slot == Antigo.Slot, "mesmo slot");

        Pool.Retire(Antigo, kZero); // dono anterior tentando aposentar
        Check(Pool.IsAlive(Atual), "handle obsoleto nao aposenta o ocupante atual");
        Check(Pool.LiveCount() == 1, "e a contagem de vivos nao se corrompe");
    }

    // 9. Allocate recusa em vez de devolver o slot invalido.
    {
        FGpuSceneSlots Pool;
        FGpuSceneSlotsTestAccess::SetMaxSlots(Pool, 2);
        const FGpuSceneHandle A = Pool.Allocate();
        const FGpuSceneHandle B = Pool.Allocate();
        Check(Pool.IsAlive(A) && Pool.IsAlive(B), "os dois primeiros alocam");

        const FGpuSceneHandle C = Pool.Allocate();
        Check(C.Slot == FGpuSceneHandle::kInvalidSlot, "no teto devolve handle invalido");
        Check(!Pool.IsAlive(C), "e ele nao vive");
        Check(Pool.Capacity() == 2, "o pool nao cresceu alem do teto");
        Check(Pool.AllocationFailures() == 1, "a recusa e contada, nao silenciosa");

        // E o teto nao impede reuso: liberar um slot volta a permitir alocar.
        Pool.Retire(A, FGpuFenceSet{ 1, 1 });
        Pool.Collect(FGpuFenceSet{ 1, 1 });
        const FGpuSceneHandle D = Pool.Allocate();
        Check(Pool.IsAlive(D), "depois do Collect, alocar volta a funcionar no teto");
        Check(Pool.Capacity() == 2, "reusando, nao crescendo");
    }

    // 10. Collect custa O(aposentados), e nao O(capacidade).
    {
        FGpuSceneSlots Pool;
        FGpuSceneHandle Muitos[64];
        for (FGpuSceneHandle& H : Muitos) H = Pool.Allocate();
        Check(Pool.Capacity() == 64, "64 slots vivos");
        Check(FGpuSceneSlotsTestAccess::RetiredListSize(Pool) == 0,
              "sem aposentados, a lista esta vazia: o Collect nao tem o que percorrer");

        Pool.Retire(Muitos[7], FGpuFenceSet{ 5, 5 });
        Check(FGpuSceneSlotsTestAccess::RetiredListSize(Pool) == 1, "um aposentado na lista");

        // O TAMANHO da lista nao prova nada sozinho: uma implementacao que varre a capacidade
        // inteira tambem a mantem correta. O que prova e a contagem de VISITAS.
        Check(Pool.Collect(FGpuFenceSet{ 4, 4 }) == 0, "fence insuficiente nao libera");
        Check(FGpuSceneSlotsTestAccess::LastCollectVisits(Pool) == 1,
              "com um ticket pendente entre 64 slots, o Collect visita UM");
        Check(FGpuSceneSlotsTestAccess::RetiredListSize(Pool) == 1,
              "e o slot continua na lista para a proxima tentativa");

        Check(Pool.Collect(FGpuFenceSet{ 5, 5 }) == 1, "fence suficiente libera");
        Check(FGpuSceneSlotsTestAccess::LastCollectVisits(Pool) == 1, "e visitou UM de novo");
        Check(FGpuSceneSlotsTestAccess::RetiredListSize(Pool) == 0,
              "e a lista esvazia: nada sobra para percorrer");

        // Sem aposentado nenhum, o Collect nao toca slot algum — nem os 64 vivos.
        Check(Pool.Collect(FGpuFenceSet{ 99, 99 }) == 0, "collect sem aposentados nao libera nada");
        Check(FGpuSceneSlotsTestAccess::LastCollectVisits(Pool) == 0,
              "e visita ZERO slots: nada de varredura por capacidade");

        // Ciclos repetidos nao podem fazer a lista crescer sem limite.
        for (int Ciclo = 0; Ciclo < 10; ++Ciclo) {
            const FGpuSceneHandle H = Pool.Allocate();
            Pool.Retire(H, FGpuFenceSet{ 10, 10 });
            Pool.Collect(FGpuFenceSet{ 10, 10 });
        }
        Check(FGpuSceneSlotsTestAccess::RetiredListSize(Pool) == 0,
              "dez ciclos de retire/collect nao deixam residuo na lista");
        Check(Pool.RetiredCount() == 0, "nem no contador");
    }

    if (Failures > 0) {
        std::cerr << "[GPU-SCENE-HANDLE] " << Failures << " falha(s)\n";
        return 1;
    }
    std::cout << "[GPU-SCENE-HANDLE] ok - invalido, generation, wrap, reload e fence por fila\n";
    return 0;
}
