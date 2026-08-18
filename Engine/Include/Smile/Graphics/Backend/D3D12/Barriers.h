#pragma once

#include <d3d12.h>
#include <cassert>
#include "Smile/Core/Types.h"

namespace Smile {
    // Transicao UNICA e imediata. Para 2 ou mais no mesmo ponto, use FBarrierBatch: o driver
    // resolve um lote de uma vez em vez de serializar por chamada.
    //
    // Existia como copia local identica em 5 arquivos (FlickerHeatmap, TemporalAA, FsrPass,
    // DlssPass, DlssRRPass) antes de virar uma funcao so.
    inline void TransitionResource(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Res,
                                   D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After,
                                   u32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        if (!Cmd || !Res || Before == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        B.Transition.pResource   = Res;
        B.Transition.Subresource = Subresource;
        B.Transition.StateBefore = Before;
        B.Transition.StateAfter  = After;
        Cmd->ResourceBarrier(1, &B);
    }

    // Acumulador de resource barriers: empilha transicoes e emite todas num UNICO
    // ResourceBarrier no Flush — o driver resolve os waits/flushes de cache de uma
    // vez em vez de serializar por chamada. Contrato: Flush ANTES do primeiro
    // draw/dispatch/copy que depende das transicoes empilhadas (nao flusha sozinho).
    //
    // Construir com uma command list liga o AUTO-FLUSH (modelo do GPUContextDX12 do Flax):
    // ao encher, o lote e emitido sozinho em vez de estourar um assert. Emitir barreira
    // ANTES do previsto e sempre correto — o que nao pode e emitir depois do consumidor —
    // entao o auto-flush troca uma falha dura por uma perda de lote. Sem command list, o
    // comportamento historico (assert) fica de pe.
    class FBarrierBatch {
    public:
        static constexpr u32 kCapacity = 16;

        FBarrierBatch() = default;
        explicit FBarrierBatch(ID3D12GraphicsCommandList* AutoFlushTarget)
            : AutoFlush(AutoFlushTarget) {}

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
            if (Count == kCapacity && AutoFlush) Flush(AutoFlush);
            assert(Count < kCapacity && "FBarrierBatch cheio: Flush antes ou aumente kCapacity");
            return Barriers[Count++];
        }

        D3D12_RESOURCE_BARRIER     Barriers[kCapacity]{};
        u32                        Count     = 0;
        ID3D12GraphicsCommandList* AutoFlush = nullptr;
    };
}
