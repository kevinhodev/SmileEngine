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
} 
