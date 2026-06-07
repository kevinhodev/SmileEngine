#include "Smile/Graphics/Water.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>
#include <cmath>
#include <stdexcept>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadShader(const std::string& _Name) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Name;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir shader de agua: " + FullPath);
                throw std::runtime_error("Water shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    void FWaterRenderer::BuildRootSignature(ID3D12Device* _Device) {
        // t0 = cubemap especular (reflexao IBL, PS).
        D3D12_DESCRIPTOR_RANGE SpecRange{};
        SpecRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SpecRange.NumDescriptors                    = 1;
        SpecRange.BaseShaderRegister                = 0; // t0
        SpecRange.RegisterSpace                     = 0;
        SpecRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t1 = displacement do FFT (VS desloca o vertice + calcula normal; PS le no futuro).
        D3D12_DESCRIPTOR_RANGE FFTRange{};
        FFTRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        FFTRange.NumDescriptors                    = 1;
        FFTRange.BaseShaderRegister                = 1; // t1
        FFTRange.RegisterSpace                     = 0;
        FFTRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t4 = normal mipped derivado do FFT (PS bump + Toksvig).
        D3D12_DESCRIPTOR_RANGE FFTNormalRange{};
        FFTNormalRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        FFTNormalRange.NumDescriptors                    = 1;
        FFTNormalRange.BaseShaderRegister                = 4; // t4
        FFTNormalRange.RegisterSpace                     = 0;
        FFTNormalRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE AtmoSkyViewRange{};
        AtmoSkyViewRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        AtmoSkyViewRange.NumDescriptors                    = 1;
        AtmoSkyViewRange.BaseShaderRegister                = 5; // t5
        AtmoSkyViewRange.RegisterSpace                     = 0;
        AtmoSkyViewRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t2..t3 = scene-color + scene-depth (refracao/fog, Etapa 3) — tabela contigua, PS.
        D3D12_DESCRIPTOR_RANGE SceneRange{};
        SceneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SceneRange.NumDescriptors                    = 2;
        SceneRange.BaseShaderRegister                = 2; // t2, t3
        SceneRange.RegisterSpace                     = 0;
        SceneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[6]{};
        // b0 = WaterConstants (VS reconstroi o grid; PS faz o shading) -> visivel a todos.
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; // b0
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SpecRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &FFTRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL; // VS + PS

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &SceneRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &FFTNormalRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[5].DescriptorTable.pDescriptorRanges   = &AtmoSkyViewRange;
        RootParams[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0 = linear wrap (FFT displacement) -> visivel a VS+PS (o VS amostra o FFT);
        // s1 = linear clamp (cubemap, so PS).
        D3D12_STATIC_SAMPLER_DESC Samplers[3]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0; // s0
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS amostra o FFT

        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].ShaderRegister   = 1; // s1
        Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        Samplers[2]                  = Samplers[0];
        Samplers[2].Filter           = D3D12_FILTER_ANISOTROPIC;
        Samplers[2].MaxAnisotropy    = 16;
        Samplers[2].ShaderRegister   = 2; // s2
        Samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Water root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));
    }

    void FWaterRenderer::BuildPSO(ID3D12Device* _Device, u32 _SampleCount,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        // NAME_WE no CMake gera .cso flat (sem prefixo de pasta), como Clouds/Atmosphere.
        auto VS = LoadShader("WaterSurface.vs_6_0.cso");
        auto PS = LoadShader("WaterSurface.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, sizeof(Vec4),
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // opaco

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; // superficie escreve depth
        Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        Depth.StencilEnable  = FALSE;

        auto CreateWaterPSO = [&](D3D12_FILL_MODE _FillMode,
                                  Microsoft::WRL::ComPtr<ID3D12PipelineState>& _Out) {
            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode              = _FillMode;
            Raster.CullMode              = D3D12_CULL_MODE_NONE; // agua vista dos dois lados
            Raster.FrontCounterClockwise = FALSE;
            Raster.DepthClipEnable       = TRUE;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
            Desc.pRootSignature        = RootSignature.Get();
            Desc.VS                    = { VS.data(), VS.size() };
            Desc.PS                    = { PS.data(), PS.size() };
            Desc.BlendState            = Blend;
            Desc.SampleMask            = UINT_MAX;
            Desc.RasterizerState       = Raster;
            Desc.DepthStencilState     = Depth;
            Desc.InputLayout           = { InputLayout, _countof(InputLayout) };
            Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            Desc.NumRenderTargets      = 1;
            Desc.RTVFormats[0]         = _RTFormat;
            Desc.DSVFormat             = _DSFormat;
            Desc.SampleDesc            = { _SampleCount, 0 };

            SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&_Out)));
        };

        CreateWaterPSO(D3D12_FILL_MODE_SOLID, PSO);
        CreateWaterPSO(D3D12_FILL_MODE_WIREFRAME, WireframePSO);
    }

    void FWaterRenderer::BuildGrid(ID3D12Device* _Device) {
        // Grade local em [0,1]^2 reutilizada por todos os tiles da quadtree. z guarda
        // edge Chebyshev para debug. Os indices sao gerados em subsets estilo Asylum:
        // lod interno (levelsize 32/16/8) x 81 patterns (3^4 bordas).
        const u32 N = kGridPoints;
        const u32 MeshDim = kGridPoints - 1;
        const f32 Rcp = 1.0f / static_cast<f32>(N - 1);

        std::vector<f32> Verts;
        Verts.reserve(static_cast<size_t>(N) * N * 3);
        auto PushVertex = [&Verts](f32 _U, f32 _V, f32 _Tag) {
            Verts.push_back(_U);
            Verts.push_back(_V);
            Verts.push_back(_Tag);
        };

        for (u32 y = 0; y < N; ++y) {
            const f32 v = y * Rcp;
            for (u32 x = 0; x < N; ++x) {
                const f32 u = x * Rcp;
                const f32 fx = std::fabs(u * 2.0f - 1.0f);
                const f32 fy = std::fabs(v * 2.0f - 1.0f);
                const f32 edge = (fx > fy) ? fx : fy; // max(fx, fy)
                PushVertex(u, v, edge);
            }
        }

        auto PatternIndex = [](u32 _Left, u32 _Right, u32 _Bottom, u32 _Top) {
            return _Left + _Right * 3u + _Bottom * 9u + _Top * 27u;
        };
        auto VertexIndex = [N](u32 _X, u32 _Y) {
            return _Y * N + _X;
        };
        auto EdgeDegree = [](u32 _LevelSize, u32 _PatternDegree) {
            return _LevelSize >> _PatternDegree;
        };

        std::vector<u32> Indices;
        Indices.reserve(static_cast<size_t>(kSubsetRangeCount) * MeshDim * MeshDim * 6);

        auto AddTri = [&Indices](u32 _A, u32 _B, u32 _C) {
            Indices.push_back(_A);
            Indices.push_back(_B);
            Indices.push_back(_C);
        };
        auto GenerateBoundary = [&](u32 _LeftDegree, u32 _TopDegree,
                                    u32 _RightDegree, u32 _BottomDegree,
                                    u32 _LevelSize, u32 _LevelStep) {
            auto LevelVertex = [&](u32 _X, u32 _Y) {
                return VertexIndex(_X * _LevelStep, _Y * _LevelStep);
            };

            if (_TopDegree < _LevelSize) {
                const u32 Step = _LevelSize / _TopDegree;
                for (u32 i = 0; i < _LevelSize; i += Step) {
                    AddTri(LevelVertex(i, 0), LevelVertex(i + Step / 2, 1), LevelVertex(i + Step, 0));

                    for (u32 j = 0; j < Step / 2; ++j) {
                        if (i == 0 && j == 0 && _LeftDegree < _LevelSize) continue;
                        AddTri(LevelVertex(i, 0), LevelVertex(i + j, 1), LevelVertex(i + j + 1, 1));
                    }
                    for (u32 j = Step / 2; j < Step; ++j) {
                        if (i == _LevelSize - Step && j == Step - 1 && _RightDegree < _LevelSize) continue;
                        AddTri(LevelVertex(i + Step, 0), LevelVertex(i + j, 1), LevelVertex(i + j + 1, 1));
                    }
                }
            }

            if (_LeftDegree < _LevelSize) {
                const u32 Step = _LevelSize / _LeftDegree;
                for (u32 i = 0; i < _LevelSize; i += Step) {
                    AddTri(LevelVertex(0, i), LevelVertex(0, i + Step), LevelVertex(1, i + Step / 2));

                    for (u32 j = 0; j < Step / 2; ++j) {
                        if (i == 0 && j == 0 && _TopDegree < _LevelSize) continue;
                        AddTri(LevelVertex(0, i), LevelVertex(1, i + j + 1), LevelVertex(1, i + j));
                    }
                    for (u32 j = Step / 2; j < Step; ++j) {
                        if (i == _LevelSize - Step && j == Step - 1 && _BottomDegree < _LevelSize) continue;
                        AddTri(LevelVertex(0, i + Step), LevelVertex(1, i + j + 1), LevelVertex(1, i + j));
                    }
                }
            }

            if (_RightDegree < _LevelSize) {
                const u32 Step = _LevelSize / _RightDegree;
                for (u32 i = 0; i < _LevelSize; i += Step) {
                    AddTri(LevelVertex(_LevelSize, i), LevelVertex(_LevelSize - 1, i + Step / 2), LevelVertex(_LevelSize, i + Step));

                    for (u32 j = 0; j < Step / 2; ++j) {
                        if (i == 0 && j == 0 && _TopDegree < _LevelSize) continue;
                        AddTri(LevelVertex(_LevelSize, i), LevelVertex(_LevelSize - 1, i + j), LevelVertex(_LevelSize - 1, i + j + 1));
                    }
                    for (u32 j = Step / 2; j < Step; ++j) {
                        if (i == _LevelSize - Step && j == Step - 1 && _BottomDegree < _LevelSize) continue;
                        AddTri(LevelVertex(_LevelSize, i + Step), LevelVertex(_LevelSize - 1, i + j), LevelVertex(_LevelSize - 1, i + j + 1));
                    }
                }
            }

            if (_BottomDegree < _LevelSize) {
                const u32 Step = _LevelSize / _BottomDegree;
                for (u32 i = 0; i < _LevelSize; i += Step) {
                    AddTri(LevelVertex(i, _LevelSize), LevelVertex(i + Step, _LevelSize), LevelVertex(i + Step / 2, _LevelSize - 1));

                    for (u32 j = 0; j < Step / 2; ++j) {
                        if (i == 0 && j == 0 && _LeftDegree < _LevelSize) continue;
                        AddTri(LevelVertex(i, _LevelSize), LevelVertex(i + j + 1, _LevelSize - 1), LevelVertex(i + j, _LevelSize - 1));
                    }
                    for (u32 j = Step / 2; j < Step; ++j) {
                        if (i == _LevelSize - Step && j == Step - 1 && _RightDegree < _LevelSize) continue;
                        AddTri(LevelVertex(i + Step, _LevelSize), LevelVertex(i + j + 1, _LevelSize - 1), LevelVertex(i + j, _LevelSize - 1));
                    }
                }
            }
        };

        for (u32 Lod = 0; Lod < kSubsetLODCount; ++Lod) {
            const u32 LevelSize = MeshDim >> Lod;
            const u32 LevelStep = 1u << Lod;

            auto LevelVertex = [&](u32 _X, u32 _Y) {
                return VertexIndex(_X * LevelStep, _Y * LevelStep);
            };

            for (u32 Top = 0; Top < 3; ++Top) {
                for (u32 Bottom = 0; Bottom < 3; ++Bottom) {
                    for (u32 Right = 0; Right < 3; ++Right) {
                        for (u32 Left = 0; Left < 3; ++Left) {
                            const u32 Pattern = PatternIndex(Left, Right, Bottom, Top);
                            const u32 RangeIndex = Lod * kSubsetPatternCount + Pattern;
                            PatternRanges[RangeIndex].Start = static_cast<u32>(Indices.size());

                            const u32 LeftDegree = EdgeDegree(LevelSize, Left);
                            const u32 RightDegree = EdgeDegree(LevelSize, Right);
                            const u32 BottomDegree = EdgeDegree(LevelSize, Bottom);
                            const u32 TopDegree = EdgeDegree(LevelSize, Top);

                            const u32 InnerLeft   = (LeftDegree   == LevelSize) ? 0u         : 1u;
                            const u32 InnerRight  = (RightDegree  == LevelSize) ? LevelSize  : LevelSize - 1u;
                            const u32 InnerBottom = (BottomDegree == LevelSize) ? LevelSize  : LevelSize - 1u;
                            const u32 InnerTop    = (TopDegree    == LevelSize) ? 0u         : 1u;

                            for (u32 y = InnerTop; y < InnerBottom; ++y) {
                                for (u32 x = InnerLeft; x < InnerRight; ++x) {
                                    const u32 i0 = LevelVertex(x, y);
                                    const u32 i1 = LevelVertex(x + 1, y);
                                    const u32 i2 = LevelVertex(x, y + 1);
                                    const u32 i3 = LevelVertex(x + 1, y + 1);
                                    AddTri(i0, i2, i1);
                                    AddTri(i1, i2, i3);
                                }
                            }

                            GenerateBoundary(LeftDegree, TopDegree, RightDegree, BottomDegree,
                                             LevelSize, LevelStep);
                            PatternRanges[RangeIndex].Count =
                                static_cast<u32>(Indices.size()) - PatternRanges[RangeIndex].Start;
                        }
                    }
                }
            }
        }

        const UINT VBSize = static_cast<UINT>(Verts.size() * sizeof(f32));
        const UINT IBSize = static_cast<UINT>(Indices.size() * sizeof(u32));

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC RDesc{};
        RDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        RDesc.Height           = 1;
        RDesc.DepthOrArraySize = 1;
        RDesc.MipLevels        = 1;
        RDesc.Format           = DXGI_FORMAT_UNKNOWN;
        RDesc.SampleDesc       = { 1, 0 };
        RDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_RANGE NoRead{ 0, 0 };
        void* Mapped = nullptr;

        RDesc.Width = VBSize;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &RDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VertexBuffer)));
        SMILE_HR(VertexBuffer->Map(0, &NoRead, &Mapped));
        std::memcpy(Mapped, Verts.data(), VBSize);
        VertexBuffer->Unmap(0, nullptr);
        VBView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VBView.StrideInBytes  = 3 * sizeof(f32);
        VBView.SizeInBytes    = VBSize;

        RDesc.Width = IBSize;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &RDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&IndexBuffer)));
        SMILE_HR(IndexBuffer->Map(0, &NoRead, &Mapped));
        std::memcpy(Mapped, Indices.data(), IBSize);
        IndexBuffer->Unmap(0, nullptr);
        IBView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IBView.Format         = DXGI_FORMAT_R32_UINT;
        IBView.SizeInBytes    = IBSize;
    }

    void FWaterRenderer::BuildInstanceBuffer(ID3D12Device* _Device) {
        // Uma regiao de kMaxTileInstances por frame in flight (double-buffered).
        const UINT FrameSize  = static_cast<UINT>(sizeof(TileInstance) * kMaxTileInstances);
        const UINT BufferSize = FrameSize * FCommandQueue::kFramesInFlight;

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = BufferSize;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&InstanceBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(InstanceBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&MappedInstancesBase)));

        // BufferLocation eh repontado por frame no UpdatePerFrame; SizeInBytes = 1 regiao.
        InstanceVBView.BufferLocation = InstanceBuffer->GetGPUVirtualAddress();
        InstanceVBView.StrideInBytes  = sizeof(TileInstance);
        InstanceVBView.SizeInBytes    = FrameSize;
    }

    void FWaterRenderer::BuildTileInstances(const Mat44& _ViewProj, const Mat44& _Projection,
                                            const Vec3& _CameraPos,
                                            u32 _ScreenW, u32 _ScreenH) {
        if (!MappedInstances) {
            InstanceCount = 0;
            return;
        }

        struct Node {
            f32 X;
            f32 Z;
            f32 Size;
            u32 Depth;
            u32 LOD = 0;
            f32 Coverage = 0.0f;
            f32 Morph = 0.0f;
        };

        InstanceCount = 0;

        const f32 BaseSize = std::max(TileBaseSize, 1.0f);
        const f32 CoverageThreshold = std::max(TileCoverageThreshold, 1.0f);
        const u32 MeshDim = kGridPoints - 1;
        const u32 Depth = std::min(TileMaxDepth, 10u);
        const u32 RootCells = 1u << Depth;
        const f32 RootSize = BaseSize * static_cast<f32>(1u << Depth);
        const f32 SnapX = std::floor(_CameraPos.X / BaseSize) * BaseSize;
        const f32 SnapZ = std::floor(_CameraPos.Z / BaseSize) * BaseSize;
        const f32 RootX = SnapX - RootSize * 0.5f;
        const f32 RootZ = SnapZ - RootSize * 0.5f;
        const f32 ScreenW = static_cast<f32>(_ScreenW ? _ScreenW : 1u);
        const f32 ScreenH = static_cast<f32>(_ScreenH ? _ScreenH : 1u);
        // Asylum TerrainQuadTree::CalculateCoverage usa proj._11 * proj._22.
        // Passar a Projection real evita depender de como o ViewProj empacota a camera.
        const f32 ProjScaleX = std::fabs(_Projection.M[0][0]);
        const f32 ProjScaleY = std::fabs(_Projection.M[1][1]);
        const f32 ScreenProjectionScale = ScreenW * ScreenH * 0.25f *
                                          std::max(ProjScaleX * ProjScaleY, 0.001f);
        const f32 BoundsPad = 12.0f;
        const f32 MinCoverageDist = 1.0f;
        const f32 CoveragePad = 1.35f;

        auto ClipAt = [&_ViewProj](f32 _X, f32 _Y, f32 _Z) {
            return Vec4{
                _X * _ViewProj.M[0][0] + _Y * _ViewProj.M[1][0] + _Z * _ViewProj.M[2][0] + _ViewProj.M[3][0],
                _X * _ViewProj.M[0][1] + _Y * _ViewProj.M[1][1] + _Z * _ViewProj.M[2][1] + _ViewProj.M[3][1],
                _X * _ViewProj.M[0][2] + _Y * _ViewProj.M[1][2] + _Z * _ViewProj.M[2][2] + _ViewProj.M[3][2],
                _X * _ViewProj.M[0][3] + _Y * _ViewProj.M[1][3] + _Z * _ViewProj.M[2][3] + _ViewProj.M[3][3],
            };
        };
        auto NodeIntersectsFrustum = [&](const Node& _N) {
            bool OutsideLeft = true, OutsideRight = true;
            bool OutsideBottom = true, OutsideTop = true;
            bool OutsideNear = true, OutsideFar = true;

            const f32 X0 = _N.X - BoundsPad;
            const f32 X1 = _N.X + _N.Size + BoundsPad;
            const f32 Z0 = _N.Z - BoundsPad;
            const f32 Z1 = _N.Z + _N.Size + BoundsPad;
            const f32 Y0 = WaterLevel - BoundsPad;
            const f32 Y1 = WaterLevel + BoundsPad;

            for (u32 xi = 0; xi < 2; ++xi) {
                for (u32 yi = 0; yi < 2; ++yi) {
                    for (u32 zi = 0; zi < 2; ++zi) {
                        const Vec4 C = ClipAt(xi ? X1 : X0, yi ? Y1 : Y0, zi ? Z1 : Z0);
                        OutsideLeft   = OutsideLeft   && (C.X < -C.W);
                        OutsideRight  = OutsideRight  && (C.X >  C.W);
                        OutsideBottom = OutsideBottom && (C.Y < -C.W);
                        OutsideTop    = OutsideTop    && (C.Y >  C.W);
                        OutsideNear   = OutsideNear   && (C.Z <  0.0f);
                        OutsideFar    = OutsideFar    && (C.Z >  C.W);
                    }
                }
            }

            return !(OutsideLeft || OutsideRight || OutsideBottom ||
                     OutsideTop || OutsideNear || OutsideFar);
        };

        auto PushChildren = [](std::vector<Node>& _Out, const Node& _N) {
            const f32 Half = _N.Size * 0.5f;
            const u32 ChildDepth = _N.Depth + 1;
            _Out.push_back({ _N.X,        _N.Z,        Half, ChildDepth });
            _Out.push_back({ _N.X + Half, _N.Z,        Half, ChildDepth });
            _Out.push_back({ _N.X,        _N.Z + Half, Half, ChildDepth });
            _Out.push_back({ _N.X + Half, _N.Z + Half, Half, ChildDepth });
        };

        auto NodeScreenCoverage = [&](const Node& _N) {
            const f32 CellSize = _N.Size / static_cast<f32>(MeshDim);
            const f32 WorldArea = CellSize * CellSize;
            f32 MaxCoverage = 0.0f;

            static constexpr f32 Samples[16][2] = {
                { 0.0f,    0.0f   }, { 0.0f,    1.0f   },
                { 1.0f,    0.0f   }, { 1.0f,    1.0f   },
                { 0.5f,    0.333f }, { 0.25f,   0.667f },
                { 0.75f,   0.111f }, { 0.125f,  0.444f },
                { 0.625f,  0.778f }, { 0.375f,  0.222f },
                { 0.875f,  0.556f }, { 0.0625f, 0.889f },
                { 0.5625f, 0.037f }, { 0.3125f, 0.37f  },
                { 0.8125f, 0.704f }, { 0.1875f, 0.148f },
            };

            for (const auto& Sample : Samples) {
                const f32 X = (_N.X - CoveragePad) + (_N.Size + 2.0f * CoveragePad) * Sample[0];
                const f32 Z = (_N.Z - CoveragePad) + (_N.Size + 2.0f * CoveragePad) * Sample[1];
                const f32 Dx = X - _CameraPos.X;
                const f32 Dy = WaterLevel - _CameraPos.Y;
                const f32 Dz = Z - _CameraPos.Z;
                const f32 DistSq = std::max(Dx * Dx + Dy * Dy + Dz * Dz,
                                            MinCoverageDist * MinCoverageDist);
                const f32 Coverage = WorldArea * ScreenProjectionScale / DistSq;
                MaxCoverage = std::max(MaxCoverage, Coverage);
            }

            return MaxCoverage;
        };
        auto CalculateNodeLOD = [&](f32 _Coverage) {
            u32 LOD = 0;
            f32 Coverage = _Coverage;
            for (; LOD + 1 < kSubsetLODCount; ++LOD) {
                if (Coverage > CoverageThreshold)
                    break;
                Coverage *= 4.0f;
            }
            return std::min(LOD, kSubsetLODCount - 1);
        };
        auto CalculateNodeMorph = [&](f32 _Coverage, u32 _LOD) {
            if (_LOD + 1 >= kSubsetLODCount)
                return 0.0f;

            f32 SwitchCoverage = CoverageThreshold;
            for (u32 I = 0; I < _LOD; ++I)
                SwitchCoverage *= 0.25f;

            const f32 MorphStartCoverage = SwitchCoverage * 1.75f;
            const f32 Denom = std::max(MorphStartCoverage - SwitchCoverage, 1.0f);
            f32 T = (MorphStartCoverage - _Coverage) / Denom;
            T = std::clamp(T, 0.0f, 1.0f);
            return T * T * (3.0f - 2.0f * T);
        };

        std::vector<Node> Stack;
        Stack.reserve(512);
        Stack.push_back({ RootX, RootZ, RootSize, 0 });

        std::vector<Node> Leaves;
        Leaves.reserve(1024);

        while (!Stack.empty()) {
            const Node N = Stack.back();
            Stack.pop_back();

            if (!NodeIntersectsFrustum(N)) {
                continue;
            }

            const f32 Coverage = NodeScreenCoverage(N);
            const bool CanSplit = N.Depth < Depth && N.Size > BaseSize * 1.01f;
            const bool ShouldSplit = CanSplit && Coverage > CoverageThreshold;
            if (ShouldSplit) {
                PushChildren(Stack, N);
                continue;
            }

            Node Leaf = N;
            Leaf.Coverage = Coverage;
            Leaf.LOD = CalculateNodeLOD(Coverage);
            Leaf.Morph = CalculateNodeMorph(Coverage, Leaf.LOD);
            Leaves.push_back(Leaf);
        }

        std::vector<i32> LeafGrid(static_cast<size_t>(RootCells) * RootCells, -1);
        auto RebuildLeafGrid = [&]() {
            std::fill(LeafGrid.begin(), LeafGrid.end(), -1);
            for (u32 LeafIndex = 0; LeafIndex < static_cast<u32>(Leaves.size()); ++LeafIndex) {
                const Node& N = Leaves[LeafIndex];
                const i32 X0 = std::max(0, static_cast<i32>(std::round((N.X - RootX) / BaseSize)));
                const i32 Z0 = std::max(0, static_cast<i32>(std::round((N.Z - RootZ) / BaseSize)));
                const i32 Count = std::max(1, static_cast<i32>(std::round(N.Size / BaseSize)));
                const i32 X1 = std::min(static_cast<i32>(RootCells), X0 + Count);
                const i32 Z1 = std::min(static_cast<i32>(RootCells), Z0 + Count);

                for (i32 Z = Z0; Z < Z1; ++Z) {
                    for (i32 X = X0; X < X1; ++X) {
                        LeafGrid[static_cast<size_t>(Z) * RootCells + X] = static_cast<i32>(LeafIndex);
                    }
                }
            }
        };

        auto LeafAtCell = [&](i32 _X, i32 _Z) -> i32 {
            if (_X < 0 || _Z < 0 || _X >= static_cast<i32>(RootCells) || _Z >= static_cast<i32>(RootCells))
                return -1;
            return LeafGrid[static_cast<size_t>(_Z) * RootCells + _X];
        };

        // Os subsets so cobrem vizinho igual, 2x e 4x mais grosso. Se a quadtree
        // gerar saltos maiores, subdividimos o lado grosso para nao sobrar T-junction.
        static constexpr f32 MaxSubsetScale = 4.0f;
        for (u32 BalancePass = 0; BalancePass < 4; ++BalancePass) {
            RebuildLeafGrid();
            std::vector<u8> SplitFlags(Leaves.size(), 0);

            auto MarkPair = [&](u32 _AIndex, i32 _BIndex) {
                if (_BIndex < 0 || static_cast<u32>(_BIndex) == _AIndex)
                    return;

                const Node& A = Leaves[_AIndex];
                const Node& B = Leaves[static_cast<size_t>(_BIndex)];
                if (A.Size > B.Size * MaxSubsetScale + 0.01f && A.Depth < Depth)
                    SplitFlags[_AIndex] = 1;
                if (B.Size > A.Size * MaxSubsetScale + 0.01f && B.Depth < Depth)
                    SplitFlags[static_cast<size_t>(_BIndex)] = 1;
            };

            for (u32 LeafIndex = 0; LeafIndex < static_cast<u32>(Leaves.size()); ++LeafIndex) {
                const Node& N = Leaves[LeafIndex];
                const i32 X0 = static_cast<i32>(std::round((N.X - RootX) / BaseSize));
                const i32 Z0 = static_cast<i32>(std::round((N.Z - RootZ) / BaseSize));
                const i32 Count = std::max(1, static_cast<i32>(std::round(N.Size / BaseSize)));

                for (i32 I = 0; I < Count; ++I) {
                    MarkPair(LeafIndex, LeafAtCell(X0 - 1,     Z0 + I));
                    MarkPair(LeafIndex, LeafAtCell(X0 + Count, Z0 + I));
                    MarkPair(LeafIndex, LeafAtCell(X0 + I,     Z0 - 1));
                    MarkPair(LeafIndex, LeafAtCell(X0 + I,     Z0 + Count));
                }
            }

            if (std::find(SplitFlags.begin(), SplitFlags.end(), 1) == SplitFlags.end())
                break;

            std::vector<Node> Balanced;
            Balanced.reserve(std::min<size_t>(Leaves.size() * 2, kMaxTileInstances));
            for (u32 LeafIndex = 0; LeafIndex < static_cast<u32>(Leaves.size()); ++LeafIndex) {
                const Node& N = Leaves[LeafIndex];
                if (SplitFlags[LeafIndex] && Balanced.size() + 4 <= kMaxTileInstances) {
                    PushChildren(Balanced, N);
                } else {
                    Balanced.push_back(N);
                }
            }

            Leaves.swap(Balanced);
        }

        for (Node& N : Leaves) {
            N.Coverage = NodeScreenCoverage(N);
            N.LOD = CalculateNodeLOD(N.Coverage);
            N.Morph = CalculateNodeMorph(N.Coverage, N.LOD);
        }

        for (u32 LODBalancePass = 0; LODBalancePass < 4; ++LODBalancePass) {
            RebuildLeafGrid();
            bool Changed = false;

            auto EffectiveCellSize = [&](const Node& _N) {
                const u32 LevelSize = std::max(1u, MeshDim >> _N.LOD);
                return _N.Size / static_cast<f32>(LevelSize);
            };
            auto BalanceLODPair = [&](u32 _AIndex, i32 _BIndex) {
                if (_BIndex < 0 || static_cast<u32>(_BIndex) == _AIndex)
                    return;

                Node& A = Leaves[_AIndex];
                Node& B = Leaves[static_cast<size_t>(_BIndex)];
                const f32 ACell = EffectiveCellSize(A);
                const f32 BCell = EffectiveCellSize(B);

                const bool AIsSmaller = A.Size < B.Size * 0.999f;
                const bool BIsSmaller = B.Size < A.Size * 0.999f;
                const f32 AAllowedScale = AIsSmaller ? 1.0f : MaxSubsetScale;
                const f32 BAllowedScale = BIsSmaller ? 1.0f : MaxSubsetScale;

                if (ACell > BCell * AAllowedScale + 0.001f && A.LOD > 0) {
                    --A.LOD;
                    Changed = true;
                }
                if (BCell > ACell * BAllowedScale + 0.001f && B.LOD > 0) {
                    --B.LOD;
                    Changed = true;
                }
            };

            for (u32 LeafIndex = 0; LeafIndex < static_cast<u32>(Leaves.size()); ++LeafIndex) {
                const Node& N = Leaves[LeafIndex];
                const i32 X0 = static_cast<i32>(std::round((N.X - RootX) / BaseSize));
                const i32 Z0 = static_cast<i32>(std::round((N.Z - RootZ) / BaseSize));
                const i32 Count = std::max(1, static_cast<i32>(std::round(N.Size / BaseSize)));

                for (i32 I = 0; I < Count; ++I) {
                    BalanceLODPair(LeafIndex, LeafAtCell(X0 - 1,     Z0 + I));
                    BalanceLODPair(LeafIndex, LeafAtCell(X0 + Count, Z0 + I));
                    BalanceLODPair(LeafIndex, LeafAtCell(X0 + I,     Z0 - 1));
                    BalanceLODPair(LeafIndex, LeafAtCell(X0 + I,     Z0 + Count));
                }
            }

            if (!Changed)
                break;
        }

        for (Node& N : Leaves) {
            N.Morph = CalculateNodeMorph(N.Coverage, N.LOD);
        }

        RebuildLeafGrid();
        auto EdgePattern = [&](const Node& _N, u32 _Edge) {
            const i32 X0 = static_cast<i32>(std::round((_N.X - RootX) / BaseSize));
            const i32 Z0 = static_cast<i32>(std::round((_N.Z - RootZ) / BaseSize));
            const i32 Count = std::max(1, static_cast<i32>(std::round(_N.Size / BaseSize)));
            i32 NX = X0 + Count / 2;
            i32 NZ = Z0 + Count / 2;

            if (_Edge == 0) { NX = X0 - 1;         NZ = Z0 + Count / 2; }
            if (_Edge == 1) { NX = X0 + Count;     NZ = Z0 + Count / 2; }
            if (_Edge == 2) { NX = X0 + Count / 2; NZ = Z0 + Count; } // bottom: +Z
            if (_Edge == 3) { NX = X0 + Count / 2; NZ = Z0 - 1; }     // top: -Z

            const i32 Neighbor = LeafAtCell(NX, NZ);
            if (Neighbor < 0) return 0u;

            const Node& Adj = Leaves[static_cast<size_t>(Neighbor)];
            if (Adj.Size <= _N.Size * 0.999f)
                return 0u;

            const f32 NodeLevelSize = static_cast<f32>(MeshDim >> _N.LOD);
            const f32 AdjLevelSize = static_cast<f32>(MeshDim >> Adj.LOD);
            const f32 Scale = Adj.Size / std::max(_N.Size, 1.0f) *
                              NodeLevelSize / std::max(AdjLevelSize, 1.0f);
            if (Scale > 3.999f) return 2u;
            if (Scale > 1.999f) return 1u;
            return 0u;
        };
        auto PatternIndex = [](u32 _Left, u32 _Right, u32 _Bottom, u32 _Top) {
            return _Left + _Right * 3u + _Bottom * 9u + _Top * 27u;
        };

        for (u32 RangeIndex = 0; RangeIndex < kSubsetRangeCount; ++RangeIndex) {
            PatternInstanceStarts[RangeIndex] = 0;
            PatternInstanceCounts[RangeIndex] = 0;
        }

        struct PendingInstance {
            TileInstance Instance;
            u32 RangeIndex = 0;
        };

        std::vector<PendingInstance> Pending;
        Pending.reserve(std::min<size_t>(Leaves.size(), kMaxTileInstances));
        for (const Node& N : Leaves) {
            if (Pending.size() >= kMaxTileInstances) break;

            const u32 Left   = EdgePattern(N, 0);
            const u32 Right  = EdgePattern(N, 1);
            const u32 Bottom = EdgePattern(N, 2);
            const u32 Top    = EdgePattern(N, 3);
            const u32 Pattern = PatternIndex(Left, Right, Bottom, Top);
            const u32 LOD = std::min(N.LOD, kSubsetLODCount - 1);
            const u32 RangeIndex = LOD * kSubsetPatternCount + Pattern;

            Pending.push_back({
                { { N.X, N.Z, N.Size, static_cast<f32>(RangeIndex) },
                  { N.Morph, N.Coverage, static_cast<f32>(LOD), 0.0f } },
                RangeIndex
            });
            ++PatternInstanceCounts[RangeIndex];
        }

        u32 Start = 0;
        for (u32 RangeIndex = 0; RangeIndex < kSubsetRangeCount; ++RangeIndex) {
            PatternInstanceStarts[RangeIndex] = Start;
            Start += PatternInstanceCounts[RangeIndex];
            PatternInstanceCounts[RangeIndex] = 0;
        }

        for (const PendingInstance& PendingInst : Pending) {
            const u32 RangeIndex = PendingInst.RangeIndex;
            const u32 Dst = PatternInstanceStarts[RangeIndex] + PatternInstanceCounts[RangeIndex]++;
            MappedInstances[Dst] = PendingInst.Instance;
        }
        InstanceCount = static_cast<u32>(Pending.size());
    }

    void FWaterRenderer::Initialize(ID3D12Device* _Device, u32 _SampleCount,
                                    DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildRootSignature(_Device);
        BuildPSO(_Device, _SampleCount, _RTFormat, _DSFormat);
        BuildGrid(_Device);
        BuildInstanceBuffer(_Device);

        // Upload heap CBV (mapeado persistente — 1 frame in-flight torna seguro reescrever).
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(WaterConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CBV)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CBV->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCBVBase)));
    }

    void FWaterRenderer::Recreate(ID3D12Device* _Device, u32 _SampleCount,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildPSO(_Device, _SampleCount, _RTFormat, _DSFormat);
    }

    void FWaterRenderer::Resize(ID3D12Device*, u32, u32) {
        // No-op ate a Etapa 3 (snapshots de scene-color/depth para refracao).
    }

    void FWaterRenderer::UpdatePerFrame(u32 _FrameSlot, const Mat44& _ViewProj, const Mat44& _Projection,
                                        const Mat44& _InvViewProj,
                                        const Vec3& _CameraPos, const Vec3& _SunDir, f32 _SunIntensity,
                                        const Vec3& _SunColor, f32 _ElapsedTime,
                                        bool _IBLEnabled, f32 _IBLIntensity,
                                        u32 _ScreenW, u32 _ScreenH, f32 _NearZ, f32 _FarZ,
                                        bool _HasSceneCopies, bool _UseAtmosphereSky) {
        if (!MappedCBVBase || !MappedInstancesBase) return;

        // Reaponta CB + instancias + view de instancia para a regiao deste frame.
        FrameSlot = _FrameSlot;
        MappedCBV = reinterpret_cast<WaterConstants*>(
            MappedCBVBase + static_cast<size_t>(FrameSlot) * sizeof(WaterConstants));
        MappedInstances = reinterpret_cast<TileInstance*>(
            MappedInstancesBase + static_cast<size_t>(FrameSlot) * sizeof(TileInstance) * kMaxTileInstances);
        InstanceVBView.BufferLocation = InstanceBuffer->GetGPUVirtualAddress() +
            static_cast<UINT64>(FrameSlot) * sizeof(TileInstance) * kMaxTileInstances;

        const f32 Cos = std::cos(WindDir);
        const f32 Sin = std::sin(WindDir);

        MappedCBV->ViewProj     = _ViewProj;
        MappedCBV->InvViewProj  = _InvViewProj;
        MappedCBV->CameraPos    = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        MappedCBV->SunDirection = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        MappedCBV->SunColor     = { _SunColor.X, _SunColor.Y, _SunColor.Z, 0.0f };
        // OceanParams0/1 — semantica da Cry (terrain_water_quad.cpp:491).
        MappedCBV->OceanParams0    = { WindDir, WindSpeed, 0.0f, WavesAmount };
        MappedCBV->OceanParams1    = { WavesSize, Cos, Sin, WaterLevel };
        MappedCBV->DeepColorDensity = { DeepColor.X, DeepColor.Y, DeepColor.Z, 0.0f };
        // Misc: time, gate de reflexao IBL, intensidade, mip max do cubemap especular (6).
        MappedCBV->Misc = { _ElapsedTime, _IBLEnabled ? 1.0f : 0.0f, _IBLIntensity, 6.0f };
        // ProjGrid: maxDist/horizonBias ainda usados pelo fade final; y/w ficam legados.
        MappedCBV->ProjGrid     = { 8000.0f, 1.3f, 4.5f, 1.0f / 20000.0f };
        // ShadeParams: FresnelGloss, ReflectionScale, expoente do sun spec, escala do sun spec.
        MappedCBV->ShadeParams  = { FresnelGloss, ReflectionScale, 256.0f, 1.0f };
        // OceanFFT: usa FFT?, escala mestre do deslocamento, escala choppy, "up" do normal.
        MappedCBV->OceanFFT     = { UseFFT ? 1.0f : 0.0f, FFTDispScale, FFTChoppyScale, FFTNormalUp };
        // OceanFade: distancia de inicio e faixa do achatamento (anti-shimmer no horizonte).
        MappedCBV->OceanFade    = { FFTFadeStart, FFTFadeRange, 0.0f, 0.0f };
        // Bump de detalhe (Etapa 2): tilings + escalas de normal; strength/parallax/toggle.
        MappedCBV->BumpParams   = { BumpTilling, BumpDetailTilling, BumpNormalsScale, BumpDetailScale };
        MappedCBV->BumpParams2  = { BumpStrength, ParallaxHeight, UseBump ? 1.0f : 0.0f, BumpFadeDist };
        // Refração / fog (Etapa 3).
        const f32 W = (f32)(_ScreenW ? _ScreenW : 1), H = (f32)(_ScreenH ? _ScreenH : 1);
        MappedCBV->ScreenParams     = { W, H, 1.0f / W, 1.0f / H };
        MappedCBV->DepthParams      = { _NearZ, _FarZ, _HasSceneCopies ? 1.0f : 0.0f, 0.0f };
        MappedCBV->RefractionParams = { RefractionBumpScale, SoftIntersectionFactor, FogDensity, ReflectionBumpScale };
        MappedCBV->AerialFogParams  = { AerialFogStart, AerialFogRange, AerialFogDensity,
                                         UseAerialFog ? (_UseAtmosphereSky ? 2.0f : 1.0f) : 0.0f };
        MappedCBV->FarBlendParams   = { FarBlendStart, FarBlendRange,
                                         FarSwellHeight, FarSwellNormalStrength };
        MappedCBV->DebugParams      = { static_cast<f32>(static_cast<u32>(DebugMode)), 0.0f, 0.0f, 0.0f };
        // densidade do fog tambem em DeepColorDensity.w (Cry empacota la).
        MappedCBV->DeepColorDensity = { DeepColor.X, DeepColor.Y, DeepColor.Z, FogDensity };
        // In-scattering (turquesa) + absorcao por canal + clamp do sun-spec (Etapa 3).
        MappedCBV->InScatterColor  = { InScatterColor.X, InScatterColor.Y, InScatterColor.Z, InScatterDensity };
        MappedCBV->AbsorptionColor = { AbsorptionColor.X, AbsorptionColor.Y, AbsorptionColor.Z, SunSpecClamp };
        // Foam: coverage(limiar J), sharpness, intensidade (0 desliga), distancia de fade.
        MappedCBV->FoamParams = { FoamCoverage, FoamSharpness,
                                  UseFoam ? FoamIntensity : 0.0f, FoamFadeDist };
        MappedCBV->FoamColor  = { FoamColor.X, FoamColor.Y, FoamColor.Z, FoamSpecSuppress };

        BuildTileInstances(_ViewProj, _Projection, _CameraPos, _ScreenW, _ScreenH);
    }

    void FWaterRenderer::RenderSurface(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                                       u32 _SpecularCubeSRVSlot, u32 _FFTDisplacementSRVSlot,
                                       u32 _FFTNormalSRVSlot, u32 _SceneCopyTableStart,
                                       u32 _AtmosphereSkyViewSRVSlot) {
        if (!PSO || InstanceCount == 0) return;

        _CommandList->SetGraphicsRootSignature(RootSignature.Get());
        ID3D12PipelineState* ActivePSO =
            (DebugMode == EDebugMode::Wireframe && WireframePSO) ? WireframePSO.Get() : PSO.Get();
        _CommandList->SetPipelineState(ActivePSO);
        _CommandList->SetGraphicsRootConstantBufferView(
            0, CBV->GetGPUVirtualAddress() + static_cast<UINT64>(FrameSlot) * sizeof(WaterConstants));
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_SpecularCubeSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_FFTDisplacementSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_SceneCopyTableStart));
        _CommandList->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(_FFTNormalSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(5, _SRVHeap.GpuHandle(_AtmosphereSkyViewSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW Views[] = { VBView, InstanceVBView };
        _CommandList->IASetVertexBuffers(0, _countof(Views), Views);
        _CommandList->IASetIndexBuffer(&IBView);
        for (u32 RangeIndex = 0; RangeIndex < kSubsetRangeCount; ++RangeIndex) {
            const u32 Count = PatternInstanceCounts[RangeIndex];
            const IndexRange& Range = PatternRanges[RangeIndex];
            if (Count == 0 || Range.Count == 0) continue;
            _CommandList->DrawIndexedInstanced(Range.Count, Count, Range.Start, 0,
                                               PatternInstanceStarts[RangeIndex]);
        }
    }
}
