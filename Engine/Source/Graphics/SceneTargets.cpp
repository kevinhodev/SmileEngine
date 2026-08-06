#include "Smile/Graphics/SceneTargets.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/DepthConfig.h"
#include <cstring>
#include "Smile/Core/HResultCheck.h"

// Corpos movidos do Renderer.cpp sem reescrita. A unica mudanca e de PARAMETRO: o que era
// membro do Renderer (Device, SRVHeap) ou chamada dele (RenderWidth/RenderHeight) agora entra
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

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format               = DXGI_FORMAT_D32_FLOAT;
        ClearValue.DepthStencil.Depth   = kClearDepth; 
        ClearValue.DepthStencil.Stencil = 0;

        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &ClearValue,
            IID_PPV_ARGS(&DepthBuffer)));
        VramTracker::Register(DepthBuffer.Get(), EVramCategory::RenderTargets);

        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        DSVDesc.Texture2D.MipSlice = 0;
        _Device->CreateDepthStencilView(DepthBuffer.Get(), &DSVDesc, DSVHeap.CpuHandle(0));

        if (DepthSRVSlot == kInvalidSlot)
            DepthSRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MipLevels       = 1;
        SRVDesc.Texture2D.MostDetailedMip = 0;
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

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = NormalFormat;
        ResourceDesc.SampleDesc       = { 1, 0 }; 
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format   = NormalFormat;
        ClearValue.Color[0] = 0.5f; ClearValue.Color[1] = 0.5f; 
        ClearValue.Color[2] = 0.5f; ClearValue.Color[3] = 0.0f;

        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &ClearValue,
            IID_PPV_ARGS(&NormalBuffer)));
        VramTracker::Register(NormalBuffer.Get(), EVramCategory::RenderTargets);
        NormalBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = NormalFormat;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _Device->CreateRenderTargetView(NormalBuffer.Get(), &RTVDesc,
                                                NormalRTVHeap.CpuHandle(0));

        if (NormalSRVSlot == kInvalidSlot)
            NormalSRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = NormalFormat;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, NormalBuffer.Get(), SRVDesc, NormalSRVSlot);
    }

    void FSceneTargets::CreateHDRBuffers(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        UINT Width  = _Width;
        UINT Height = _Height;
        if (Width == 0 || Height == 0) return;

        HDRColorBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&HDRColorBuffer)));
        VramTracker::Register(HDRColorBuffer.Get(), EVramCategory::RenderTargets);

        if (!HDRRTVHeap.Native())
            HDRRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _Device->CreateRenderTargetView(HDRColorBuffer.Get(), &RTVDesc, HDRRTVHeap.CpuHandle(0));

        if (HDRSRVSlot == kInvalidSlot)
            HDRSRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, HDRColorBuffer.Get(), SRVDesc, HDRSRVSlot);
    }

    void FSceneTargets::CreateVelocityBuffer(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        const u32 Width = _Width, Height = _Height;
        if (Width == 0 || Height == 0) return;

        VelocityBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R16G16_FLOAT;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // RT (G-buffer escreve velocity) + UAV (passe de velocity do background preenche o ceu/nuvens/fog).
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&VelocityBuffer)));
        VramTracker::Register(VelocityBuffer.Get(), EVramCategory::RenderTargets);
        VelocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!VelocityRTVHeap.Native())
            VelocityRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _Device->CreateRenderTargetView(VelocityBuffer.Get(), &RTVDesc, VelocityRTVHeap.CpuHandle(0));

        if (VelocitySRVSlot == kInvalidSlot)
            VelocitySRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, VelocityBuffer.Get(), SRVDesc, VelocitySRVSlot);

        if (VelocityUavSlot == kInvalidSlot)
            VelocityUavSlot = _SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
        UAVDesc.Format        = DXGI_FORMAT_R16G16_FLOAT;
        UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _SRVHeap.CreateUAV(_Device, VelocityBuffer.Get(), UAVDesc, VelocityUavSlot);
    }

    void FSceneTargets::CreateUpscaleMasks(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                        u32 _Width, u32 _Height) {
        const u32 Width = _Width, Height = _Height;
        if (Width == 0 || Height == 0) return;

        UpscaleReactiveMask.Reset();
        UpscaleCompositionMask.Reset();

        if (!UpscaleMaskRTVHeap.Native())
            UpscaleMaskRTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = Width;
        Desc.Height           = Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R8_UNORM;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE Clear{};
        Clear.Format = Desc.Format;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &Clear, IID_PPV_ARGS(&UpscaleReactiveMask)));
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &Clear, IID_PPV_ARGS(&UpscaleCompositionMask)));
        VramTracker::Register(UpscaleReactiveMask.Get(), EVramCategory::RenderTargets);
        VramTracker::Register(UpscaleCompositionMask.Get(), EVramCategory::RenderTargets);
        UpscaleReactiveState = UpscaleCompositionState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_RENDER_TARGET_VIEW_DESC RTV{};
        RTV.Format = Desc.Format;
        RTV.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
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

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = static_cast<UINT16>(SceneColorMipCount);
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ResourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneColorCopy)));
        VramTracker::Register(SceneColorCopy.Get(), EVramCategory::RenderTargets);
        for (u32 Mip = 0; Mip < SceneColorMipCount; ++Mip)
            SceneColorMipStates[Mip] = D3D12_RESOURCE_STATE_COPY_DEST;

        ResourceDesc.MipLevels = 1;
        ResourceDesc.Flags     = D3D12_RESOURCE_FLAG_NONE;
        ResourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneDepthCopy)));
        VramTracker::Register(SceneDepthCopy.Get(), EVramCategory::RenderTargets);
        SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (SceneCopyTableStart == kInvalidSlot)
            SceneCopyTableStart = _SRVHeap.Allocate(2);
        if (SceneColorMipSRVStart == kInvalidSlot)
            SceneColorMipSRVStart = _SRVHeap.Allocate(kSceneColorMipMax);
        if (SceneColorMipUAVStart == kInvalidSlot)
            SceneColorMipUAVStart = _SRVHeap.Allocate(kSceneColorMipMax);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = SceneColorMipCount;
        _SRVHeap.CreateSRV(_Device, SceneColorCopy.Get(), SRVDesc, SceneCopyTableStart);

        SRVDesc.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
        UAVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (u32 Mip = 0; Mip < SceneColorMipCount; ++Mip) {
            SRVDesc.Texture2D.MostDetailedMip = Mip;
            _SRVHeap.CreateSRV(_Device, SceneColorCopy.Get(), SRVDesc,
                              SceneColorMipSRVStart + Mip);
            UAVDesc.Texture2D.MipSlice = Mip;
            _SRVHeap.CreateUAV(_Device, SceneColorCopy.Get(), UAVDesc,
                              SceneColorMipUAVStart + Mip);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC DSRV{};
        DSRV.Format                  = DXGI_FORMAT_R32_FLOAT;
        DSRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        DSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DSRV.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, SceneDepthCopy.Get(), DSRV, SceneCopyTableStart + 1);
    }

}
