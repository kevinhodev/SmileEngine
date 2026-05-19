#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <d3d12sdklayers.h>
#include <format>

namespace Smile {
    void FD3D12Device::Initialize(bool _EnableDebugLayer) {
        UINT FactoryFlags = 0;

        if (_EnableDebugLayer) {
            ComPtr<ID3D12Debug> DebugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController)))) {
                DebugController->EnableDebugLayer();
                FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LogInfo("D3D12 - Debug Layer Ativado");
            } else {
                LogWarning("D3D12 debug layer indisponivel (Graphics Tools nao instalado?)");
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

        DXGI_ADAPTER_DESC1 desc{};
        Adapter->GetDesc1(&desc);

        AdapterDescription          = desc.Description;
        AdapterDedicatedVideoMemory = static_cast<u64>(desc.DedicatedVideoMemory);

        SMILE_HR(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

        // ── GPU info ─────────────────────────────────────────────────────────
        {
            std::string nameUtf8(AdapterDescription.begin(), AdapterDescription.end());
            double vramGb = static_cast<double>(AdapterDedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0);
            LogInfo(std::format("GPU: {} | VRAM: {:.1f} GB", nameUtf8, vramGb));
        }

        // ── Max feature level ─────────────────────────────────────────────────
        {
            constexpr D3D_FEATURE_LEVEL kLevels[] = {
                D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            };
            D3D12_FEATURE_DATA_FEATURE_LEVELS flData{};
            flData.NumFeatureLevels        = static_cast<UINT>(std::size(kLevels));
            flData.pFeatureLevelsRequested = kLevels;
            if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &flData, sizeof(flData)))) {
                const char* flStr =
                    flData.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_12_2 ? "12_2" :
                    flData.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_12_1 ? "12_1" :
                    flData.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_12_0 ? "12_0" :
                    flData.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0";
                LogInfo(std::format("D3D12 - Feature Level Máxima: {}", flStr));
            }
        }

        // ── MSAA support ──────────────────────────────────────────────────────
        {
            auto checkMSAA = [&](UINT samples) -> bool {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaaData{};
                msaaData.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
                msaaData.SampleCount = samples;
                if (FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                       &msaaData, sizeof(msaaData))))
                    return false;
                return msaaData.NumQualityLevels > 0;
            };
            bool msaa4  = checkMSAA(4);
            bool msaa8  = checkMSAA(8);
            LogInfo(std::format("MSAA Suportado: 4x={} 8x={}", msaa4 ? "Sim" : "Não", msaa8 ? "Sim" : "Não"));
        }

        LogInfo(std::format("Tearing (VSync off): {}", IsTearingSupported ? "Suportado" : "Não Suportado"));
    }
} 
