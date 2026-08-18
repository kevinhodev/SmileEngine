#pragma once

#include <Windows.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/D3D12Device.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/UploadQueue.h"
#include "Smile/Graphics/Backend/D3D12/ComputeQueue.h"
#include "Smile/Graphics/Backend/D3D12/SwapChain.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Debug/GpuProfiler.h"

namespace Smile {
    // Ownership e lifecycle dos objetos que formam o backend D3D12 do renderer.
    class FRenderBackend {
    public:
        FRenderBackend() = default;
        ~FRenderBackend() noexcept;

        FRenderBackend(const FRenderBackend&) = delete;
        FRenderBackend& operator=(const FRenderBackend&) = delete;

        void Initialize(HWND Window, u32 Width, u32 Height, bool EnableDebugLayer);
        void FlushDirect();
        void Resize(u32 Width, u32 Height);
        void Present();
        void Shutdown();

        FD3D12Device       Device;
        FCommandQueue      DirectQueue;
        FUploadQueue       UploadQueue;
        FAsyncComputeQueue ComputeQueue;
        FGpuProfiler       DirectProfiler;
        FGpuProfiler       ComputeProfiler;
        FSwapChain         SwapChain;
        FTextureSRVHeap    SRVHeap;

    private:
        bool Initialized = false;
    };
}
