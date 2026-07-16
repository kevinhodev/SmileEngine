#pragma once

#include <d3d12.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "Smile/Core/Types.h"

namespace Smile {
    // Profiler de GPU por passe via timestamp queries: cada escopo grava EndQuery(TIMESTAMP)
    // no inicio e no fim, o frame resolve tudo pra um readback ring e o resultado e lido
    // kFramesInFlight frames depois (quando o fence do slot ja garantiu que a GPU terminou —
    // mesmo wait que o CommandQueue.BeginFrame ja faz, entao a leitura nunca bloqueia).
    // Escopos podem aninhar (pilha); ms suavizado por media movel exponencial.
    class FGpuProfiler {
    public:
        static constexpr u32 kMaxScopes = 64;

        void Initialize(ID3D12Device* Device, ID3D12CommandQueue* Queue, u32 FramesInFlight);
        bool IsInitialized() const { return QueryHeap != nullptr; }

        // Ordem por frame: BeginFrame (le o slot antigo) -> Begin/End dos passes -> Resolve
        // (ultima coisa antes do Close). Nomes DEVEM ser literais/estaveis (guarda o ponteiro).
        void BeginFrame(u32 FrameSlot);
        void Begin(ID3D12GraphicsCommandList* List, const char* Name);
        void End(ID3D12GraphicsCommandList* List); // fecha o escopo aberto mais recente
        void Resolve(ID3D12GraphicsCommandList* List);

        struct FScopeResult {
            const char* Name = nullptr;
            f64 Milliseconds = 0.0; // suavizado (EMA 0.1)
        };
        // Snapshot do ultimo frame lido, na ordem de gravacao ("Frame" total incluso).
        const std::vector<FScopeResult>& Results() const { return LastResults; }

    private:
        struct FScopeRec {
            const char* Name = nullptr;
            u32 BeginQuery = 0;
            u32 EndQuery   = 0;
        };

        ComPtr<ID3D12QueryHeap> QueryHeap;
        ComPtr<ID3D12Resource>  Readback;
        const u64* ReadbackMapped = nullptr; // mapeado persistente (heap READBACK)

        u64 TimestampFrequency = 0;
        u32 FramesInFlight_    = 2;
        u32 QueriesPerFrame    = kMaxScopes * 2;
        u32 FrameSlot_         = 0;
        u32 NextQuery          = 0; // proxima query LOCAL do slot corrente

        std::vector<FScopeRec>              CurrentScopes;
        std::vector<u32>                    OpenStack;
        std::vector<std::vector<FScopeRec>> PendingScopes; // por slot: o que foi gravado la
        std::unordered_map<std::string, f64> Smoothed;
        std::vector<FScopeResult>           LastResults;
    };

    // Escopo RAII: FGpuScope S(Profiler, List, "Nome");
    class FGpuScope {
    public:
        FGpuScope(FGpuProfiler& _Profiler, ID3D12GraphicsCommandList* _List, const char* _Name)
            : Profiler(_Profiler), List(_List) {
            Profiler.Begin(List, _Name);
        }
        ~FGpuScope() { Profiler.End(List); }
        FGpuScope(const FGpuScope&) = delete;
        FGpuScope& operator=(const FGpuScope&) = delete;

    private:
        FGpuProfiler&              Profiler;
        ID3D12GraphicsCommandList* List;
    };
}
