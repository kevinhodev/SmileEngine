#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Core/Logger.h"
#include <DirectXTex.h>
#include <omm.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(_OPENMP)
#include <omp.h>
#endif
#include <stdexcept>
#include <string>

namespace Smile {
    FOmmInputKey FOmmInputKey::From(const FGpuMesh* _Mesh, const FMaterial* _Material) {
        FOmmInputKey K{};
        if (!_Mesh || !_Material) return K;
        K.Mesh = _Mesh;
        // O plano de alpha do bake sai do BaseColor do material, que em runtime e o slot 0. Se o
        // material nao tem albedo, o bake usou uma textura branca — e o nulo representa isso.
        K.AlphaTexture = _Material->Albedo;
        K.BaseAlpha    = _Material->Constants.BaseColorFactor.W;
        K.AlphaCutoff  = _Material->Constants.AlphaCutoff;
        K.AlphaTest    = _Material->Constants.AlphaTest;
        return K;
    }

    namespace {
        struct FAlphaPlane {
            std::vector<float> Pixels;
            u32 Width  = 1;
            u32 Height = 1;
        };

        [[noreturn]] void Fail(const std::string& _Message) {
            throw std::runtime_error("Opacity Micromap: " + _Message);
        }

        void CheckOmm(omm::Result _Result, const char* _Operation) {
            if (_Result != omm::Result::SUCCESS)
                Fail(std::string(_Operation) + " falhou (omm::Result=" +
                     std::to_string(static_cast<int>(_Result)) + ")");
        }

        void OmmMessage(omm::MessageSeverity _Severity, const char* _Message, void*) {
            const std::string Text = std::string("[OMM Baker] ") +
                                     (_Message ? _Message : "mensagem vazia");
            switch (_Severity) {
                case omm::MessageSeverity::Fatal:       LogError(Text);   break;
                case omm::MessageSeverity::Warning:
                case omm::MessageSeverity::PerfWarning: LogWarning(Text); break;
                default:                                LogDebug(Text);   break;
            }
        }

        FAlphaPlane ExtractAlpha(const FTextureCPUData* _Texture, float _BaseAlpha) {
            FAlphaPlane Out;
            _BaseAlpha = std::clamp(_BaseAlpha, 0.0f, 1.0f);
            if (!_Texture || !_Texture->Valid()) {
                Out.Pixels = { _BaseAlpha };
                return Out;
            }

            const FMipData& Mip = _Texture->Mips.front();
            if (Mip.Width == 0 || Mip.Height == 0 || Mip.Pixels.empty())
                Fail("mip 0 da textura de alpha e invalida");

            size_t RowPitch = 0;
            size_t SlicePitch = 0;
            if (FAILED(DirectX::ComputePitch(_Texture->Format, Mip.Width, Mip.Height,
                                             RowPitch, SlicePitch)) ||
                SlicePitch > Mip.Pixels.size())
                Fail("layout da textura de alpha nao corresponde ao DXGI_FORMAT");

            DirectX::Image Source{};
            Source.width      = Mip.Width;
            Source.height     = Mip.Height;
            Source.format     = _Texture->Format;
            Source.rowPitch   = RowPitch;
            Source.slicePitch = SlicePitch;
            Source.pixels     = const_cast<u8*>(Mip.Pixels.data());

            DirectX::ScratchImage FloatImage;
            const DirectX::Image* Image = &Source;
            if (DirectX::IsCompressed(Source.format)) {
                if (FAILED(DirectX::Decompress(Source, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                               FloatImage)))
                    Fail("DirectXTex nao conseguiu descomprimir a textura BC de alpha");
                Image = FloatImage.GetImage(0, 0, 0);
            } else if (Source.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
                if (FAILED(DirectX::Convert(Source, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                            DirectX::TEX_FILTER_DEFAULT, 0.0f, FloatImage)))
                    Fail("DirectXTex nao conseguiu converter a textura de alpha para FP32");
                Image = FloatImage.GetImage(0, 0, 0);
            }
            if (!Image || !Image->pixels) Fail("imagem FP32 de alpha vazia");

            Out.Width  = static_cast<u32>(Image->width);
            Out.Height = static_cast<u32>(Image->height);
            Out.Pixels.resize(static_cast<size_t>(Out.Width) * Out.Height);
            for (u32 Y = 0; Y < Out.Height; ++Y) {
                const float* Row = reinterpret_cast<const float*>(
                    Image->pixels + static_cast<size_t>(Y) * Image->rowPitch);
                for (u32 X = 0; X < Out.Width; ++X)
                    Out.Pixels[static_cast<size_t>(Y) * Out.Width + X] =
                        std::clamp(Row[X * 4 + 3] * _BaseAlpha, 0.0f, 1.0f);
            }
            return Out;
        }

        D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT ToD3DFormat(u16 _Format) {
            switch (static_cast<omm::Format>(_Format)) {
                case omm::Format::OC1_2_State:
                    return D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE;
                case omm::Format::OC1_4_State:
                    return D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
                default: Fail("formato de micro-map desconhecido no resultado do baker");
            }
        }

        std::shared_ptr<FOpacityMicromapData> MakeAllUnknown(const FMesh& _Mesh,
                                                              f32 _BaseAlpha,
                                                              f32 _AlphaCutoff) {
            auto Out = std::make_shared<FOpacityMicromapData>();
            Out->TriangleCount    = static_cast<u32>(_Mesh.Indices.size() / 3);
            Out->IndexFormat      = DXGI_FORMAT_R32_UINT;
            Out->IndexStride      = sizeof(u32);
            Out->BakedBaseAlpha   = _BaseAlpha;
            Out->BakedAlphaCutoff = _AlphaCutoff;
            Out->Indices.resize(static_cast<size_t>(Out->TriangleCount) * sizeof(u32));
            const i32 Special = D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE;
            for (u32 I = 0; I < Out->TriangleCount; ++I)
                std::memcpy(Out->Indices.data() + static_cast<size_t>(I) * sizeof(u32),
                            &Special, sizeof(Special));
            if (!Out->Valid()) Fail("OMM all-unknown internamente inconsistente");
            return Out;
        }
    }

    std::shared_ptr<FOpacityMicromapData> FOpacityMicromapBaker::Bake(
        const FMesh& _Mesh, const FTextureCPUData* _AlphaTexture,
        f32 _BaseAlpha, f32 _AlphaCutoff, bool _EnableInternalThreads,
        u32 _InternalThreadCount) {
        if (_Mesh.Vertices.empty() || _Mesh.Indices.empty() || (_Mesh.Indices.size() % 3) != 0)
            Fail("mesh sem triangulos validos");
        if (_Mesh.Vertices.size() > std::numeric_limits<u32>::max() ||
            _Mesh.Indices.size() > std::numeric_limits<u32>::max())
            Fail("mesh excede os limites de 32 bits do baker");

        FAlphaPlane Alpha = ExtractAlpha(_AlphaTexture, _BaseAlpha);
        const float Cutoff = std::clamp(_AlphaCutoff, 0.0f, 1.0f);
        // O raster/HLSL usa >=; o baker classifica com >. Deslocar um ULP preserva a igualdade.
        const float BakeCutoff = std::nextafter(Cutoff, -std::numeric_limits<float>::infinity());

        omm::Baker Baker = nullptr;
        omm::Cpu::Texture Texture = 0;
        omm::Cpu::BakeResult Result = 0;
        try {
            omm::BakerCreationDesc BakerDesc{};
            BakerDesc.type = omm::BakerType::CPU;
            BakerDesc.messageInterface.messageCallback = OmmMessage;
            CheckOmm(omm::CreateBaker(BakerDesc, &Baker), "CreateBaker");

            omm::Cpu::TextureMipDesc Mip{};
            Mip.width       = Alpha.Width;
            Mip.height      = Alpha.Height;
            // OMM 1.9.2 interpreta rowPitch em TEXELS e multiplica internamente pelo tamanho do
            // formato. Zero seleciona o layout compacto (width texels) e evita contar sizeof(float)
            // duas vezes — isso fazia CreateTexture ler quatro vezes alem do plano FP32.
            Mip.rowPitch    = 0;
            Mip.textureData = Alpha.Pixels.data();

            omm::Cpu::TextureDesc TextureDesc{};
            TextureDesc.format      = omm::Cpu::TextureFormat::FP32;
            TextureDesc.mips        = &Mip;
            TextureDesc.mipCount    = 1;
            TextureDesc.alphaCutoff = BakeCutoff;
            CheckOmm(omm::Cpu::CreateTexture(Baker, TextureDesc, &Texture), "CreateTexture");

            omm::Cpu::BakeInputDesc Input{};
            Input.bakeFlags = omm::Cpu::BakeFlags::EnableValidation;
            if (_EnableInternalThreads) {
#if defined(_OPENMP)
                if (_InternalThreadCount > 0) {
                    omp_set_dynamic(0);
                    omp_set_num_threads(static_cast<int>(_InternalThreadCount));
                }
#else
                (void)_InternalThreadCount;
#endif
                Input.bakeFlags = static_cast<omm::Cpu::BakeFlags>(
                    static_cast<u32>(Input.bakeFlags) |
                    static_cast<u32>(omm::Cpu::BakeFlags::EnableInternalThreads));
            }
            Input.texture = Texture;
            Input.runtimeSamplerDesc.addressingMode = omm::TextureAddressMode::Wrap;
            Input.runtimeSamplerDesc.filter         = omm::TextureFilterMode::Linear;
            Input.alphaMode             = omm::AlphaMode::Test;
            Input.texCoordFormat        = omm::TexCoordFormat::UV32_FLOAT;
            Input.texCoords             = &_Mesh.Vertices.front().TexCoord[0];
            Input.texCoordStrideInBytes = sizeof(Vertex);
            Input.indexFormat           = omm::IndexFormat::UINT_32;
            Input.indexBuffer           = _Mesh.Indices.data();
            Input.indexCount            = static_cast<u32>(_Mesh.Indices.size());
            Input.dynamicSubdivisionScale = 2.0f;
            Input.rejectionThreshold      = 0.0f;
            Input.alphaCutoff              = BakeCutoff;
            Input.alphaCutoffLessEqual     = omm::OpacityState::Transparent;
            Input.alphaCutoffGreater       = omm::OpacityState::Opaque;
            Input.format                   = omm::Format::OC1_4_State;
            Input.unknownStatePromotion    = omm::UnknownStatePromotion::ForceOpaque;
            Input.unresolvedTriState       = omm::SpecialIndex::FullyUnknownOpaque;
            Input.maxSubdivisionLevel      = 8;
            // Limite UV/texel: impede ilhas patologicas de monopolizarem minutos do commit.
            // Acima dele anexamos indices especiais UNKNOWN ao BLAS OMM; continua sendo a
            // geometria DXR 1.2, e o alpha-test exato so roda nesses triangulos.
            Input.maxWorkloadSize = 1ull << 30;

            const omm::Result BakeStatus = omm::Cpu::Bake(Baker, Input, &Result);
            if (BakeStatus == omm::Result::WORKLOAD_TOO_BIG) {
                auto Out = MakeAllUnknown(_Mesh, _BaseAlpha, _AlphaCutoff);
                LogWarning("[OMM] - Workload excedeu 2^30; usando indices OMM UNKNOWN em " +
                           std::to_string(Out->TriangleCount) + " triangulos");
                omm::Cpu::DestroyTexture(Baker, Texture); Texture = 0;
                omm::DestroyBaker(Baker); Baker = nullptr;
                return Out;
            }
            CheckOmm(BakeStatus, "Bake");
            const omm::Cpu::BakeResultDesc* Baked = nullptr;
            CheckOmm(omm::Cpu::GetBakeResultDesc(Result, &Baked), "GetBakeResultDesc");
            if (!Baked) Fail("baker devolveu descritor nulo");

            auto Out = std::make_shared<FOpacityMicromapData>();
            Out->TriangleCount = static_cast<u32>(_Mesh.Indices.size() / 3);
            Out->BakedBaseAlpha = _BaseAlpha;
            Out->BakedAlphaCutoff = _AlphaCutoff;
            if (Baked->indexCount != Out->TriangleCount)
                Fail("indice OMM nao possui uma entrada por triangulo");

            switch (Baked->indexFormat) {
                case omm::IndexFormat::UINT_8:
                    Out->IndexFormat = DXGI_FORMAT_R8_UINT; Out->IndexStride = 1; break;
                case omm::IndexFormat::UINT_16:
                    Out->IndexFormat = DXGI_FORMAT_R16_UINT; Out->IndexStride = 2; break;
                case omm::IndexFormat::UINT_32:
                    Out->IndexFormat = DXGI_FORMAT_R32_UINT; Out->IndexStride = 4; break;
                default: Fail("formato do index buffer OMM desconhecido");
            }

            Out->ArrayData.resize(Baked->arrayDataSize);
            if (Baked->arrayDataSize)
                std::memcpy(Out->ArrayData.data(), Baked->arrayData, Baked->arrayDataSize);

            Out->Descs.reserve(Baked->descArrayCount);
            for (u32 I = 0; I < Baked->descArrayCount; ++I) {
                const omm::Cpu::OpacityMicromapDesc& Src = Baked->descArray[I];
                D3D12_RAYTRACING_OPACITY_MICROMAP_DESC Dst{};
                Dst.ByteOffset       = Src.offset;
                Dst.SubdivisionLevel = Src.subdivisionLevel;
                Dst.Format           = ToD3DFormat(Src.format);
                Out->Descs.push_back(Dst);
            }

            Out->Histogram.reserve(Baked->descArrayHistogramCount);
            for (u32 I = 0; I < Baked->descArrayHistogramCount; ++I) {
                const omm::Cpu::OpacityMicromapUsageCount& Src = Baked->descArrayHistogram[I];
                D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY Dst{};
                Dst.Count            = Src.count;
                Dst.SubdivisionLevel = Src.subdivisionLevel;
                Dst.Format           = ToD3DFormat(Src.format);
                Out->Histogram.push_back(Dst);
            }

            Out->Indices.resize(static_cast<size_t>(Baked->indexCount) * Out->IndexStride);
            if (!Out->Indices.empty())
                std::memcpy(Out->Indices.data(), Baked->indexBuffer, Out->Indices.size());
            if (!Out->Valid()) Fail("resultado do baker e internamente inconsistente");

            LogDebug("[OMM] - Bake 4-state: " + std::to_string(Out->TriangleCount) +
                     " triangulos, " + std::to_string(Out->Descs.size()) +
                     " micro-maps, " + std::to_string(Out->ArrayData.size()) + " bytes");

            omm::Cpu::DestroyBakeResult(Result); Result = 0;
            omm::Cpu::DestroyTexture(Baker, Texture); Texture = 0;
            omm::DestroyBaker(Baker); Baker = nullptr;
            return Out;
        } catch (...) {
            if (Result)  omm::Cpu::DestroyBakeResult(Result);
            if (Texture) omm::Cpu::DestroyTexture(Baker, Texture);
            if (Baker)   omm::DestroyBaker(Baker);
            throw;
        }
    }
}
