#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

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

    namespace {
        // Average a 2x2 footprint of an RGBA8 source mip into one RGBA8 destination
        // texel using straight linear averaging on the 8-bit values. Adequate for
        // color/MR/AO/height/emissive — these aren't sRGB-decoded but the difference
        // vs proper sRGB-linear averaging is invisible on PBR content.
        void DownsampleColor2x2(const u8* Src, u32 SrcW, u32 SrcH,
                                u8* Dst, u32 DstW, u32 DstH) {
            for (u32 y = 0; y < DstH; ++y) {
                u32 sy0 = std::min(y * 2u,       SrcH - 1);
                u32 sy1 = std::min(y * 2u + 1u,  SrcH - 1);
                for (u32 x = 0; x < DstW; ++x) {
                    u32 sx0 = std::min(x * 2u,       SrcW - 1);
                    u32 sx1 = std::min(x * 2u + 1u,  SrcW - 1);
                    const u8* p00 = Src + (sy0 * SrcW + sx0) * 4;
                    const u8* p01 = Src + (sy0 * SrcW + sx1) * 4;
                    const u8* p10 = Src + (sy1 * SrcW + sx0) * 4;
                    const u8* p11 = Src + (sy1 * SrcW + sx1) * 4;
                    u8* d = Dst + (y * DstW + x) * 4;
                    for (u32 c = 0; c < 4; ++c)
                        d[c] = static_cast<u8>((u32(p00[c]) + p01[c] + p10[c] + p11[c] + 2) / 4);
                }
            }
        }

        // Normal-aware downsample: decodes 4 source normals from [0,1] to [-1,1],
        // averages them as vectors, re-normalizes, and re-encodes. The Toksvig
        // factor T = |sum|/N is written to alpha — the PS uses (1 - T²) as an
        // additive variance to roughness². Source RGB encodes the tangent-space
        // normal; alpha is overwritten with Toksvig.
        // Returns the average T over the destination mip (for logging).
        float DownsampleNormal2x2(const u8* Src, u32 SrcW, u32 SrcH,
                                  u8* Dst, u32 DstW, u32 DstH) {
            double TSum = 0.0;
            u32    TCount = 0;
            for (u32 y = 0; y < DstH; ++y) {
                u32 sy0 = std::min(y * 2u,       SrcH - 1);
                u32 sy1 = std::min(y * 2u + 1u,  SrcH - 1);
                for (u32 x = 0; x < DstW; ++x) {
                    u32 sx0 = std::min(x * 2u,       SrcW - 1);
                    u32 sx1 = std::min(x * 2u + 1u,  SrcW - 1);
                    const u8* P[4] = {
                        Src + (sy0 * SrcW + sx0) * 4,
                        Src + (sy0 * SrcW + sx1) * 4,
                        Src + (sy1 * SrcW + sx0) * 4,
                        Src + (sy1 * SrcW + sx1) * 4,
                    };
                    // Decode and sum.
                    float nx = 0, ny = 0, nz = 0;
                    for (int i = 0; i < 4; ++i) {
                        float vx = P[i][0] / 255.0f * 2.0f - 1.0f;
                        float vy = P[i][1] / 255.0f * 2.0f - 1.0f;
                        float vz = P[i][2] / 255.0f * 2.0f - 1.0f;
                        // Normalize each input to be safe — some normal maps
                        // come out of JPG slightly denormalized.
                        float len = std::sqrt(vx*vx + vy*vy + vz*vz);
                        if (len > 1e-6f) { vx /= len; vy /= len; vz /= len; }
                        nx += vx; ny += vy; nz += vz;
                    }
                    float sumLen = std::sqrt(nx*nx + ny*ny + nz*nz);
                    float T = sumLen / 4.0f; // = |sum| / N (each input has length 1)
                    if (T < 1e-4f) T = 1e-4f;
                    // Renormalize the average vector for the output normal.
                    float invLen = 1.0f / sumLen;
                    float ox = nx * invLen;
                    float oy = ny * invLen;
                    float oz = nz * invLen;
                    u8* d = Dst + (y * DstW + x) * 4;
                    d[0] = static_cast<u8>(std::clamp((ox * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    d[1] = static_cast<u8>(std::clamp((oy * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    d[2] = static_cast<u8>(std::clamp((oz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    d[3] = static_cast<u8>(std::clamp(T * 255.0f + 0.5f, 0.0f, 255.0f));
                    TSum  += T;
                    TCount += 1;
                }
            }
            return TCount > 0 ? static_cast<float>(TSum / TCount) : 1.0f;
        }
    }

    FTexture FTexture::Upload(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                               FTextureSRVHeap& _SRVHeap,
                               const std::vector<FMipData>& _Mips, DXGI_FORMAT _Format) {
        const u32 MipCount = static_cast<u32>(_Mips.size());
        const u32 Width    = _Mips[0].Width;
        const u32 Height   = _Mips[0].Height;

        D3D12_RESOURCE_DESC TextureDesc{};
        TextureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TextureDesc.Width            = Width;
        TextureDesc.Height           = Height;
        TextureDesc.DepthOrArraySize = 1;
        TextureDesc.MipLevels        = static_cast<UINT16>(MipCount);
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

        // GetCopyableFootprints for ALL mips.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Layouts(MipCount);
        std::vector<UINT>   NumRows(MipCount);
        std::vector<UINT64> RowSize(MipCount);
        UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&TextureDesc, 0, MipCount, 0,
                                       Layouts.data(), NumRows.data(),
                                       RowSize.data(), &TotalSize);

        // Single staging buffer covering all mips.
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

        for (u32 i = 0; i < MipCount; ++i) {
            const u32 SrcRowPitch = _Mips[i].Width * 4;
            const u8* SrcBase = _Mips[i].Pixels.data();
            u8*       DstBase = Mapped + Layouts[i].Offset;
            const u32 Rows    = NumRows[i];
            for (u32 Row = 0; Row < Rows; ++Row) {
                const u8* Src = SrcBase + Row * SrcRowPitch;
                u8*       Dst = DstBase + static_cast<UINT64>(Row) * Layouts[i].Footprint.RowPitch;
                memcpy(Dst, Src, SrcRowPitch);
            }
        }
        Staging->Unmap(0, nullptr);

        _CommandQueue.ResetForRecording();
        ID3D12GraphicsCommandList* CommandList = _CommandQueue.List();

        for (u32 i = 0; i < MipCount; ++i) {
            D3D12_TEXTURE_COPY_LOCATION Src{};
            Src.pResource       = Staging.Get();
            Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint = Layouts[i];

            D3D12_TEXTURE_COPY_LOCATION Dst{};
            Dst.pResource        = GPUTexture.Get();
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = i;

            CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }

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
        SRVDesc.Texture2D.MipLevels           = MipCount;
        SRVDesc.Texture2D.MostDetailedMip     = 0;
        SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, GPUTexture.Get(), SRVDesc, Slot);

        FTexture Result;
        Result.GpuResource = std::move(GPUTexture);
        Result.Slot        = Slot;
        Result.TexWidth    = Width;
        Result.TexHeight   = Height;
        Result.TexMipCount = MipCount;
        return Result;
    }

    FTexture FTexture::LoadFromFile(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                                     FTextureSRVHeap& _SRVHeap,
                                     const std::wstring& _Path, bool _IsNormalMap) {
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

        // Mip 0
        std::vector<FMipData> Mips;
        Mips.reserve(16);
        FMipData Mip0;
        Mip0.Width  = Width;
        Mip0.Height = Height;
        Mip0.Pixels.resize(static_cast<size_t>(Width) * Height * 4);
        SMILE_HR(Converter->CopyPixels(nullptr, Width * 4,
                                        static_cast<UINT>(Mip0.Pixels.size()),
                                        Mip0.Pixels.data()));

        // For normal maps, force the alpha of mip 0 to 1.0 (T = 1: no variance).
        // JPG/PNG normal maps don't carry a meaningful alpha — we own it now.
        if (_IsNormalMap) {
            for (size_t i = 0; i < Mip0.Pixels.size(); i += 4)
                Mip0.Pixels[i + 3] = 255;
        }
        Mips.push_back(std::move(Mip0));

        // Build the full mip chain. Stop when both dims hit 1.
        u32 W = Width, H = Height;
        float MinT = 1.0f;
        while (W > 1 || H > 1) {
            u32 PrevW = W;
            u32 PrevH = H;
            W = std::max(1u, W / 2);
            H = std::max(1u, H / 2);

            FMipData Next;
            Next.Width  = W;
            Next.Height = H;
            Next.Pixels.resize(static_cast<size_t>(W) * H * 4);

            const auto& Prev = Mips.back();
            if (_IsNormalMap) {
                float AvgT = DownsampleNormal2x2(Prev.Pixels.data(), PrevW, PrevH,
                                                 Next.Pixels.data(), W, H);
                MinT = std::min(MinT, AvgT);
            } else {
                DownsampleColor2x2(Prev.Pixels.data(), PrevW, PrevH,
                                   Next.Pixels.data(), W, H);
            }
            Mips.push_back(std::move(Next));
        }

        if (_IsNormalMap) {
            LogInfo("Normal map loaded: " + std::to_string(Width) + "x" + std::to_string(Height) +
                    ", " + std::to_string(Mips.size()) + " mips, Toksvig minAvg=" +
                    std::to_string(MinT));
        } else {
            LogInfo("Texture loaded: " + std::to_string(Width) + "x" + std::to_string(Height) +
                    ", " + std::to_string(Mips.size()) + " mips");
        }

        return Upload(_Device, _CommandQueue, _SRVHeap, Mips, DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    FTexture FTexture::CreateDefault(ID3D12Device* _Device, FCommandQueue& _CommandQueue,
                                      FTextureSRVHeap& _SRVHeap,
                                      EDefaultTexture _Type) {
        FMipData Mip;
        Mip.Width  = 1;
        Mip.Height = 1;
        Mip.Pixels.resize(4);
        switch (_Type) {
            case EDefaultTexture::White:
                Mip.Pixels = { 255, 255, 255, 255 };
                break;
            case EDefaultTexture::FlatNormal:
                // RGB = (0.5, 0.5, 1.0) = +Z normal; alpha = 1.0 (Toksvig: no variance).
                Mip.Pixels = { 128, 128, 255, 255 };
                break;
            case EDefaultTexture::Black:
                Mip.Pixels = { 0, 0, 0, 255 };
                break;
            case EDefaultTexture::MidGrey:
                Mip.Pixels = { 128, 128, 128, 255 };
                break;
            case EDefaultTexture::ORM:
                Mip.Pixels = { 255, 128, 0, 255 };
                break;
        }
        std::vector<FMipData> Mips;
        Mips.push_back(std::move(Mip));
        return Upload(_Device, _CommandQueue, _SRVHeap, Mips, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}
