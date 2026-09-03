#include "Smile/Scene/GpuSceneHandle.h"

#include <limits>

namespace Smile {
    FGpuSceneHandle FGpuSceneSlots::Allocate() {
        u32 Slot;
        if (!FreeList.empty()) {
            Slot = FreeList.back();
            FreeList.pop_back();
        } else {
            // Recusa ANTES de crescer. Em Slots.size() == kInvalidSlot o cast produziria
            // exatamente o slot invalido, e a funcao devolveria um handle que contradiz o proprio
            // contrato; no valor seguinte o cast volta a zero e Capacity() e os lacos em u32
            // passam a truncar. O desfecho e explicito: handle invalido, e um contador para o caso
            // nao ser silencioso.
            if (Slots.size() >= MaxSlots) {
                ++AllocationFailures_;
                return FGpuSceneHandle{};
            }
            Slot = static_cast<u32>(Slots.size());
            Slots.push_back(FSlot{});
        }

        FSlot& Entry = Slots[Slot];
        // Incrementa ANTES de publicar: um slot novo esta em 0 (reservada) e o primeiro ocupante
        // sai com 1, e um slot reusado sai com generation estritamente maior que a do anterior.
        ++Entry.Generation;
        Entry.State    = EGpuSlotState::Live;
        Entry.Required = FGpuFenceSet{};
        ++LiveCount_;

        return FGpuSceneHandle{ Slot, Entry.Generation };
    }

    void FGpuSceneSlots::Retire(FGpuSceneHandle _Handle, const FGpuFenceSet& _Required) {
        // Aposentar um handle que ja nao e o vivo daquele slot seria aposentar o OCUPANTE ATUAL
        // por conta de um dono anterior — exatamente a troca de identidade que este tipo existe
        // para impedir. IsAlive cobre os dois casos: generation velha e estado que nao e Live.
        if (!IsAlive(_Handle)) return;
        RetireSlot(_Handle.Slot, _Required);
    }

    void FGpuSceneSlots::RetireSlot(u32 _Slot, const FGpuFenceSet& _Required) {
        // A operacao que pode lancar vem ANTES das mudancas de estado. Se o push_back falhasse
        // depois delas, o slot ficaria Retired sem constar na lista e nunca mais seria coletado —
        // vazamento silencioso de capacidade.
        RetiredList.push_back(_Slot);

        FSlot& Entry = Slots[_Slot];
        Entry.State    = EGpuSlotState::Retired;
        Entry.Required = _Required;
        --LiveCount_;
        ++RetiredCount_;
    }

    u32 FGpuSceneSlots::Collect(const FGpuFenceSet& _Completed) {
        // Percorre SO os aposentados, e nao a capacidade inteira. Varrer todos os slots recolocaria
        // uma passada O(N) de CPU por frame — na cena de 1M, exatamente o custo que esta fase
        // existe para remover. Um early-out em RetiredCount_ == 0 nao resolveria: bastaria um
        // ticket pendente para a varredura integral voltar.
        //
        // Compactacao no lugar: quem nao pode sair fica, quem saiu some da lista. Cada slot aparece
        // no maximo uma vez, porque Retire so age sobre slot Live e a saida de Retired acontece
        // aqui, junto com a remocao da entrada.
        u32    Liberados = 0;
        size_t Escrita   = 0;
        LastCollectVisits_ = 0;
        for (size_t Leitura = 0; Leitura < RetiredList.size(); ++Leitura) {
            const u32 Index = RetiredList[Leitura];
            FSlot&    Entry = Slots[Index];
            ++LastCollectVisits_;

            if (!_Completed.Covers(Entry.Required)) {
                RetiredList[Escrita++] = Index;
                continue;
            }

            --RetiredCount_;
            // A transicao para Exhausted acontece AQUI, depois das fences, e nao no momento do
            // incremento: o slot ainda precisava esperar a GPU soltar o ocupante anterior. E como
            // ele sai da lista nesta mesma passagem, o contador sobe exatamente uma vez por slot,
            // por mais vezes que o Collect seja chamado.
            if (Entry.Generation == std::numeric_limits<u32>::max()) {
                Entry.State = EGpuSlotState::Exhausted;
                ++ExhaustedCount_;
                continue;
            }

            Entry.State    = EGpuSlotState::Free;
            Entry.Required = FGpuFenceSet{};
            FreeList.push_back(Index);
            ++Liberados;
        }
        RetiredList.resize(Escrita);
        return Liberados;
    }

    bool FGpuSceneSlots::IsAlive(FGpuSceneHandle _Handle) const {
        if (_Handle.Slot >= Slots.size()) return false;
        const FSlot& Entry = Slots[_Handle.Slot];
        // Generation 0 nunca e alocada, entao um handle zerado por memset ({0,0}) morre aqui pela
        // comparacao, sem precisar de caso especial.
        return Entry.State == EGpuSlotState::Live && Entry.Generation == _Handle.Generation;
    }

    void FGpuSceneSlots::RetireAll(const FGpuFenceSet& _Required) {
        // Reserva antes de mexer em estado, pelo mesmo motivo do RetireSlot: uma realocacao que
        // falhasse no meio deixaria parte dos slots Retired e fora da lista.
        RetiredList.reserve(RetiredList.size() + LiveCount_);
        for (u32 Index = 0; Index < static_cast<u32>(Slots.size()); ++Index) {
            if (Slots[Index].State != EGpuSlotState::Live) continue;
            RetireSlot(Index, _Required);
        }
    }
}
