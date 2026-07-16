#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <d3d12sdklayers.h>
#include <string>

namespace Smile {
    namespace {
        void CALLBACK D3D12DebugMessageCallback(D3D12_MESSAGE_CATEGORY,
                                                D3D12_MESSAGE_SEVERITY _Severity,
                                                D3D12_MESSAGE_ID,
                                                LPCSTR _Description, void*) {
            switch (_Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                case D3D12_MESSAGE_SEVERITY_ERROR:
                    LogError(std::string("[D3D12] ") + _Description);
                    break;
                case D3D12_MESSAGE_SEVERITY_WARNING:
                    LogWarning(std::string("[D3D12] ") + _Description);
                    break;
                default:
                    break; 
            }
        }
    }

    void FD3D12Device::Initialize(bool _EnableDebugLayer) {
        UINT FactoryFlags = 0;

        if (_EnableDebugLayer) {
            ComPtr<ID3D12Debug> DebugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController)))) {
                DebugController->EnableDebugLayer();
                FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LogInfo("[D3D12] - Debug Layer Ativado");
            } else {
                LogWarning("[D3D12] - Debug Layer Indisponivel (Graphics Tools nao Instalado?)");
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

        if (FAILED(Adapter.As(&Adapter3)))
            LogWarning("[D3D12] - IDXGIAdapter3 indisponivel; medicao de uso de VRAM desativada");

        SMILE_HR(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

        if (_EnableDebugLayer) {
            ComPtr<ID3D12InfoQueue1> InfoQueue;
            if (SUCCEEDED(Device.As(&InfoQueue))) {
                DWORD Cookie = 0;
                if (SUCCEEDED(InfoQueue->RegisterMessageCallback(
                        D3D12DebugMessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                        nullptr, &Cookie))) {
                    LogInfo("[D3D12] - Debug Messages Roteadas para o Logger");
                }
            } else {
                LogWarning("ID3D12InfoQueue1 indisponivel; erros da debug layer so no OutputDebugString");
            }
        }

        LogInfo("[D3D12] - Device Criado");

        if (SUCCEEDED(Device.As(&DeviceRT))) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5{};
            if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                      &Options5, sizeof(Options5)))) {
                IsRaytracingSupported =
                    (Options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
                LogInfo(std::string("[D3D12] - Raytracing Tier: ") +
                        std::to_string(static_cast<int>(Options5.RaytracingTier)));
            }
        }
        if (IsRaytracingSupported)
            LogInfo("[D3D12] - DXR Tier 1.1+ disponivel (inline ray tracing / GI habilitavel)");
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
