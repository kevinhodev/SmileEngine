#pragma once

// Historico de um valor por renderavel, endereçado pelo SLOT DE RESIDENCIA e nao pela posicao na
// lista.
//
// Por que existe: guardar "o que este objeto era no frame passado" num vetor indexado por posicao
// significa que remover alguem desloca a cauda e cada sobrevivente passa a ler o valor do
// ex-vizinho. Para transform, isso e motion vector de outro objeto -- aparece como smear. A saida
// que o codigo usava era descartar TUDO a cada mudanca estrutural: funciona, e joga fora a
// convergencia de quem nao se mexeu.
//
// O handle e guardado junto do valor porque slot sozinho nao basta: um slot reciclado apontaria
// para o valor do ocupante anterior, e a generation e o unico campo que distingue os dois.
//
// CPU puro e sem d3d12 de proposito, para ser testavel sem device. Ha duas instancias em uso, com
// ciclos de vida independentes: o PrevMVP do raster e os transforms do motion vector de RT.

#include "Smile/Core/Types.h"
#include "Smile/Scene/GpuSceneHandle.h"

#include <vector>

namespace Smile {
    template <typename T>
    class TSlotKeyedHistory {
    public:
        // Descarta o historico inteiro. Usado onde a comparacao por handle nao ajuda -- corte de
        // camera, troca de cena, reset deterministico --, e nao mais em mudanca estrutural, que e
        // exatamente o que o enderecamento por slot passou a cobrir.
        void Invalidate() { Entries.clear(); }

        void EnsureCapacity(u32 SlotCount) {
            if (Entries.size() < SlotCount) Entries.resize(SlotCount, FEntry{});
        }

        // Valor do frame anterior DAQUELE objeto, ou `Fallback` quando nao ha.
        //
        // Nao ha historico em tres casos, e os tres tem o mesmo desfecho certo: objeto novo, slot
        // fora da capacidade guardada, e slot reciclado (handle diferente). Devolver o valor do
        // ocupante anterior no ultimo caso seria inventar um passado que nunca existiu.
        const T& PreviousOr(FGpuSceneHandle Handle, const T& Fallback) const {
            // Generation 0 e reservada para "nunca alocado". Conferir so o slot deixava um handle
            // zerado com slot valido -- {0,0} -- casar com uma entrada guardada por outro {0,0},
            // e o contrato diz que ele nao identifica ninguem.
            if (Handle.Generation == 0 || Handle.Slot >= Entries.size()) return Fallback;
            const FEntry& E = Entries[Handle.Slot];
            if (E.Handle != Handle) return Fallback;
            return E.Value;
        }

        void Store(FGpuSceneHandle Handle, const T& Value) {
            // Mesma razao do PreviousOr, e aqui o estrago e maior: guardar por {0,0} SOBRESCREVIA
            // a entrada valida daquele slot, apagando o historico de quem esta vivo nele.
            if (Handle.Generation == 0 || Handle.Slot >= Entries.size()) return;
            Entries[Handle.Slot] = FEntry{ Value, Handle };
        }

        // Invisibilidade nao aposenta a residencia, mas encerra a continuidade do historico.
        void Forget(FGpuSceneHandle Handle) {
            if (Handle.Generation != 0 && Handle.Slot < Entries.size() &&
                Entries[Handle.Slot].Handle == Handle) Entries[Handle.Slot] = FEntry{};
        }

        u32  Capacity() const { return static_cast<u32>(Entries.size()); }
        bool Empty()    const { return Entries.empty(); }

    private:
        struct FEntry {
            T               Value{};
            FGpuSceneHandle Handle;
        };
        std::vector<FEntry> Entries;
    };
}
