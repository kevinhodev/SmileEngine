#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace Smile {
    namespace {
        std::string WideToUtf8(std::wstring_view _Text) {
            if (_Text.empty()) return {};
            const int Length = WideCharToMultiByte(
                CP_UTF8, 0, _Text.data(), static_cast<int>(_Text.size()),
                nullptr, 0, nullptr, nullptr);
            if (Length <= 0) return "<nome indisponivel>";

            std::string Utf8(static_cast<size_t>(Length), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, _Text.data(), static_cast<int>(_Text.size()),
                Utf8.data(), Length, nullptr, nullptr);
            return Utf8;
        }

        std::string Hex4(unsigned _Value) {
            char Text[9]{};
            std::snprintf(Text, sizeof(Text), "%04X", _Value);
            return Text;
        }

        void CALLBACK D3D12DebugMessageCallback(D3D12_MESSAGE_CATEGORY _Category,
                                                D3D12_MESSAGE_SEVERITY _Severity,
                                                D3D12_MESSAGE_ID _Id,
                                                LPCSTR _Description, void*) {
            const std::string Prefix =
                "[D3D12 #" + std::to_string(static_cast<unsigned>(_Id)) +
                " cat=" + std::to_string(static_cast<unsigned>(_Category)) + "] ";
            switch (_Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                case D3D12_MESSAGE_SEVERITY_ERROR:
                    LogError(Prefix + _Description);
                    break;
                case D3D12_MESSAGE_SEVERITY_WARNING:
                    LogWarning(Prefix + _Description);
                    break;
                default:
                    break; 
            }
        }
    }

    FD3D12Device::~FD3D12Device() noexcept {
        if (DebugInfoQueue && DebugCallbackRegistered) {
            const HRESULT Hr = DebugInfoQueue->UnregisterMessageCallback(DebugCallbackCookie);
            if (FAILED(Hr)) {
                char Message[96]{};
                std::snprintf(Message, sizeof(Message),
                              "[D3D12] Falha ao remover callback de debug: HRESULT 0x%08lX",
                              static_cast<unsigned long>(Hr));
                LogWarning(Message);
            }
        }
        DebugCallbackRegistered = false;
        DebugCallbackCookie = 0;
        DebugInfoQueue.Reset();
    }

    void FD3D12Device::Initialize(bool _EnableDebugLayer) {
        UINT FactoryFlags = 0;

        if (_EnableDebugLayer) {
            ComPtr<ID3D12Debug> DebugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController)))) {
                DebugController->EnableDebugLayer();
                FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LogDebug("[D3D12] - Debug Layer Ativado");
            } else {
                LogDebug("[D3D12] - Debug Layer Indisponivel (Graphics Tools nao Instalado?)");
            }
        }

        SMILE_HR(CreateDXGIFactory2(FactoryFlags, IID_PPV_ARGS(&Factory)));

        BOOL AllowTearing = FALSE;
        if (SUCCEEDED(Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                      &AllowTearing,
                                                      sizeof(AllowTearing)))) {
            IsTearingSupported = (AllowTearing == TRUE);
        }

        SMILE_HR(Factory->EnumAdapterByGpuPreference(0,
                                                        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&Adapter)));

        DXGI_ADAPTER_DESC1 AdapterDesc{};
        Adapter->GetDesc1(&AdapterDesc);

        AdapterDescription          = AdapterDesc.Description;
        AdapterDedicatedVideoMemory = static_cast<u64>(AdapterDesc.DedicatedVideoMemory);
        LogDebug("[D3D12] - Adapter: " + WideToUtf8(AdapterDescription) +
                " | vendor=0x" + Hex4(AdapterDesc.VendorId) +
                " device=0x" + Hex4(AdapterDesc.DeviceId) +
                " | VRAM dedicada=" +
                std::to_string(AdapterDedicatedVideoMemory / (1024ull * 1024ull)) +
                " MiB | tearing=" + (IsTearingSupported ? "sim" : "nao"));

        if (FAILED(Adapter.As(&Adapter3)))
            LogWarning("[D3D12] - IDXGIAdapter3 indisponivel; medicao de uso de VRAM desativada");

        SMILE_HR(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

        if (_EnableDebugLayer) {
            if (SUCCEEDED(Device.As(&DebugInfoQueue))) {
                // Falso positivo conhecido do debug layer (Agility SDK ~1.616): em fila
                // COPY, o validador promove a textura COMMON->COPY_DEST na propria copia
                // e dai acusa INCOMPATIBLE_BARRIER_LAYOUT (#1334) "LEGACY_COPY_DEST !=
                // COMMON" em TODO CopyTextureRegion — mesmo com o recurso criado em
                // COMMON, que e o correto (regra: recurso entra na fila COPY em COMMON).
                // Corrigido em SDKs posteriores ("promoted COPY_DEST not decaying back
                // to COMMON"); suprimido ate o runtime local atualizar.
                D3D12_MESSAGE_ID DeniedIds[] = {
                    static_cast<D3D12_MESSAGE_ID>(1334) // INCOMPATIBLE_BARRIER_LAYOUT
                };
                D3D12_INFO_QUEUE_FILTER Filter{};
                Filter.DenyList.NumIDs  = _countof(DeniedIds);
                Filter.DenyList.pIDList = DeniedIds;
                DebugInfoQueue->PushStorageFilter(&Filter);

                DWORD Cookie = 0;
                if (SUCCEEDED(DebugInfoQueue->RegisterMessageCallback(
                        D3D12DebugMessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                        nullptr, &Cookie))) {
                    DebugCallbackCookie = Cookie;
                    DebugCallbackRegistered = true;
                    LogDebug("[D3D12] - Debug Messages Roteadas para o Logger");
                }
            } else {
                LogDebug("ID3D12InfoQueue1 indisponivel; erros da debug layer so no OutputDebugString");
            }
        }

        LogDebug("[D3D12] - Device Criado");

        if (SUCCEEDED(Device.As(&DeviceRT))) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5{};
            if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                      &Options5, sizeof(Options5)))) {
                IsRaytracingSupported =
                    (Options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
                LogDebug(std::string("[D3D12] - Raytracing Tier: ") +
                        std::to_string(static_cast<int>(Options5.RaytracingTier)));
            }
        }
        if (IsRaytracingSupported)
            LogDebug("[D3D12] - DXR Tier 1.1+ disponivel (inline ray tracing / GI habilitavel)");
        else
            LogWarning("[D3D12] - DXR indisponivel (Tier < 1.1); GI sera desativada");
    }

    const FVideoMemoryInfo& FD3D12Device::QueryVideoMemory() const {
        if (!Adapter3) return VideoMemoryCache;

        const auto Now = std::chrono::steady_clock::now();
        if (VideoMemoryCache.Valid &&
            Now - VideoMemoryQueryTime < std::chrono::milliseconds(500))
            return VideoMemoryCache;
        VideoMemoryQueryTime = Now;

        DXGI_QUERY_VIDEO_MEMORY_INFO Local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO NonLocal{};
        if (FAILED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Local)) ||
            FAILED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &NonLocal)))
            return VideoMemoryCache;

        VideoMemoryCache.LocalUsage     = Local.CurrentUsage;
        VideoMemoryCache.LocalBudget    = Local.Budget;
        VideoMemoryCache.NonLocalUsage  = NonLocal.CurrentUsage;
        VideoMemoryCache.NonLocalBudget = NonLocal.Budget;
        VideoMemoryCache.DemotedBytes =
            Local.CurrentUsage > Local.Budget ? Local.CurrentUsage - Local.Budget : 0;
        VideoMemoryCache.OverBudget = VideoMemoryCache.DemotedBytes > 0;
        VideoMemoryCache.Valid      = true;

        // Loga so na transicao (entrar/sair do over-budget), nao a cada snapshot.
        if (VideoMemoryCache.OverBudget != WasOverBudget) {
            WasOverBudget = VideoMemoryCache.OverBudget;
            const auto ToMiB = [](u64 _Bytes) { return _Bytes / (1024ull * 1024ull); };
            if (WasOverBudget)
                LogWarning("[D3D12] - VRAM acima do budget do OS: uso " +
                           std::to_string(ToMiB(Local.CurrentUsage)) + " MiB / budget " +
                           std::to_string(ToMiB(Local.Budget)) +
                           " MiB — recursos podem ser despejados pra RAM (stutter)");
            else
                LogInfo("[D3D12] - VRAM de volta dentro do budget do OS");
        }
        return VideoMemoryCache;
    }
}
