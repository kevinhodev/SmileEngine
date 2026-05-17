#pragma once

// Swap chain DXGI 1.4+ com FLIP_DISCARD e RTVs por buffer.

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "Smile/Core/Types.h"
#include "Smile/Graphics/DescriptorHeap.h"

namespace Smile {

class SwapChain {
public:
    static constexpr UINT kBufferCount = 2;
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    void Initialize(IDXGIFactory6* factory,
                    ID3D12CommandQueue* queue,
                    ID3D12Device* device,
                    HWND hwnd,
                    UINT width,
                    UINT height,
                    bool allowTearing);

    // Reconstroi os back buffers no novo tamanho. Pressupoe GPU idle.
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void Present();

    UINT CurrentIndex() const { return SwapChain->GetCurrentBackBufferIndex(); }
    ID3D12Resource* CurrentBackBuffer() const { return m_buffers[CurrentIndex()].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const { return m_rtvHeap.CpuHandle(CurrentIndex()); }

    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    void CreateRenderTargetViews(ID3D12Device* device);

    ComPtr<IDXGISwapChain3> SwapChain;
    ComPtr<ID3D12Resource>  m_buffers[kBufferCount];
    DescriptorHeap          m_rtvHeap;
    UINT m_width  = 0;
    UINT m_height = 0;
    bool m_allowTearing = false;
};

} // namespace Smile
