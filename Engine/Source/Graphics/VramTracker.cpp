#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/Logger.h"
#include <mutex>
#include <unordered_map>

namespace Smile::VramTracker {
    namespace {
        struct FEntry {
            EVramCategory Category;
            u64           Bytes;
        };

        // Singleton "leaky" de proposito: o destruction callback do D3D pode disparar
        // durante o teardown estatico do processo; um estado nunca destruido nao corre
        // atras da ordem de destruicao entre TUs.
        struct FState {
            std::mutex Mutex;
            std::unordered_map<ID3D12Resource*, FEntry> Entries;
            std::array<u64, static_cast<size_t>(EVramCategory::Count)> Totals{};
            bool WarnedNoNotifier = false;
        };
        FState& State() {
            static FState* Instance = new FState();
            return *Instance;
        }

        void CALLBACK OnResourceDestroyed(void* _Data) {
            auto* Key = static_cast<ID3D12Resource*>(_Data);
            FState& S = State();
            std::lock_guard Lock(S.Mutex);
            const auto It = S.Entries.find(Key);
            if (It == S.Entries.end()) return;
            S.Totals[static_cast<size_t>(It->second.Category)] -= It->second.Bytes;
            S.Entries.erase(It);
        }
    }

    void Register(ID3D12Resource* _Resource, EVramCategory _Category) {
        if (!_Resource) return;

        // So interessa o que mora em VRAM; staging UPLOAD/READBACK fica em system memory.
        // (GetHeapProperties falha p/ reserved resources — nao usamos; ignora nesse caso.)
        D3D12_HEAP_PROPERTIES HeapProps{};
        D3D12_HEAP_FLAGS      HeapFlags{};
        if (FAILED(_Resource->GetHeapProperties(&HeapProps, &HeapFlags)) ||
            HeapProps.Type == D3D12_HEAP_TYPE_UPLOAD ||
            HeapProps.Type == D3D12_HEAP_TYPE_READBACK)
            return;

        ComPtr<ID3D12Device> Device;
        if (FAILED(_Resource->GetDevice(IID_PPV_ARGS(&Device)))) return;

        // Tamanho real alocado (alinhamento incluso), igual ao que o driver cobra.
        const D3D12_RESOURCE_DESC Desc = _Resource->GetDesc();
        const u64 Bytes = Device->GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes;

        FState& S = State();

        // Auto-desregistro quando o recurso morre. Sem o notifier (Win10 < 1803) os
        // totais so crescem — avisa uma vez e segue.
        ComPtr<ID3DDestructionNotifier> Notifier;
        if (SUCCEEDED(_Resource->QueryInterface(IID_PPV_ARGS(&Notifier)))) {
            UINT Cookie = 0;
            Notifier->RegisterDestructionCallback(&OnResourceDestroyed, _Resource, &Cookie);
        } else {
            std::lock_guard Lock(S.Mutex);
            if (!S.WarnedNoNotifier) {
                S.WarnedNoNotifier = true;
                LogWarning("[VRAM] ID3DDestructionNotifier indisponivel; totais por "
                           "categoria nao decrementam ao liberar recursos");
            }
        }

        std::lock_guard Lock(S.Mutex);
        const auto It = S.Entries.find(_Resource);
        if (It != S.Entries.end()) {
            // Re-registro do mesmo recurso vivo (nao esperado): troca em vez de somar.
            S.Totals[static_cast<size_t>(It->second.Category)] -= It->second.Bytes;
            It->second = FEntry{ _Category, Bytes };
        } else {
            S.Entries.emplace(_Resource, FEntry{ _Category, Bytes });
        }
        S.Totals[static_cast<size_t>(_Category)] += Bytes;
    }

    FVramSnapshot Snapshot() {
        FState& S = State();
        FVramSnapshot Snap;
        std::lock_guard Lock(S.Mutex);
        Snap.Bytes = S.Totals;
        for (const u64 B : Snap.Bytes) Snap.TotalTracked += B;
        return Snap;
    }

    const char* CategoryName(EVramCategory _Category) {
        switch (_Category) {
            case EVramCategory::Geometry:      return "Geometria";
            case EVramCategory::SceneTextures: return "Texturas da cena";
            case EVramCategory::RenderTargets: return "Render targets";
            case EVramCategory::Shadows:       return "Sombras";
            case EVramCategory::RaytracingAS:  return "Raytracing (AS)";
            case EVramCategory::GI:            return "GI e reflexos";
            case EVramCategory::Sky:           return "Céu e nuvens";
            case EVramCategory::Water:         return "Água";
            case EVramCategory::Misc:          return "Outros";
            default:                           return "?";
        }
    }
}
