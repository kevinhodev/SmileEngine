#include "Smile/Graphics/SwapChain.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FSwapChain::Initialize(IDXGIFactory6* _Factory,
                               ID3D12CommandQueue* _CommandQueue,
                               ID3D12Device* _Device,
                               HWND _hWnd,
                               UINT _Width,
                               UINT _Height,
                               bool _AllowTearing) {
        Width  = _Width;
        Height = _Height;
        AllowTearing = _AllowTearing;

        DXGI_SWAP_CHAIN_DESC1 SwapChainDesc{};
        SwapChainDesc.Width       = _Width;
        SwapChainDesc.Height      = _Height;
        SwapChainDesc.Format      = kFormat;
        SwapChainDesc.Stereo      = FALSE;
        SwapChainDesc.SampleDesc  = { 1, 0 };
        SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        SwapChainDesc.BufferCount = kBufferCount;
        SwapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
        SwapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        SwapChainDesc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
        SwapChainDesc.Flags       = _AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

        ComPtr<IDXGISwapChain1> Swap1;
        SMILE_HR(_Factory->CreateSwapChainForHwnd(_CommandQueue, _hWnd, &SwapChainDesc, nullptr, nullptr, &Swap1));

        SMILE_HR(_Factory->MakeWindowAssociation(_hWnd, DXGI_MWA_NO_ALT_ENTER));

        SMILE_HR(Swap1.As(&SwapChain));

        RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBufferCount, false);
        CreateRenderTargetViews(_Device);
    }

    void FSwapChain::CreateRenderTargetViews(ID3D12Device* _Device) {
        for (UINT i = 0; i < kBufferCount; ++i) {
            SMILE_HR(SwapChain->GetBuffer(i, IID_PPV_ARGS(&Buffers[i])));
            _Device->CreateRenderTargetView(Buffers[i].Get(), nullptr, RTVHeap.CpuHandle(i));
        }
    }

    void FSwapChain::Resize(ID3D12Device* _Device, UINT _Width, UINT _Height) {
        if (_Width == 0 || _Height == 0) {
            return;
        }
        if (_Width == Width && _Height == Height) {
            return;
        }

        for (UINT i = 0; i < kBufferCount; ++i) {
            Buffers[i].Reset();
        }

        DXGI_SWAP_CHAIN_DESC1 SwapChainDesc{};
        SMILE_HR(SwapChain->GetDesc1(&SwapChainDesc));
        SMILE_HR(SwapChain->ResizeBuffers(kBufferCount, _Width, _Height, SwapChainDesc.Format, SwapChainDesc.Flags));

        Width  = _Width;
        Height = _Height;

        CreateRenderTargetViews(_Device);
    }

    void FSwapChain::Present() {
        const UINT SyncInterval = AllowTearing ? 0 : 1;
        const UINT Flags        = AllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        SMILE_HR(SwapChain->Present(SyncInterval, Flags));
    }
} 
