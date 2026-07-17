#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>

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

                    float nx = 0, ny = 0, nz = 0;
                    for (int i = 0; i < 4; ++i) {
                        float vx = P[i][0] / 255.0f * 2.0f - 1.0f;
                        float vy = P[i][1] / 255.0f * 2.0f - 1.0f;
                        float vz = P[i][2] / 255.0f * 2.0f - 1.0f;
                        float len = std::sqrt(vx*vx + vy*vy + vz*vz);
                        if (len > 1e-6f) { vx /= len; vy /= len; vz /= len; }
                        nx += vx; ny += vy; nz += vz;
                    }
                    float sumLen = std::sqrt(nx*nx + ny*ny + nz*nz);
                    float T = sumLen / 4.0f; 
                    if (T < 1e-4f) T = 1e-4f;

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

    FTexture FTexture::RecordUpload(ID3D12Device* _Device, ID3D12GraphicsCommandList* _CommandList,
                                    FTextureSRVHeap& _SRVHeap,
                                    const std::vector<FMipData>& _Mips, DXGI_FORMAT _Format,
                                    std::vector<ComPtr<ID3D12Resource>>& _StagingOut,
                                    EVramCategory _Category) {
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

        // COMMON, nao COPY_DEST: recurso acessado em fila COPY tem que ENTRAR nela em
        // COMMON (a promotion COMMON->COPY_DEST acontece la; iniciar em COPY_DEST viola
        // o layout esperado e o debug layer reclama por subresource).
        ComPtr<ID3D12Resource> GPUTexture;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&GPUTexture)));
        VramTracker::Register(GPUTexture.Get(), _Category);

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Layouts(MipCount);
        std::vector<UINT>   NumRows(MipCount);
        std::vector<UINT64> RowSize(MipCount);
        UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&TextureDesc, 0, MipCount, 0,
                                       Layouts.data(), NumRows.data(),
                                       RowSize.data(), &TotalSize);

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
            const u32 SrcRowPitch = static_cast<u32>(RowSize[i]);
            const u8* SrcBase = _Mips[i].Pixels.data();
            u8*       DstBase = Mapped + Layouts[i].Offset;
            const u32 Rows    = NumRows[i];
            for (u32 Row = 0; Row < Rows; ++Row) {
                memcpy(DstBase + static_cast<UINT64>(Row) * Layouts[i].Footprint.RowPitch,
                       SrcBase + static_cast<UINT64>(Row) * SrcRowPitch, SrcRowPitch);
            }
        }
        Staging->Unmap(0, nullptr);

        for (u32 i = 0; i < MipCount; ++i) {
            D3D12_TEXTURE_COPY_LOCATION Src{};
            Src.pResource       = Staging.Get();
            Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint = Layouts[i];
            D3D12_TEXTURE_COPY_LOCATION Dst{};
            Dst.pResource        = GPUTexture.Get();
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = i;
            _CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }

        // Sem transition barrier: o upload roda na fila COPY (que nem aceita transicao pra
        // estado de shader) — a textura decai pra COMMON no fim do batch e promove pra
        // SRV implicitamente no primeiro read da fila direta.

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
        Result.GpuResource = GPUTexture;
        Result.Slot        = Slot;
        Result.TexWidth    = Width;
        Result.TexHeight   = Height;
        Result.TexMipCount = MipCount;
        Result.TexFormat   = _Format;
        _StagingOut.push_back(std::move(Staging));
        return Result;
    }

    std::vector<FTexture> FTexture::CreateBatchFromCPU(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                                                       FTextureSRVHeap& _SRVHeap,
                                                       const std::vector<FTextureCPUData>& _Data) {
        std::vector<FTexture> Out(_Data.size());
        constexpr size_t kStagingBudget = 256ull * 1024 * 1024;

        // Submit nao bloqueia: enquanto a GPU copia o batch N, o CPU ja prepara o N+1
        // (stagings retidos pela fila ate a fence). O chamador da o WaitIdle antes do
        // primeiro consumo (SceneLoader espera depois dos meshes).
        size_t i = 0;
        while (i < _Data.size()) {
            ID3D12GraphicsCommandList* CommandList = _UploadQueue.Begin();
            std::vector<ComPtr<ID3D12Resource>> Staging;
            size_t BatchBytes = 0;

            for (; i < _Data.size(); ++i) {
                if (!_Data[i].Valid()) continue;
                Out[i] = RecordUpload(_Device, CommandList, _SRVHeap,
                                      _Data[i].Mips, _Data[i].Format, Staging);
                for (const auto& m : _Data[i].Mips) BatchBytes += m.Pixels.size();
                if (BatchBytes >= kStagingBudget) { ++i; break; }
            }

            _UploadQueue.Submit(std::move(Staging));
        }
        return Out;
    }

    FTexture FTexture::Upload(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                               FTextureSRVHeap& _SRVHeap,
                               const std::vector<FMipData>& _Mips, DXGI_FORMAT _Format,
                               EVramCategory _Category) {
        ID3D12GraphicsCommandList* CommandList = _UploadQueue.Begin();
        std::vector<ComPtr<ID3D12Resource>> Staging;
        FTexture Result = RecordUpload(_Device, CommandList, _SRVHeap, _Mips, _Format, Staging,
                                       _Category);
        _UploadQueue.Submit(std::move(Staging));
        // Carga avulsa (default/lua/weather map): o chamador usa a textura em seguida,
        // entao espera aqui mesmo — o ganho de overlap e do caminho em batch.
        _UploadQueue.WaitIdle();
        return Result;
    }

    void FTexture::Release(FTextureSRVHeap& _SRVHeap) {
        if (GpuResource && Slot != kInvalidSlot) {
            _SRVHeap.Free(Slot, 1);
            Slot = kInvalidSlot;
        }
        GpuResource.Reset();
    }

    FTextureCPUData FTexture::LoadCPU(const std::wstring& _Path, bool _IsNormalMap, bool _sRGB) {
        FTextureCPUData Data;
        Data.IsNormalMap = _IsNormalMap;
        // Mesmos bytes RGBA decodificados por WIC; so a view muda (sRGB faz a HW linearizar no sample).
        Data.Format      = _sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        try {
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

            std::vector<FMipData>& Mips = Data.Mips;
            Mips.reserve(16);
            FMipData Mip0;
            Mip0.Width  = Width;
            Mip0.Height = Height;
            Mip0.Pixels.resize(static_cast<size_t>(Width) * Height * 4);
            SMILE_HR(Converter->CopyPixels(nullptr, Width * 4,
                                            static_cast<UINT>(Mip0.Pixels.size()),
                                            Mip0.Pixels.data()));

            if (_IsNormalMap) {
                for (size_t i = 0; i < Mip0.Pixels.size(); i += 4)
                    Mip0.Pixels[i + 3] = 255;
            }
            Mips.push_back(std::move(Mip0));

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

            Data.Width  = Width;
            Data.Height = Height;

            if (_IsNormalMap) {
                LogInfo("Normal map decoded: " + std::to_string(Width) + "x" + std::to_string(Height) +
                        ", " + std::to_string(Mips.size()) + " mips, Toksvig minAvg=" +
                        std::to_string(MinT));
            } else {
                LogInfo("Texture decoded: " + std::to_string(Width) + "x" + std::to_string(Height) +
                        ", " + std::to_string(Mips.size()) + " mips");
            }
        } catch (const std::exception& e) {
            LogError(std::string("Falha ao decodificar textura: ") + e.what());
            Data.Mips.clear(); 
        }
        return Data;
    }

    FTexture FTexture::CreateFromCPU(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                                     FTextureSRVHeap& _SRVHeap, const FTextureCPUData& _Data,
                                     EVramCategory _Category) {
        if (!_Data.Valid()) return FTexture{};
        return Upload(_Device, _UploadQueue, _SRVHeap, _Data.Mips, _Data.Format, _Category);
    }

    FTexture FTexture::LoadFromFile(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                                     FTextureSRVHeap& _SRVHeap,
                                     const std::wstring& _Path, bool _IsNormalMap) {
        return CreateFromCPU(_Device, _UploadQueue, _SRVHeap, LoadCPU(_Path, _IsNormalMap));
    }

    namespace {
        constexpr u32 MakeFourCC(char a, char b, char c, char d) {
            return static_cast<u32>(static_cast<u8>(a))
                 | (static_cast<u32>(static_cast<u8>(b)) << 8)
                 | (static_cast<u32>(static_cast<u8>(c)) << 16)
                 | (static_cast<u32>(static_cast<u8>(d)) << 24);
        }

        DXGI_FORMAT ToSRGB(DXGI_FORMAT _Fmt) {
            switch (_Fmt) {
                case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
                case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
                case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
                case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
                default:                    return _Fmt; 
            }
        }

        u32 BlockBytesFor(DXGI_FORMAT _Fmt) {
            switch (_Fmt) {
                case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
                case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
                    return 8;
                default:
                    return 16;
            }
        }
    }

    FTextureCPUData FTexture::LoadDDSCPU(const std::wstring& _Path, bool _sRGB) {
        FTextureCPUData Data;
        Data.IsNormalMap = false; 
        try {
            std::ifstream File(_Path, std::ios::binary | std::ios::ate);
            if (!File) throw std::runtime_error("nao abriu o arquivo");

            const std::streamoff Size = File.tellg();
            if (Size < 128) throw std::runtime_error("arquivo curto demais");
            File.seekg(0, std::ios::beg);
            std::vector<u8> Bytes(static_cast<size_t>(Size));
            File.read(reinterpret_cast<char*>(Bytes.data()), Size);
            if (!File) throw std::runtime_error("falha na leitura");

            const u8* P = Bytes.data();
            auto Rd32 = [&](size_t Off) -> u32 {
                u32 V; std::memcpy(&V, P + Off, 4); return V;
            };
            if (Rd32(0) != MakeFourCC('D','D','S',' ')) throw std::runtime_error("magic invalido");

            const u32 Height   = Rd32(12);
            const u32 Width     = Rd32(16);
            u32       MipCount  = Rd32(28);
            if (MipCount == 0) MipCount = 1;
            const u32 PFFlags  = Rd32(80);
            const u32 FourCC   = Rd32(84);

            DXGI_FORMAT Fmt = DXGI_FORMAT_UNKNOWN;
            size_t DataOffset = 128; 

            constexpr u32 DDPF_FOURCC = 0x4;
            if (!(PFFlags & DDPF_FOURCC)) throw std::runtime_error("DDS nao comprimido (sem FourCC) nao suportado");

            if (FourCC == MakeFourCC('D','X','1','0')) {
                Fmt = static_cast<DXGI_FORMAT>(Rd32(128));
                DataOffset = 148;
            } else if (FourCC == MakeFourCC('D','X','T','1')) {
                Fmt = DXGI_FORMAT_BC1_UNORM;
            } else if (FourCC == MakeFourCC('D','X','T','3')) {
                Fmt = DXGI_FORMAT_BC2_UNORM;
            } else if (FourCC == MakeFourCC('D','X','T','5')) {
                Fmt = DXGI_FORMAT_BC3_UNORM;
            } else if (FourCC == MakeFourCC('A','T','I','1') || FourCC == MakeFourCC('B','C','4','U')) {
                Fmt = DXGI_FORMAT_BC4_UNORM;
            } else if (FourCC == MakeFourCC('A','T','I','2') || FourCC == MakeFourCC('B','C','5','U')) {
                Fmt = DXGI_FORMAT_BC5_UNORM;
            } else {
                throw std::runtime_error("FourCC nao suportado");
            }

            if (_sRGB) Fmt = ToSRGB(Fmt);
            const u32 BlockBytes = BlockBytesFor(Fmt);

            std::vector<FMipData>& Mips = Data.Mips;
            Mips.reserve(MipCount);
            size_t Off = DataOffset;
            for (u32 i = 0; i < MipCount; ++i) {
                const u32 W = std::max(1u, Width  >> i);
                const u32 H = std::max(1u, Height >> i);
                const u32 BlocksW = std::max(1u, (W + 3) / 4);
                const u32 BlocksH = std::max(1u, (H + 3) / 4);
                const size_t Size = static_cast<size_t>(BlocksW) * BlocksH * BlockBytes;
                if (Off + Size > Bytes.size()) throw std::runtime_error("dados de mip truncados");

                FMipData Mip;
                Mip.Width  = W;
                Mip.Height = H;
                Mip.Pixels.assign(P + Off, P + Off + Size);
                Mips.push_back(std::move(Mip));
                Off += Size;
            }

            Data.Width  = Width;
            Data.Height = Height;
            Data.Format = Fmt;
        } catch (const std::exception& e) {
            LogError(std::string("Falha ao carregar DDS: ") + e.what());
            Data.Mips.clear();
        }
        return Data;
    }

    FTexture FTexture::LoadDDS(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                              FTextureSRVHeap& _SRVHeap, const std::wstring& _Path, bool _sRGB) {
        return CreateFromCPU(_Device, _UploadQueue, _SRVHeap, LoadDDSCPU(_Path, _sRGB));
    }

    FTexture FTexture::CreateDefault(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
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
        return Upload(_Device, _UploadQueue, _SRVHeap, Mips, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}
