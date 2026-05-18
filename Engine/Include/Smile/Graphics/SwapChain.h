#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/DescriptorHeap.h"

namespace Smile {
    class FSwapChain {
    public:
        static constexpr UINT kBufferCount = 2;
        static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        void Initialize(IDXGIFactory6* Factory,
                        ID3D12CommandQueue* CommandQueue,
                        ID3D12Device* Device,
                        HWND hWnd,
                        UINT Width,
                        UINT Height,
                        bool AllowTearing);

        void Resize(ID3D12Device* Device, UINT Width, UINT Height);

        void Present();

        UINT CurrentIndex() const { return SwapChain->GetCurrentBackBufferIndex(); }
        ID3D12Resource* CurrentBackBuffer() const { return Buffers[CurrentIndex()].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const { return RTVHeap.CpuHandle(CurrentIndex()); }

        UINT GetWidth() const { return Width; }
        UINT GetHeight() const { return Height; }

    private:
        void CreateRenderTargetViews(ID3D12Device* Device);

        ComPtr<IDXGISwapChain3> SwapChain;
        ComPtr<ID3D12Resource>  Buffers[kBufferCount];
        FDescriptorHeap          RTVHeap;
        UINT Width  = 0;
        UINT Height = 0;
        bool AllowTearing = false;
    };
} 
