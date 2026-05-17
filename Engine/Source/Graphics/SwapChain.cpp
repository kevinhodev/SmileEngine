#include "Smile/Graphics/SwapChain.h"

#include "Smile/Core/HResultCheck.h"

namespace Smile {

void SwapChain::Initialize(IDXGIFactory6* _Factory,
                           ID3D12CommandQueue* _CommandQueue,
                           ID3D12Device* _Device,
                           HWND _hWnd,
                           UINT _Width,
                           UINT _Height,
                           bool _AllowTearing) {
    m_width  = _Width;
    m_height = _Height;
    m_allowTearing = _AllowTearing;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = _Width;
    desc.Height      = _Height;
    desc.Format      = kFormat;
    desc.Stereo      = FALSE;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags       = _AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    ComPtr<IDXGISwapChain1> swap1;
    SMILE_HR(_Factory->CreateSwapChainForHwnd(_CommandQueue, _hWnd, &desc, nullptr, nullptr, &swap1));

    // Desativa Alt+Enter padrao do DXGI: Qt cuida do gerenciamento da janela
    SMILE_HR(_Factory->MakeWindowAssociation(_hWnd, DXGI_MWA_NO_ALT_ENTER));

    SMILE_HR(swap1.As(&SwapChain));

    // Heap RTV: 1 entrada por back buffer
    m_rtvHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBufferCount, false);
    CreateRenderTargetViews(_Device);
}

void SwapChain::CreateRenderTargetViews(ID3D12Device* device) {
    for (UINT i = 0; i < kBufferCount; ++i) {
        SMILE_HR(SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_buffers[i])));
        device->CreateRenderTargetView(m_buffers[i].Get(), nullptr, m_rtvHeap.CpuHandle(i));
    }
}

void SwapChain::Resize(ID3D12Device* device, UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return;
    }
    if (width == m_width && height == m_height) {
        return;
    }

    // Libera as referencias aos back buffers antigos antes do ResizeBuffers
    for (UINT i = 0; i < kBufferCount; ++i) {
        m_buffers[i].Reset();
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    SMILE_HR(SwapChain->GetDesc1(&desc));
    SMILE_HR(SwapChain->ResizeBuffers(kBufferCount, width, height, desc.Format, desc.Flags));

    m_width  = width;
    m_height = height;

    CreateRenderTargetViews(device);
}

void SwapChain::Present() {
    // Tearing requer Present(0, ALLOW_TEARING). VSync padrao caso contrario.
    const UINT syncInterval = m_allowTearing ? 0 : 1;
    const UINT flags        = m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    SMILE_HR(SwapChain->Present(syncInterval, flags));
}

} // namespace Smile
