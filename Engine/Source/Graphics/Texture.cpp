#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

// WIC — built into Windows SDK, zero extra dependencies
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace Smile {

    // ---------------------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------------------

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

    // Aligns `size` up to the next multiple of `align`.
    static UINT64 AlignUp(UINT64 Size, UINT64 Align) {
        return (Size + Align - 1) & ~(Align - 1);
    }

    // ---------------------------------------------------------------------------
    // FTexture::Upload — core path: pixels → staging → DEFAULT heap
    // ---------------------------------------------------------------------------

    FTexture FTexture::Upload(ID3D12Device* Device, FCommandQueue& CmdQueue,
                               FTextureSRVHeap& SRVHeap,
                               const u8* Pixels, u32 W, u32 H, DXGI_FORMAT Format) {
        // --- 1. Create final texture on DEFAULT heap ---
        D3D12_RESOURCE_DESC TexDesc{};
        TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TexDesc.Width            = W;
        TexDesc.Height           = H;
        TexDesc.DepthOrArraySize = 1;
        TexDesc.MipLevels        = 1;
        TexDesc.Format           = Format;
        TexDesc.SampleDesc       = { 1, 0 };
        TexDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TexDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES DefaultHeap{};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        ComPtr<ID3D12Resource> GpuTex;
        SMILE_HR(Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TexDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&GpuTex)));

        // --- 2. Compute layout for the staging buffer ---
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT   NumRows   = 0;
        UINT64 RowSize   = 0;
        UINT64 TotalSize = 0;
        Device->GetCopyableFootprints(&TexDesc, 0, 1, 0,
                                      &Layout, &NumRows, &RowSize, &TotalSize);

        // --- 3. Create staging buffer on UPLOAD heap ---
        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC BufDesc{};
        BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufDesc.Width            = TotalSize;
        BufDesc.Height           = 1;
        BufDesc.DepthOrArraySize = 1;
        BufDesc.MipLevels        = 1;
        BufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        BufDesc.SampleDesc       = { 1, 0 };
        BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        ComPtr<ID3D12Resource> Staging;
        SMILE_HR(Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Staging)));

        // --- 4. Copy pixels into staging (row by row to handle pitch alignment) ---
        u8* Mapped = nullptr;
        SMILE_HR(Staging->Map(0, nullptr, reinterpret_cast<void**>(&Mapped)));

        const u32 SrcRowPitch = W * 4; // 4 bytes per pixel (RGBA8)
        for (u32 Row = 0; Row < H; ++Row) {
            const u8* Src = Pixels + Row * SrcRowPitch;
            u8*       Dst = Mapped + Layout.Offset + static_cast<UINT64>(Row) * Layout.Footprint.RowPitch;
            memcpy(Dst, Src, SrcRowPitch);
        }
        Staging->Unmap(0, nullptr);

        // --- 5. Record upload commands ---
        CmdQueue.ResetForRecording();
        ID3D12GraphicsCommandList* Cmd = CmdQueue.List();

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource       = Staging.Get();
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = Layout;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource        = GpuTex.Get();
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = 0;

        Cmd->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

        // Transition to PIXEL_SHADER_RESOURCE
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = GpuTex.Get();
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Cmd->ResourceBarrier(1, &Barrier);

        SMILE_HR(Cmd->Close());
        ID3D12CommandList* Lists[] = { Cmd };
        CmdQueue.ExecuteAndSync(Lists, 1);
        // Staging can now go out of scope safely (GPU is done with it after sync)

        // --- 6. Create SRV in the heap ---
        u32 Slot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                        = Format;
        SRVDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels           = 1;
        SRVDesc.Texture2D.MostDetailedMip     = 0;
        SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        SRVHeap.CreateSRV(Device, GpuTex.Get(), SRVDesc, Slot);

        FTexture Result;
        Result.GpuResource = std::move(GpuTex);
        Result.Slot        = Slot;
        Result.TexWidth    = W;
        Result.TexHeight   = H;
        return Result;
    }

    // ---------------------------------------------------------------------------
    // FTexture::LoadFromFile — WIC decoder → Upload
    // ---------------------------------------------------------------------------

    FTexture FTexture::LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                     FTextureSRVHeap& SRVHeap,
                                     const std::wstring& Path) {
        auto Factory = GetWICFactory();

        ComPtr<IWICBitmapDecoder> Decoder;
        SMILE_HR(Factory->CreateDecoderFromFilename(
            Path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &Decoder));

        ComPtr<IWICBitmapFrameDecode> Frame;
        SMILE_HR(Decoder->GetFrame(0, &Frame));

        // Convert to 32-bit RGBA (handles any source format)
        ComPtr<IWICFormatConverter> Converter;
        SMILE_HR(Factory->CreateFormatConverter(&Converter));
        SMILE_HR(Converter->Initialize(
            Frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom));

        UINT W = 0, H = 0;
        SMILE_HR(Converter->GetSize(&W, &H));

        std::vector<u8> Pixels(static_cast<size_t>(W) * H * 4);
        SMILE_HR(Converter->CopyPixels(nullptr, W * 4,
                                        static_cast<UINT>(Pixels.size()),
                                        Pixels.data()));

        LogInfo(std::string("Texture loaded: ") + std::to_string(W) + "x" + std::to_string(H));
        return Upload(Device, CmdQueue, SRVHeap, Pixels.data(), W, H,
                      DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    // ---------------------------------------------------------------------------
    // FTexture::CreateDefault — 1×1 fallback pixels
    // ---------------------------------------------------------------------------

    FTexture FTexture::CreateDefault(ID3D12Device* Device, FCommandQueue& CmdQueue,
                                      FTextureSRVHeap& SRVHeap,
                                      EDefaultTexture Type) {
        u8 Pixel[4] = {};
        switch (Type) {
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
                // R=occlusion(1), G=roughness(128≈0.5), B=metallic(0)
                Pixel[0] = 255; Pixel[1] = 128; Pixel[2] = 0; Pixel[3] = 255;
                break;
        }
        return Upload(Device, CmdQueue, SRVHeap, Pixel, 1, 1,
                      DXGI_FORMAT_R8G8B8A8_UNORM);
    }

} // namespace Smile
