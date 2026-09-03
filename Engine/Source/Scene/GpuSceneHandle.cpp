#include "Smile/Scene/GpuSceneHandle.h"

#include <limits>

namespace Smile {
    FGpuSceneHandle FGpuSceneSlots::Allocate() {
        u32 Slot;
        if (!FreeList.empty()) {
            Slot = FreeList.back();
            FreeList.pop_back();
        } else {
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

        FSlot& Entry = Slots[_Handle.Slot];
        Entry.State    = EGpuSlotState::Retired;
        Entry.Required = _Required;
        --LiveCount_;
        ++RetiredCount_;
    }

    u32 FGpuSceneSlots::Collect(const FGpuFenceSet& _Completed) {
        u32 Liberados = 0;
        for (u32 Index = 0; Index < static_cast<u32>(Slots.size()); ++Index) {
            FSlot& Entry = Slots[Index];
            if (Entry.State != EGpuSlotState::Retired) continue;
            if (!_Completed.Covers(Entry.Required)) continue;

            --RetiredCount_;
            // A transicao para Exhausted acontece AQUI, depois das fences, e nao no momento do
            // incremento: o slot ainda precisava esperar a GPU soltar o ocupante anterior. E como
            // ele sai de Retired nesta mesma passagem, o contador sobe exatamente uma vez por
            // slot, por mais vezes que o Collect seja chamado.
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
        for (u32 Index = 0; Index < static_cast<u32>(Slots.size()); ++Index) {
            FSlot& Entry = Slots[Index];
            if (Entry.State != EGpuSlotState::Live) continue;
            Entry.State    = EGpuSlotState::Retired;
            Entry.Required = _Required;
            --LiveCount_;
            ++RetiredCount_;
        }
    }
}
