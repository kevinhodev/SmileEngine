#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"
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
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace Smile {
    i32 FOpacityMicromapData::IndexOf(u32 _Triangle) const {
        const size_t Offset = static_cast<size_t>(_Triangle) * IndexStride;
        if (_Triangle >= TriangleCount || Offset + IndexStride > Indices.size())
            throw std::out_of_range("Opacity Micromap: IndexOf fora do index buffer");
        switch (IndexStride) {
            case 1: { i8  V; std::memcpy(&V, Indices.data() + Offset, 1); return V; }
            case 2: { i16 V; std::memcpy(&V, Indices.data() + Offset, 2); return V; }
            case 4: { i32 V; std::memcpy(&V, Indices.data() + Offset, 4); return V; }
            default: throw std::logic_error("Opacity Micromap: IndexStride invalido");
        }
    }

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

        std::shared_ptr<FOpacityMicromapData> MakeAllUnknown(const FOmmGeometry& _Geometry,
                                                              f32 _BaseAlpha,
                                                              f32 _AlphaCutoff) {
            auto Out = std::make_shared<FOpacityMicromapData>();
            Out->TriangleCount    = _Geometry.IndexCount / 3u;
            Out->IndexFormat      = DXGI_FORMAT_R32_UINT;
            Out->IndexStride      = sizeof(u32);
            Out->BakedBaseAlpha   = _BaseAlpha;
            Out->BakedAlphaCutoff = _AlphaCutoff;
            Out->Indices.resize(static_cast<size_t>(Out->TriangleCount) * sizeof(u32));
            const i32 Special = D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE;
            for (u32 I = 0; I < Out->TriangleCount; ++I)
                std::memcpy(Out->Indices.data() + static_cast<size_t>(I) * sizeof(u32),
                            &Special, sizeof(Special));
            Out->WorkloadExcludedTriangles = Out->TriangleCount;
            Out->Coverage.FullyUnknownTriangles = Out->TriangleCount;
            Out->Coverage.KnownArea = 0.0f;
            if (!Out->Valid()) Fail("OMM all-unknown internamente inconsistente");
            return Out;
        }

        // As contagens da cobertura, lidas do index buffer FINAL — o unico que conhece os
        // triangulos que a divisao de workload tirou do bake.
        void CountCoverage(FOpacityMicromapData& _Out) {
            FOmmCoverage& C = _Out.Coverage;
            C.FullyKnownTriangles = C.MicromapTriangles = C.FullyUnknownTriangles = 0;
            for (u32 T = 0; T < _Out.TriangleCount; ++T) {
                const i32 I = _Out.IndexOf(T);
                if (I >= 0)
                    ++C.MicromapTriangles;
                else if (I == D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT ||
                         I == D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE)
                    ++C.FullyKnownTriangles;
                else
                    ++C.FullyUnknownTriangles;
            }
        }

        // Area do triangulo em espaco de UV — a mesma GetArea2D com que o SDK pondera o
        // knownAreaMetric. UV nao-finita nao tem area (o SDK tambem a deixa em zero).
        double UvArea(const FOmmGeometry& _Geometry, u32 _Triangle) {
            const auto* Base = reinterpret_cast<const u8*>(_Geometry.TexCoords);
            f32 P[6];
            for (u32 C = 0; C < 3; ++C)
                std::memcpy(&P[C * 2],
                            Base + static_cast<size_t>(_Geometry.Indices[_Triangle * 3u + C]) *
                                       _Geometry.TexCoordStride,
                            sizeof(f32) * 2);
            const double Cross = (static_cast<double>(P[2]) - P[0]) * (static_cast<double>(P[5]) - P[1]) -
                                 (static_cast<double>(P[4]) - P[0]) * (static_cast<double>(P[3]) - P[1]);
            const double Area = 0.5 * std::abs(Cross);
            return std::isfinite(Area) ? Area : 0.0;
        }

        // A DIVISAO de uma malha que estourou o teto de workload: quais triangulos vao ao baker e
        // quais saem como UNKNOWN.
        //
        // Por que existe: antes disto, WORKLOAD_TOO_BIG derrubava a malha INTEIRA para
        // FULLY_UNKNOWN_OPAQUE. No BistroExterior eram 10 malhas e 134 k triangulos — um sexto da
        // folhagem sem ganho nenhum de micro-map, por causa de poucos triangulos com UV enorme.
        //
        // E por que nao baixar o nivel de subdivisao e tentar de novo, que seria o obvio: o
        // workload do SDK NAO depende do nivel. ComputeWorkloadSize (bake_cpu_impl.cpp:662) soma a
        // area da AABB de UV em texels do mip 0, uma vez por triangulo de UV unico. Baixar 8 -> 4
        // devolveria o mesmo numero. O que reduz o workload e tirar os triangulos caros.
        //
        // O custo aqui e a MESMA conta, com a mesma deduplicacao. Mas em double: no SDK o produto
        // e int*int e da wrap acima de 2^31 texels.
        struct FWorkloadSplit {
            std::vector<u32> Kept;  // triangulos que vao ao baker, na ordem original
            u32 Excluded = 0;
            double TotalCost = 0.0; // workload da malha inteira, na conta do SDK
        };

        FWorkloadSplit SplitByWorkload(const FOmmGeometry& _Geometry, u32 _TexWidth,
                                       u32 _TexHeight, u64 _Budget) {
            struct FUvTri {
                u32 Bits[6];
                bool operator==(const FUvTri& O) const {
                    return std::memcmp(Bits, O.Bits, sizeof(Bits)) == 0;
                }
            };
            struct FUvTriHash {
                size_t operator()(const FUvTri& K) const {
                    return static_cast<size_t>(
                        OmmHashFinish(OmmHashAppend(kOmmHashSeed, K.Bits, sizeof(K.Bits))));
                }
            };

            const u32 TriangleCount = _Geometry.IndexCount / 3u;
            const auto* Base = reinterpret_cast<const u8*>(_Geometry.TexCoords);
            std::unordered_map<FUvTri, u32, FUvTriHash> UniqueId;
            UniqueId.reserve(TriangleCount);
            std::vector<double> Cost;
            std::vector<u32> UniqueOfTriangle(TriangleCount);

            for (u32 T = 0; T < TriangleCount; ++T) {
                FUvTri Key;
                f32 Uv[6];
                for (u32 C = 0; C < 3; ++C) {
                    const u32 V = _Geometry.Indices[T * 3u + C];
                    if (V >= _Geometry.VertexCount)
                        Fail("indice de vertice fora da malha na divisao de workload");
                    std::memcpy(&Uv[C * 2], Base + static_cast<size_t>(V) * _Geometry.TexCoordStride,
                                sizeof(f32) * 2);
                }
                std::memcpy(Key.Bits, Uv, sizeof(Key.Bits));
                const auto [It, Inserted] =
                    UniqueId.try_emplace(Key, static_cast<u32>(Cost.size()));
                UniqueOfTriangle[T] = It->second;
                if (!Inserted) continue;

                const f32 MinU = std::min({ Uv[0], Uv[2], Uv[4] });
                const f32 MaxU = std::max({ Uv[0], Uv[2], Uv[4] });
                const f32 MinV = std::min({ Uv[1], Uv[3], Uv[5] });
                const f32 MaxV = std::max({ Uv[1], Uv[3], Uv[5] });
                // UV nao-finita: o SDK nao bakeia (GetIsInvalid) e nao cobra nada por ela.
                double C = 0.0;
                if (std::isfinite(MinU) && std::isfinite(MaxU) &&
                    std::isfinite(MinV) && std::isfinite(MaxV))
                    C = std::floor((static_cast<double>(MaxU) - MinU) * _TexWidth) *
                        std::floor((static_cast<double>(MaxV) - MinV) * _TexHeight);
                Cost.push_back(C);
            }

            // Guloso pelos mais caros: com valor igual por triangulo, e o que mantem o MAIOR numero
            // deles dentro do teto.
            //
            // O que isto NAO e: um conserto para malha com "uns poucos triangulos de UV enorme". No
            // BistroExterior (2026-10-31) as 10 malhas que estouram sao o contrario — a maioria
            // dos triangulos e cara (34 192 de 37 366 numa delas), cards grandes no atlas que somam
            // varias vezes o teto. A divisao recuperou 21 k dos 134 k triangulos perdidos. O resto
            // pede um teto maior, e teto maior so cabe no bake OFFLINE.
            double Total = std::accumulate(Cost.begin(), Cost.end(), 0.0);
            const double MeshTotal = Total;
            std::vector<u32> Order(Cost.size());
            std::iota(Order.begin(), Order.end(), 0u);
            std::sort(Order.begin(), Order.end(),
                      [&](u32 A, u32 B) { return Cost[A] > Cost[B]; });
            std::vector<u8> Drop(Cost.size(), 0);
            for (u32 Id : Order) {
                if (Total <= static_cast<double>(_Budget)) break;
                Drop[Id] = 1;
                Total -= Cost[Id];
            }

            FWorkloadSplit Out;
            Out.TotalCost = MeshTotal;
            Out.Kept.reserve(TriangleCount);
            for (u32 T = 0; T < TriangleCount; ++T) {
                if (Drop[UniqueOfTriangle[T]]) ++Out.Excluded;
                else Out.Kept.push_back(T);
            }
            return Out;
        }
    }

    FOmmGeometry FOpacityMicromapBaker::GeometryOf(const FMesh& _Mesh) {
        if (_Mesh.Vertices.empty() || _Mesh.Indices.empty()) return {};
        FOmmGeometry G;
        G.TexCoords      = &_Mesh.Vertices.front().TexCoord[0];
        G.TexCoordStride = sizeof(Vertex);
        G.VertexCount    = static_cast<u32>(_Mesh.Vertices.size());
        G.Indices        = _Mesh.Indices.data();
        G.IndexCount     = static_cast<u32>(_Mesh.Indices.size());
        return G;
    }

    std::shared_ptr<FOpacityMicromapData> FOpacityMicromapBaker::Bake(
        const FMesh& _Mesh, const FTextureCPUData* _AlphaTexture,
        f32 _BaseAlpha, f32 _AlphaCutoff, bool _EnableInternalThreads,
        u32 _InternalThreadCount, u64 _MaxWorkloadSize) {
        // O teto de 32 bits e conferido AQUI, sobre o vetor, e nao na FOmmGeometry: la os
        // tamanhos ja sao u32, entao o estouro teria acontecido no cast silenciosamente.
        if (_Mesh.Vertices.size() > std::numeric_limits<u32>::max() ||
            _Mesh.Indices.size() > std::numeric_limits<u32>::max())
            Fail("mesh excede os limites de 32 bits do baker");
        return Bake(GeometryOf(_Mesh), _AlphaTexture, _BaseAlpha, _AlphaCutoff,
                    _EnableInternalThreads, _InternalThreadCount, _MaxWorkloadSize);
    }

    std::shared_ptr<FOpacityMicromapData> FOpacityMicromapBaker::Bake(
        const FOmmGeometry& _Geometry, const FTextureCPUData* _AlphaTexture,
        f32 _BaseAlpha, f32 _AlphaCutoff, bool _EnableInternalThreads,
        u32 _InternalThreadCount, u64 _MaxWorkloadSize) {
        if (!_Geometry.Valid())
            Fail("geometria de bake invalida (sem UV, sem indice, ou contagem nao-triangular)");

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
            Input.texCoords             = _Geometry.TexCoords;
            Input.texCoordStrideInBytes = _Geometry.TexCoordStride;
            Input.indexFormat           = omm::IndexFormat::UINT_32;
            Input.indexBuffer           = _Geometry.Indices;
            Input.indexCount            = _Geometry.IndexCount;
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
            // Acima dele os triangulos mais caros saem do bake com indice especial UNKNOWN (ver
            // SplitByWorkload); continua sendo a geometria DXR 1.2, e o alpha-test exato so roda
            // nesses triangulos. O bake offline passa kOmmNoWorkloadLimit, que e o "sem limite"
            // do proprio SDK — ai WORKLOAD_TOO_BIG nao acontece e a divisao nunca roda.
            const u64 kMaxWorkloadSize = _MaxWorkloadSize;
            Input.maxWorkloadSize = kMaxWorkloadSize;

            omm::Result BakeStatus = omm::Cpu::Bake(Baker, Input, &Result);

            // Estourou: bakear o que cabe em vez de desistir da malha. O orcamento de tempo e o
            // mesmo — o que vai ao baker continua abaixo do teto. A primeira tentativa usa o
            // proprio teto, e a conta e a do SDK, entao ela deve passar; reduzir pela metade cobre
            // uma divergencia entre as duas contas (o wrap de int do lado de la).
            const u32 TriangleCount = _Geometry.IndexCount / 3u;
            FWorkloadSplit Split;
            std::vector<u32> KeptIndices; // vivo ate a copia do resultado: o Input aponta para ca
            bool Divided = false;
            u64 Budget = kMaxWorkloadSize;
            for (u32 Attempt = 0; BakeStatus == omm::Result::WORKLOAD_TOO_BIG && Attempt < 4;
                 ++Attempt, Budget /= 2) {
                Split = SplitByWorkload(_Geometry, Alpha.Width, Alpha.Height, Budget);
                if (Split.Kept.empty()) break;
                KeptIndices.resize(Split.Kept.size() * 3u);
                for (size_t K = 0; K < Split.Kept.size(); ++K)
                    std::memcpy(&KeptIndices[K * 3u], &_Geometry.Indices[Split.Kept[K] * 3u],
                                sizeof(u32) * 3u);
                Input.indexBuffer = KeptIndices.data();
                Input.indexCount  = static_cast<u32>(KeptIndices.size());
                // 32 bits fixos: a costura abaixo encaixa os especiais dos excluidos no MESMO
                // buffer, e assim nao ha largura de indice para negociar com o que o baker escolheu.
                Input.bakeFlags = static_cast<omm::Cpu::BakeFlags>(
                    static_cast<u32>(Input.bakeFlags) |
                    static_cast<u32>(omm::Cpu::BakeFlags::Force32BitIndices));
                Divided = true;
                BakeStatus = omm::Cpu::Bake(Baker, Input, &Result);
            }

            if (BakeStatus == omm::Result::WORKLOAD_TOO_BIG) {
                auto Out = MakeAllUnknown(_Geometry, _BaseAlpha, _AlphaCutoff);
                LogWarning("[OMM] - Workload excedeu o teto e nem a divisao coube; usando indices "
                           "OMM UNKNOWN em " + std::to_string(Out->TriangleCount) + " triangulos");
                omm::Cpu::DestroyTexture(Baker, Texture); Texture = 0;
                omm::DestroyBaker(Baker); Baker = nullptr;
                return Out;
            }
            CheckOmm(BakeStatus, "Bake");
            const omm::Cpu::BakeResultDesc* Baked = nullptr;
            CheckOmm(omm::Cpu::GetBakeResultDesc(Result, &Baked), "GetBakeResultDesc");
            if (!Baked) Fail("baker devolveu descritor nulo");

            auto Out = std::make_shared<FOpacityMicromapData>();
            Out->TriangleCount = TriangleCount;
            Out->BakedBaseAlpha = _BaseAlpha;
            Out->BakedAlphaCutoff = _AlphaCutoff;
            const u32 BakedTriangles = Divided ? static_cast<u32>(Split.Kept.size()) : TriangleCount;
            if (Baked->indexCount != BakedTriangles)
                Fail("indice OMM nao possui uma entrada por triangulo bakeado");

            switch (Baked->indexFormat) {
                case omm::IndexFormat::UINT_8:
                    Out->IndexFormat = DXGI_FORMAT_R8_UINT; Out->IndexStride = 1; break;
                case omm::IndexFormat::UINT_16:
                    Out->IndexFormat = DXGI_FORMAT_R16_UINT; Out->IndexStride = 2; break;
                case omm::IndexFormat::UINT_32:
                    Out->IndexFormat = DXGI_FORMAT_R32_UINT; Out->IndexStride = 4; break;
                default: Fail("formato do index buffer OMM desconhecido");
            }
            if (Divided && Out->IndexStride != sizeof(u32))
                Fail("baker ignorou Force32BitIndices no bake dividido");

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

            Out->Indices.resize(static_cast<size_t>(TriangleCount) * Out->IndexStride);
            if (!Divided) {
                if (!Out->Indices.empty())
                    std::memcpy(Out->Indices.data(), Baked->indexBuffer, Out->Indices.size());
            } else {
                // A COSTURA: os excluidos com o especial, os bakeados no lugar de origem. Os
                // indices do baker apontam para Descs/ArrayData deste mesmo resultado, entao o
                // array nao muda — so a posicao de cada entrada no buffer da malha.
                const i32 Special =
                    D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE;
                for (u32 T = 0; T < TriangleCount; ++T)
                    std::memcpy(Out->Indices.data() + static_cast<size_t>(T) * sizeof(u32),
                                &Special, sizeof(Special));
                const auto* Src = static_cast<const u8*>(Baked->indexBuffer);
                for (size_t K = 0; K < Split.Kept.size(); ++K)
                    std::memcpy(Out->Indices.data() + static_cast<size_t>(Split.Kept[K]) * sizeof(u32),
                                Src + K * sizeof(u32), sizeof(u32));
                Out->WorkloadExcludedTriangles = Split.Excluded;
                // O multiplo do teto e o numero que decide o que fazer com o resto: perto de 1x,
                // subir o teto resolve; muitas vezes acima, so o bake offline.
                const double OverCap = Split.TotalCost / static_cast<double>(kMaxWorkloadSize);
                LogWarning("[OMM] - Workload " + std::to_string(OverCap).substr(0, 5) +
                           "x o teto; " + std::to_string(Split.Excluded) + " de " +
                           std::to_string(TriangleCount) +
                           " triangulos (os maiores em UV) ficam com indice UNKNOWN, o resto foi "
                           "bakeado");
            }
            if (!Out->Valid()) Fail("resultado do baker e internamente inconsistente");

            // COBERTURA. GetStats2, e nao GetStats: so a variante que recebe o handle do resultado
            // tem a area por triangulo; a outra devolve knownAreaMetric = 0 (debug_impl.cpp:638).
            CountCoverage(*Out);
            omm::Debug::Stats Stats{};
            CheckOmm(omm::Debug::GetStats2(Baker, Result, &Stats), "GetStats2");
            // Triangulo de UV toda degenerada deixa a area total em zero, e o SDK divide por ela.
            double Known = std::isfinite(Stats.knownAreaMetric) ? Stats.knownAreaMetric : 0.0;
            if (Divided) {
                // O SDK so mediu o que bakeou. Os excluidos sao area 100% desconhecida, e entram
                // no denominador com a area de UV deles.
                std::vector<u8> IsKept(TriangleCount, 0);
                for (u32 T : Split.Kept) IsKept[T] = 1;
                double AreaBaked = 0.0, AreaExcluded = 0.0;
                for (u32 T = 0; T < TriangleCount; ++T)
                    (IsKept[T] ? AreaBaked : AreaExcluded) += UvArea(_Geometry, T);
                const double AreaTotal = AreaBaked + AreaExcluded;
                Known = AreaTotal > 0.0 ? Known * AreaBaked / AreaTotal : 0.0;
            }
            Out->Coverage.KnownArea = static_cast<f32>(std::clamp(Known, 0.0, 1.0));

            LogDebug("[OMM] - Bake 4-state: " + std::to_string(Out->TriangleCount) +
                     " triangulos, " + std::to_string(Out->Descs.size()) +
                     " micro-maps, " + std::to_string(Out->ArrayData.size()) + " bytes | " +
                     std::to_string(Out->Coverage.FullyKnownTriangles) + " resolvidos, " +
                     std::to_string(Out->Coverage.MicromapTriangles) + " com micro-map, " +
                     std::to_string(Out->Coverage.FullyUnknownTriangles) + " UNKNOWN, area " +
                     std::to_string(static_cast<int>(Out->Coverage.KnownArea * 100.0f + 0.5f)) +
                     "% conhecida");

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
