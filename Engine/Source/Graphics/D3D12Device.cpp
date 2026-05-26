#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <d3d12sdklayers.h>

namespace Smile {
    void FD3D12Device::Initialize(bool _EnableDebugLayer) {
        UINT FactoryFlags = 0;

        if (_EnableDebugLayer) {
            ComPtr<ID3D12Debug> DebugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController)))) {
                DebugController->EnableDebugLayer();
                FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LogInfo("D3D12 debug layer ativado");
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

        DXGI_ADAPTER_DESC1 AdapterDesc{};
        Adapter->GetDesc1(&AdapterDesc);

        AdapterDescription          = AdapterDesc.Description;
        AdapterDedicatedVideoMemory = static_cast<u64>(AdapterDesc.DedicatedVideoMemory);

        SMILE_HR(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

        LogInfo("D3D12 device criado");
    }
} 
