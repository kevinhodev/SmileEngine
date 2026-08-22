#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Smile::VramTracker {
    namespace {
        struct FEntry {
            EVramCategory Category;
            u64           Bytes;
            const char*   Label; // literal do call site; nullptr = nao rotulado
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

    void Register(ID3D12Resource* _Resource, EVramCategory _Category, const char* _Label) {
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
            It->second = FEntry{ _Category, Bytes, _Label };
        } else {
            S.Entries.emplace(_Resource, FEntry{ _Category, Bytes, _Label });
        }
        S.Totals[static_cast<size_t>(_Category)] += Bytes;
    }

    FVramSnapshot Snapshot() {
        FState& S = State();
        FVramSnapshot Snap;
        std::lock_guard Lock(S.Mutex);
        Snap.Bytes = S.Totals;
        for (const u64 B : Snap.Bytes) Snap.TotalTracked += B;
        Snap.LiveResources = static_cast<u32>(S.Entries.size());
        return Snap;
    }

    std::vector<FVramLabelEntry> LabelBreakdown() {
        FState& S = State();
        std::vector<FVramLabelEntry> Out;
        {
            std::lock_guard Lock(S.Mutex);
            for (const auto& [Res, E] : S.Entries) {
                if (!E.Label) continue; // sem rotulo: fica no resto da categoria
                // Agrupa pelo TEXTO e nao pelo ponteiro: o mesmo literal em duas TUs pode nao ser
                // o mesmo endereco, e ai o ping-pong sairia como duas linhas iguais.
                const auto Hit = std::find_if(Out.begin(), Out.end(), [&](const FVramLabelEntry& P) {
                    return P.Category == E.Category && std::strcmp(P.Label, E.Label) == 0;
                });
                if (Hit != Out.end()) Hit->Bytes += E.Bytes;
                else                  Out.push_back(FVramLabelEntry{ E.Category, E.Label, E.Bytes });
            }
        }
        std::sort(Out.begin(), Out.end(),
                  [](const FVramLabelEntry& A, const FVramLabelEntry& B) { return A.Bytes > B.Bytes; });
        return Out;
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
            case EVramCategory::Terrain:       return "Terreno";
            case EVramCategory::Misc:          return "Outros";
            default:                           return "?";
        }
    }

    void LogBreakdown(u64 _LocalUsageBytes) {
        const FVramSnapshot Snap = Snapshot();

        // Uma casa decimal em MB, do jeito que a janela mostra — comparar duas builds vira
        // diferenca de texto, nao conta de bytes.
        auto Mb = [](u64 Bytes) {
            const u64 Tenths = (Bytes * 10u + (1u << 19)) >> 20; // arredonda p/ 0,1 MiB
            return std::to_string(Tenths / 10u) + "," + std::to_string(Tenths % 10u) + " MB";
        };

        std::vector<std::pair<size_t, u64>> Sorted;
        for (size_t i = 0; i < static_cast<size_t>(EVramCategory::Count); ++i)
            if (Snap.Bytes[i] > 0) Sorted.emplace_back(i, Snap.Bytes[i]);
        std::sort(Sorted.begin(), Sorted.end(),
                  [](const auto& A, const auto& B) { return A.second > B.second; });

        // Mesma fonte que a janela de Estatisticas consome (ver LabelBreakdown), ja ordenada.
        const std::vector<FVramLabelEntry> Labels = LabelBreakdown();

        std::string Line = "[VRAM] rastreado " + Mb(Snap.TotalTracked) + " em " +
                           std::to_string(Snap.LiveResources) + " recursos";
        if (_LocalUsageBytes > 0) {
            Line += " | DXGI " + Mb(_LocalUsageBytes);
            // So faz sentido quando o DXGI ja contabilizou tudo que registramos; se vier menor
            // (recurso recem-criado que o driver ainda nao cobrou), imprimir a subtracao daria
            // um numero negativo disfarcado de u64.
            if (_LocalUsageBytes > Snap.TotalTracked)
                Line += " | nao rastreado " + Mb(_LocalUsageBytes - Snap.TotalTracked);
        }
        for (const auto& [Index, Bytes] : Sorted) {
            Line += "\n         " + std::string(CategoryName(static_cast<EVramCategory>(Index))) +
                    ": " + Mb(Bytes);
            u64  Labeled = 0;
            bool Any     = false;
            for (const FVramLabelEntry& E : Labels) {
                if (static_cast<size_t>(E.Category) != Index) continue;
                Line += "\n             " + std::string(E.Label) + ": " + Mb(E.Bytes);
                Labeled += E.Bytes;
                Any = true;
            }
            // O resto so aparece quando existe rotulo na categoria — numa categoria inteiramente
            // sem rotulo a linha seria uma copia do total, so ruido.
            if (Any && Bytes > Labeled)
                Line += "\n             (sem rotulo): " + Mb(Bytes - Labeled);
        }
        LogInfo(Line);
    }
}
