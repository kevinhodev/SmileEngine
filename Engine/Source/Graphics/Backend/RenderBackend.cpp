#include "Smile/Graphics/Backend/RenderBackend.h"

namespace Smile {
    FRenderBackend::~FRenderBackend() noexcept {
        try {
            Shutdown();
        } catch (...) {
            // Destruidores dos donos do backend nao podem propagar falhas de device/flush.
        }
    }

    void FRenderBackend::Initialize(HWND _Window, u32 _Width, u32 _Height,
                                    bool _EnableDebugLayer) {
        if (Initialized) return;

        Device.Initialize(_EnableDebugLayer);
        DirectQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        UploadQueue.Initialize(Device.Native());
        ComputeQueue.Initialize(Device.Native());
        DirectProfiler.Initialize(Device.Native(), DirectQueue.Native(),
                                  FCommandQueue::kFramesInFlight);
        ComputeProfiler.Initialize(Device.Native(), ComputeQueue.Native(),
                                   FAsyncComputeQueue::kSlots);
        SwapChain.Initialize(Device.GetFactory(), DirectQueue.Native(), Device.Native(),
                             _Window, _Width, _Height, Device.TearingSupported());
        SRVHeap.Initialize(Device.Native());
        Initialized = true;
    }

    void FRenderBackend::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        FlushDirect();
        SwapChain.Resize(Device.Native(), _Width, _Height);
    }

    void FRenderBackend::FlushDirect() {
        if (Initialized) DirectQueue.Flush();
    }

    void FRenderBackend::Present() {
        if (Initialized) SwapChain.Present();
    }

    void FRenderBackend::Shutdown() {
        if (!Initialized) return;
        DirectQueue.Flush();
        ComputeQueue.Shutdown();
        UploadQueue.Shutdown();
        Initialized = false;
    }
}
