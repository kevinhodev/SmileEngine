#pragma once

#include <d3d12.h>
#include <cassert>
#include "Smile/Core/Types.h"

namespace Smile {
    // Acumulador de resource barriers: empilha transicoes e emite todas num UNICO
    // ResourceBarrier no Flush — o driver resolve os waits/flushes de cache de uma
    // vez em vez de serializar por chamada. Contrato: Flush ANTES do primeiro
    // draw/dispatch/copy que depende das transicoes empilhadas (nao flusha sozinho).
    class FBarrierBatch {
    public:
        static constexpr u32 kCapacity = 16;

        // Transicao crua: o chamador conhece o estado atual. No-op se Before == After.
        void Transition(ID3D12Resource* Res, D3D12_RESOURCE_STATES Before,
                        D3D12_RESOURCE_STATES After,
                        u32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            if (!Res || Before == After) return;
            D3D12_RESOURCE_BARRIER& B = Push();
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            B.Transition.pResource   = Res;
            B.Transition.Subresource = Subresource;
            B.Transition.StateBefore = Before;
            B.Transition.StateAfter  = After;
        }

        // Transicao com estado RASTREADO: le o atual de Cur e escreve To de volta —
        // o tracker do chamador segue sendo a unica fonte de verdade do recurso.
        void TransitionTracked(ID3D12Resource* Res, D3D12_RESOURCE_STATES& Cur,
                               D3D12_RESOURCE_STATES To,
                               u32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            Transition(Res, Cur, To, Subresource);
            Cur = To;
        }

        void UAV(ID3D12Resource* Res) {
            D3D12_RESOURCE_BARRIER& B = Push();
            B.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            B.Flags         = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            B.UAV.pResource = Res;
        }

        bool Empty() const { return Count == 0; }

        void Flush(ID3D12GraphicsCommandList* Cmd) {
            if (Count > 0) {
                Cmd->ResourceBarrier(Count, Barriers);
                Count = 0;
            }
        }

    private:
        D3D12_RESOURCE_BARRIER& Push() {
            assert(Count < kCapacity && "FBarrierBatch cheio: Flush antes ou aumente kCapacity");
            return Barriers[Count++];
        }

        D3D12_RESOURCE_BARRIER Barriers[kCapacity]{};
        u32                    Count = 0;
    };
}
