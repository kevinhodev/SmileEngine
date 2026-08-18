#include "Smile/Graphics/Renderer/SceneTargets.h"
#include "Smile/Graphics/RHI/GpuResources.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/Renderer/DepthConfig.h"
#include <cstring>

// Render targets centrais da cena. GpuResources centraliza a criacao comum; heaps, slots e
// estados permanecem no agregado. Device, SRVHeap e as dimensoes entram
// por argumento — que e exatamente o que torna esta classe extraivel.
namespace Smile {

    void FSceneTargets::CreateDepthBuffer(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        UINT Width  = _Width;
        UINT Height = _Height;
        if (Width == 0 || Height == 0) return;

        if (!DSVHeap.Native())
            DSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

        DepthBuffer.Reset();

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format               = DXGI_FORMAT_D32_FLOAT;
        ClearValue.DepthStencil.Depth   = kClearDepth; 
        ClearValue.DepthStencil.Stencil = 0;

        DepthBuffer = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R32_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            EVramCategory::RenderTargets, &ClearValue);

        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        DSVDesc.Texture2D.MipSlice = 0;
        _Device->CreateDepthStencilView(DepthBuffer.Get(), &DSVDesc, DSVHeap.CpuHandle(0));

        if (DepthSRVSlot == kInvalidSlot)
            DepthSRVSlot = _SRVHeap.Allocate(1);

        const D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc =
            GpuResources::SrvTex2D(DXGI_FORMAT_R32_FLOAT);
        _SRVHeap.CreateSRV(_Device, DepthBuffer.Get(), SRVDesc, DepthSRVSlot);
    }

    void FSceneTargets::CreateNormalBuffer(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        UINT Width  = _Width;
        UINT Height = _Height;
        if (Width == 0 || Height == 0) return;

        if (!NormalRTVHeap.Native())
            NormalRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        NormalBuffer.Reset();

        const DXGI_FORMAT NormalFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format   = NormalFormat;
        ClearValue.Color[0] = 0.5f; ClearValue.Color[1] = 0.5f; 
        ClearValue.Color[2] = 0.5f; ClearValue.Color[3] = 0.0f;

        NormalBuffer = GpuResources::CreateTex2D(
            _Device, Width, Height, NormalFormat,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::RenderTargets, &ClearValue);
        NormalBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        const D3D12_RENDER_TARGET_VIEW_DESC RTVDesc = GpuResources::RtvTex2D(NormalFormat);
        _Device->CreateRenderTargetView(NormalBuffer.Get(), &RTVDesc,
                                                NormalRTVHeap.CpuHandle(0));

        if (NormalSRVSlot == kInvalidSlot)
            NormalSRVSlot = _SRVHeap.Allocate(1);

        const D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = GpuResources::SrvTex2D(NormalFormat);
        _SRVHeap.CreateSRV(_Device, NormalBuffer.Get(), SRVDesc, NormalSRVSlot);
    }

    void FSceneTargets::CreateHDRBuffers(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        UINT Width  = _Width;
        UINT Height = _Height;
        if (Width == 0 || Height == 0) return;

        HDRColorBuffer.Reset();

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        HDRColorBuffer = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            EVramCategory::RenderTargets, &ClearValue);

        if (!HDRRTVHeap.Native())
            HDRRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        const D3D12_RENDER_TARGET_VIEW_DESC RTVDesc =
            GpuResources::RtvTex2D(DXGI_FORMAT_R16G16B16A16_FLOAT);
        _Device->CreateRenderTargetView(HDRColorBuffer.Get(), &RTVDesc, HDRRTVHeap.CpuHandle(0));

        if (HDRSRVSlot == kInvalidSlot)
            HDRSRVSlot = _SRVHeap.Allocate(1);

        const D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc =
            GpuResources::SrvTex2D(DXGI_FORMAT_R16G16B16A16_FLOAT);
        _SRVHeap.CreateSRV(_Device, HDRColorBuffer.Get(), SRVDesc, HDRSRVSlot);
    }

    void FSceneTargets::CreateVelocityBuffer(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        const u32 Width = _Width, Height = _Height;
        if (Width == 0 || Height == 0) return;

        VelocityBuffer.Reset();

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        // RT (G-buffer escreve velocity) + UAV (passe de velocity do background preenche o ceu/nuvens/fog).
        VelocityBuffer = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            EVramCategory::RenderTargets, &ClearValue);
        VelocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!VelocityRTVHeap.Native())
            VelocityRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        const D3D12_RENDER_TARGET_VIEW_DESC RTVDesc =
            GpuResources::RtvTex2D(DXGI_FORMAT_R16G16_FLOAT);
        _Device->CreateRenderTargetView(VelocityBuffer.Get(), &RTVDesc, VelocityRTVHeap.CpuHandle(0));

        if (VelocitySRVSlot == kInvalidSlot)
            VelocitySRVSlot = _SRVHeap.Allocate(1);

        const D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc =
            GpuResources::SrvTex2D(DXGI_FORMAT_R16G16_FLOAT);
        _SRVHeap.CreateSRV(_Device, VelocityBuffer.Get(), SRVDesc, VelocitySRVSlot);

        if (VelocityUavSlot == kInvalidSlot)
            VelocityUavSlot = _SRVHeap.Allocate(1);
        const D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc =
            GpuResources::UavTex2D(DXGI_FORMAT_R16G16_FLOAT);
        _SRVHeap.CreateUAV(_Device, VelocityBuffer.Get(), UAVDesc, VelocityUavSlot);
    }

    void FSceneTargets::CreateUpscaleMasks(ID3D12Device* _Device, u32 _Width, u32 _Height) {
        const u32 Width = _Width, Height = _Height;
        if (Width == 0 || Height == 0) return;

        UpscaleReactiveMask.Reset();
        UpscaleCompositionMask.Reset();

        if (!UpscaleMaskRTVHeap.Native())
            UpscaleMaskRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format = DXGI_FORMAT_R8_UNORM;

        UpscaleReactiveMask = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::RenderTargets, &Clear);
        UpscaleCompositionMask = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::RenderTargets, &Clear);
        UpscaleReactiveState = UpscaleCompositionState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        const D3D12_RENDER_TARGET_VIEW_DESC RTV =
            GpuResources::RtvTex2D(DXGI_FORMAT_R8_UNORM);
        _Device->CreateRenderTargetView(
            UpscaleReactiveMask.Get(), &RTV, UpscaleMaskRTVHeap.CpuHandle(0));
        _Device->CreateRenderTargetView(
            UpscaleCompositionMask.Get(), &RTV, UpscaleMaskRTVHeap.CpuHandle(1));
    }

    void FSceneTargets::CreateSceneCopies(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        UINT Width = _Width, Height = _Height;
        if (Width == 0 || Height == 0) return;

        SceneColorCopy.Reset();
        SceneDepthCopy.Reset();

        SceneColorMipCount = 1;
        for (u32 Size = std::max(Width, Height);
             Size > 1 && SceneColorMipCount < kSceneColorMipMax; Size >>= 1)
            ++SceneColorMipCount;

        SceneColorCopy = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST,
            EVramCategory::RenderTargets, nullptr, SceneColorMipCount);
        for (u32 Mip = 0; Mip < SceneColorMipCount; ++Mip)
            SceneColorMipStates[Mip] = D3D12_RESOURCE_STATE_COPY_DEST;

        SceneDepthCopy = GpuResources::CreateTex2D(
            _Device, Width, Height, DXGI_FORMAT_R32_TYPELESS,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
            EVramCategory::RenderTargets);
        SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (SceneCopyTableStart == kInvalidSlot)
            SceneCopyTableStart = _SRVHeap.Allocate(2);
        if (SceneColorMipSRVStart == kInvalidSlot)
            SceneColorMipSRVStart = _SRVHeap.Allocate(kSceneColorMipMax);
        if (SceneColorMipUAVStart == kInvalidSlot)
            SceneColorMipUAVStart = _SRVHeap.Allocate(kSceneColorMipMax);

        const D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc =
            GpuResources::SrvTex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, SceneColorMipCount);
        _SRVHeap.CreateSRV(_Device, SceneColorCopy.Get(), SRVDesc, SceneCopyTableStart);

        for (u32 Mip = 0; Mip < SceneColorMipCount; ++Mip) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC MipSRV =
                GpuResources::SrvTex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, Mip);
            _SRVHeap.CreateSRV(_Device, SceneColorCopy.Get(), MipSRV,
                              SceneColorMipSRVStart + Mip);
            const D3D12_UNORDERED_ACCESS_VIEW_DESC MipUAV =
                GpuResources::UavTex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, Mip);
            _SRVHeap.CreateUAV(_Device, SceneColorCopy.Get(), MipUAV,
                              SceneColorMipUAVStart + Mip);
        }

        const D3D12_SHADER_RESOURCE_VIEW_DESC DSRV =
            GpuResources::SrvTex2D(DXGI_FORMAT_R32_FLOAT);
        _SRVHeap.CreateSRV(_Device, SceneDepthCopy.Get(), DSRV, SceneCopyTableStart + 1);
    }

}
