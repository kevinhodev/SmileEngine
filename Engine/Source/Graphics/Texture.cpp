#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace Smile {
    static ComPtr<IWICImagingFactory2> GetWICFactory() {
        static ComPtr<IWICImagingFactory2> Factory;
        if (!Factory) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SMILE_HR(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&Factory)));
        }
        return Factory;
    }

    static UINT64 AlignUp(UINT64 Size, UINT64 Align) {
        return (Size + Align - 1) & ~(Align - 1);
    }

    FTexture FTexture::Upload(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                               FTextureSRVHeap& _SRVHeap,
                               const u8* Pixels, u32 _Width, u32 _Height, DXGI_FORMAT _Format) {
        D3D12_RESOURCE_DESC TextureDesc{};
        TextureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TextureDesc.Width            = _Width;
        TextureDesc.Height           = _Height;
        TextureDesc.DepthOrArraySize = 1;
        TextureDesc.MipLevels        = 1;
        TextureDesc.Format           = _Format;
        TextureDesc.SampleDesc       = { 1, 0 };
        TextureDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TextureDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES DefaultHeap{};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        ComPtr<ID3D12Resource> GPUTexture;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&GPUTexture)));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT   NumRows   = 0;
        UINT64 RowSize   = 0;
        UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&TextureDesc, 0, 1, 0,
                                      &Layout, &NumRows, &RowSize, &TotalSize);

        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC BufferDesc{};
        BufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufferDesc.Width            = TotalSize;
        BufferDesc.Height           = 1;
        BufferDesc.DepthOrArraySize = 1;
        BufferDesc.MipLevels        = 1;
        BufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        BufferDesc.SampleDesc       = { 1, 0 };
        BufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufferDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        ComPtr<ID3D12Resource> Staging;
        SMILE_HR(_Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Staging)));

        u8* Mapped = nullptr;
        SMILE_HR(Staging->Map(0, nullptr, reinterpret_cast<void**>(&Mapped)));

        const u32 SrcRowPitch = _Width * 4; 
        for (u32 Row = 0; Row < _Height; ++Row) {
            const u8* Src = Pixels + Row * SrcRowPitch;
            u8*       Dst = Mapped + Layout.Offset + static_cast<UINT64>(Row) * Layout.Footprint.RowPitch;
            memcpy(Dst, Src, SrcRowPitch);
        }
        Staging->Unmap(0, nullptr);

        _CommandQueue.ResetForRecording();
        ID3D12GraphicsCommandList* CommandList = _CommandQueue.List();

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource       = Staging.Get();
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = Layout;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource        = GPUTexture.Get();
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = 0;

        CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

        D3D12_RESOURCE_BARRIER ResourceBarrier{};
        ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ResourceBarrier.Transition.pResource   = GPUTexture.Get();
        ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CommandList->ResourceBarrier(1, &ResourceBarrier);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* Lists[] = { CommandList };
        _CommandQueue.ExecuteAndSync(Lists, 1);

        u32 Slot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                        = _Format;
        SRVDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels           = 1;
        SRVDesc.Texture2D.MostDetailedMip     = 0;
        SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, GPUTexture.Get(), SRVDesc, Slot);

        FTexture Result;
        Result.GpuResource = std::move(GPUTexture);
        Result.Slot        = Slot;
        Result.TexWidth    = _Width;
        Result.TexHeight   = _Height;
        return Result;
    }

    FTexture FTexture::LoadFromFile(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                                     FTextureSRVHeap& _SRVHeap,
                                     const std::wstring& _Path) {
        auto Factory = GetWICFactory();

        ComPtr<IWICBitmapDecoder> Decoder;
        SMILE_HR(Factory->CreateDecoderFromFilename(
            _Path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &Decoder));

        ComPtr<IWICBitmapFrameDecode> Frame;
        SMILE_HR(Decoder->GetFrame(0, &Frame));

        ComPtr<IWICFormatConverter> Converter;
        SMILE_HR(Factory->CreateFormatConverter(&Converter));
        SMILE_HR(Converter->Initialize(
            Frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom));

        UINT Width = 0, Height = 0;
        SMILE_HR(Converter->GetSize(&Width, &Height));

        std::vector<u8> Pixels(static_cast<size_t>(Width) * Height * 4);
        SMILE_HR(Converter->CopyPixels(nullptr, Width * 4,
                                        static_cast<UINT>(Pixels.size()),
                                        Pixels.data()));

        LogInfo(std::string("Texture loaded: ") + std::to_string(Width) + "x" + std::to_string(Height));
        return Upload(_Device, _CommandQueue, _SRVHeap, Pixels.data(), Width, Height,
                      DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    FTexture FTexture::CreateDefault(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                                      FTextureSRVHeap& _SRVHeap,
                                      EDefaultTexture _Type) {
        u8 Pixel[4] = {};
        switch (_Type) {
            case EDefaultTexture::White:
                Pixel[0] = Pixel[1] = Pixel[2] = Pixel[3] = 255;
                break;
            case EDefaultTexture::FlatNormal:
                Pixel[0] = 128; Pixel[1] = 128; Pixel[2] = 255; Pixel[3] = 255;
                break;
            case EDefaultTexture::Black:
                Pixel[0] = Pixel[1] = Pixel[2] = 0; Pixel[3] = 255;
                break;
            case EDefaultTexture::MidGrey:
                Pixel[0] = Pixel[1] = Pixel[2] = 128; Pixel[3] = 255;
                break;
            case EDefaultTexture::ORM:
                Pixel[0] = 255; Pixel[1] = 128; Pixel[2] = 0; Pixel[3] = 255;
                break;
        }
        return Upload(_Device, _CommandQueue, _SRVHeap, Pixel, 1, 1,
                      DXGI_FORMAT_R8G8B8A8_UNORM);
    }
} 
