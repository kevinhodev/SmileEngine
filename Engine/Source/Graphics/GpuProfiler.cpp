#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

namespace Smile {
    void FGpuProfiler::Initialize(ID3D12Device* _Device, ID3D12CommandQueue* _Queue,
                                  u32 _FramesInFlight) {
        FramesInFlight_ = _FramesInFlight;
        QueriesPerFrame = kMaxScopes * 2;

        if (FAILED(_Queue->GetTimestampFrequency(&TimestampFrequency)) ||
            TimestampFrequency == 0) {
            LogWarning("[GPU] Timestamp frequency indisponivel; profiler de GPU desativado");
            return;
        }

        D3D12_QUERY_HEAP_DESC HeapDesc{};
        HeapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        HeapDesc.Count = QueriesPerFrame * FramesInFlight_;
        SMILE_HR(_Device->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(&QueryHeap)));

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(QueriesPerFrame) * FramesInFlight_ * sizeof(u64);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Readback)));

        // Map persistente: heap READBACK aceita; a leitura por slot so acontece depois do
        // wait de fence do frame (feito pelo CommandQueue.BeginFrame).
        void* Ptr = nullptr;
        D3D12_RANGE All{ 0, static_cast<SIZE_T>(Desc.Width) };
        SMILE_HR(Readback->Map(0, &All, &Ptr));
        ReadbackMapped = reinterpret_cast<const u64*>(Ptr);

        PendingScopes.resize(FramesInFlight_);
        CurrentScopes.reserve(kMaxScopes);
        OpenStack.reserve(8);
        LogInfo("[GPU] Profiler de timestamps inicializado (" +
                std::to_string(kMaxScopes) + " escopos/frame)");
    }

    void FGpuProfiler::BeginFrame(u32 _FrameSlot) {
        if (!IsInitialized()) return;
        FrameSlot_ = _FrameSlot;

        // Le o que este slot gravou ha kFramesInFlight frames — o fence do slot ja foi
        // esperado pelo BeginFrame da fila, entao os ticks estao completos no readback.
        const auto& Old = PendingScopes[FrameSlot_];
        if (!Old.empty()) {
            const u64* Base = ReadbackMapped +
                static_cast<size_t>(FrameSlot_) * QueriesPerFrame;
            const f64 TicksToMs = 1000.0 / static_cast<f64>(TimestampFrequency);

            LastResults.clear();
            LastResults.reserve(Old.size());
            for (const FScopeRec& S : Old) {
                const u64 T0 = Base[S.BeginQuery];
                const u64 T1 = Base[S.EndQuery];
                const f64 Ms = (T1 > T0) ? static_cast<f64>(T1 - T0) * TicksToMs : 0.0;

                auto [It, Inserted] = Smoothed.try_emplace(S.Name, Ms);
                if (!Inserted) It->second += (Ms - It->second) * 0.1; // EMA
                LastResults.push_back({ S.Name, It->second });
            }
        }

        CurrentScopes.clear();
        OpenStack.clear();
        NextQuery = 0;
    }

    void FGpuProfiler::Begin(ID3D12GraphicsCommandList* _List, const char* _Name) {
        if (!IsInitialized() || CurrentScopes.size() >= kMaxScopes ||
            NextQuery + 2 > QueriesPerFrame)
            return;

        FScopeRec Rec;
        Rec.Name       = _Name;
        Rec.BeginQuery = NextQuery++;
        Rec.EndQuery   = NextQuery++; // reservado ja; o End so grava
        _List->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                        FrameSlot_ * QueriesPerFrame + Rec.BeginQuery);
        OpenStack.push_back(static_cast<u32>(CurrentScopes.size()));
        CurrentScopes.push_back(Rec);
    }

    void FGpuProfiler::End(ID3D12GraphicsCommandList* _List) {
        if (!IsInitialized() || OpenStack.empty()) return;
        const FScopeRec& Rec = CurrentScopes[OpenStack.back()];
        OpenStack.pop_back();
        _List->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                        FrameSlot_ * QueriesPerFrame + Rec.EndQuery);
    }

    void FGpuProfiler::Resolve(ID3D12GraphicsCommandList* _List) {
        if (!IsInitialized()) return;
        if (NextQuery > 0) {
            _List->ResolveQueryData(
                QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                FrameSlot_ * QueriesPerFrame, NextQuery, Readback.Get(),
                static_cast<UINT64>(FrameSlot_) * QueriesPerFrame * sizeof(u64));
        }
        PendingScopes[FrameSlot_] = CurrentScopes;
    }
}
