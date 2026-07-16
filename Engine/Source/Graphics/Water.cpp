#include "Smile/Graphics/Water.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <cmath>
#include <stdexcept>

namespace Smile {
    namespace {
        u32 FloatBits(f32 _Value) {
            u32 Result = 0;
            static_assert(sizeof(Result) == sizeof(_Value));
            std::memcpy(&Result, &_Value, sizeof(Result));
            return Result;
        }
    }

    void FWaterRenderer::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SpecRange{};
        SpecRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SpecRange.NumDescriptors                    = 1;
        SpecRange.BaseShaderRegister                = 0; 
        SpecRange.RegisterSpace                     = 0;
        SpecRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE FFTRange{};
        FFTRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        FFTRange.NumDescriptors                    = 1;
        FFTRange.BaseShaderRegister                = 1; 
        FFTRange.RegisterSpace                     = 0;
        FFTRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE FFTNormalRange{};
        FFTNormalRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        FFTNormalRange.NumDescriptors                    = 1;
        FFTNormalRange.BaseShaderRegister                = 4; 
        FFTNormalRange.RegisterSpace                     = 0;
        FFTNormalRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE AtmoSkyViewRange{};
        AtmoSkyViewRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        AtmoSkyViewRange.NumDescriptors                    = 1;
        AtmoSkyViewRange.BaseShaderRegister                = 5; 
        AtmoSkyViewRange.RegisterSpace                     = 0;
        AtmoSkyViewRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE SceneRange{};
        SceneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SceneRange.NumDescriptors                    = 2;
        SceneRange.BaseShaderRegister                = 2; 
        SceneRange.RegisterSpace                     = 0;
        SceneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE SunShadowRange{};
        SunShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SunShadowRange.NumDescriptors                    = 1;
        SunShadowRange.BaseShaderRegister                = 11; // t11 = array de cascatas do CSM
        SunShadowRange.RegisterSpace                     = 0;
        SunShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[8]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; 
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SpecRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &FFTRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL; 

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

        RootParams[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[6].Descriptor.ShaderRegister = 3; // b3 = CSMCB
        RootParams[6].Descriptor.RegisterSpace  = 0;
        RootParams[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[7].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[7].DescriptorTable.pDescriptorRanges   = &SunShadowRange;
        RootParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[4]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0; 
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; 

        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].ShaderRegister   = 1; 
        Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        Samplers[2]                  = Samplers[0];
        Samplers[2].Filter           = D3D12_FILTER_ANISOTROPIC;
        Samplers[2].MaxAnisotropy    = 16;
        Samplers[2].ShaderRegister   = 3; // s2 e o sampler de comparacao do CSM
        Samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        Samplers[3]                  = Samplers[1];
        Samplers[3].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        Samplers[3].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Samplers[3].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        Samplers[3].ShaderRegister   = 2; // ShadowCmp do CSMCommon
        Samplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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

    void FWaterRenderer::BuildPSO(ID3D12Device* _Device,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat,
                                  DXGI_FORMAT _VelocityFormat) {
        auto VS = LoadShaderBytecode("WaterSurface.vs_6_0.cso");
        auto PS = LoadShaderBytecode("WaterSurface.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 1, 0,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 1, sizeof(u32),
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 1, sizeof(u32) * 2,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; 

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; 
        Depth.DepthFunc      = kDepthFuncLess; 
        Depth.StencilEnable  = FALSE;

        auto CreateWaterPSO = [&](D3D12_FILL_MODE _FillMode,
                                  Microsoft::WRL::ComPtr<ID3D12PipelineState>& _Out) {
            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode              = _FillMode;
            Raster.CullMode              = D3D12_CULL_MODE_NONE; 
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
            Desc.NumRenderTargets      = 2;
            Desc.RTVFormats[0]         = _RTFormat;
            Desc.RTVFormats[1]         = _VelocityFormat; // velocity da agua (curUV - prevUV)
            Desc.DSVFormat             = _DSFormat;
            Desc.SampleDesc            = { 1, 0 };

            SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&_Out)));
        };

        CreateWaterPSO(D3D12_FILL_MODE_SOLID, PSO);
        CreateWaterPSO(D3D12_FILL_MODE_WIREFRAME, WireframePSO);
    }

    void FWaterRenderer::BuildGenerateDrawsPipeline(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRanges[1]{};
        SRVRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRanges[0].NumDescriptors                    = 2; 
        SRVRanges[0].BaseShaderRegister                = 0;
        SRVRanges[0].RegisterSpace                     = 0;
        SRVRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE UAVRanges[1]{};
        UAVRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRanges[0].NumDescriptors                    = 5; 
        UAVRanges[0].BaseShaderRegister                = 0;
        UAVRanges[0].RegisterSpace                     = 0;
        UAVRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[0].Constants.ShaderRegister = 0;
        RootParams[0].Constants.RegisterSpace  = 0;
        RootParams[0].Constants.Num32BitValues = 40;
        RootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = SRVRanges;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = UAVRanges;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC RootSigDesc{};
        RootSigDesc.NumParameters = _countof(RootParams);
        RootSigDesc.pParameters   = RootParams;
        RootSigDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Water generate draws root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }

        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&GenerateDrawsRootSignature)));

        auto CS = LoadShaderBytecode("WaterGenerateDraws.cs_6_0.cso");
        D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature = GenerateDrawsRootSignature.Get();
        Desc.CS             = { CS.data(), CS.size() };
        SMILE_HR(_Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&GenerateDrawsPSO)));
    }

    void FWaterRenderer::BuildGrid(ID3D12Device* _Device) {
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
                const f32 edge = (fx > fy) ? fx : fy; 
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
        const UINT TileSourceFrameSize =
            static_cast<UINT>(sizeof(TileSource) * kMaxTileInstances);
        const UINT TileSourceBufferSize = TileSourceFrameSize * FCommandQueue::kFramesInFlight;
        const UINT DrawBucketFrameSize =
            static_cast<UINT>(sizeof(DrawBucketSource) * kSubsetRangeCount);
        const UINT DrawBucketBufferSize = DrawBucketFrameSize * FCommandQueue::kFramesInFlight;
        const UINT GpuInstanceFrameSize =
            static_cast<UINT>(sizeof(TileInstance) * kMaxTileInstances);
        const UINT GpuInstanceBufferSize = GpuInstanceFrameSize * FCommandQueue::kFramesInFlight;
        const UINT GpuIndirectFrameSize =
            static_cast<UINT>(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) * kSubsetRangeCount);
        const UINT GpuIndirectBufferSize = GpuIndirectFrameSize * FCommandQueue::kFramesInFlight;
        const UINT GpuDrawBucketScratchFrameSize =
            static_cast<UINT>(sizeof(u32) * kSubsetRangeCount * 3u);
        const UINT GpuDrawBucketScratchBufferSize =
            GpuDrawBucketScratchFrameSize * FCommandQueue::kFramesInFlight;
        const UINT GpuDebugCounterFrameSize =
            static_cast<UINT>(sizeof(u32) * kGpuDebugCounterCount);
        const UINT GpuDebugCounterBufferSize =
            GpuDebugCounterFrameSize * FCommandQueue::kFramesInFlight;

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = DrawBucketBufferSize;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&DrawBucketSourceBuffer)));
        SMILE_HR(DrawBucketSourceBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&MappedDrawBucketsBase)));

        D3D12_INDIRECT_ARGUMENT_DESC DrawArg{};
        DrawArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC SignatureDesc{};
        SignatureDesc.ByteStride       = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        SignatureDesc.NumArgumentDescs = 1;
        SignatureDesc.pArgumentDescs   = &DrawArg;
        SMILE_HR(_Device->CreateCommandSignature(
            &SignatureDesc, nullptr, IID_PPV_ARGS(&IndirectCommandSignature)));

        D3D12_HEAP_PROPERTIES DefaultHeap{};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC DefaultDesc = Desc;
        DefaultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        DefaultDesc.Width = GpuInstanceBufferSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &DefaultDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&GpuInstanceBuffer)));
        VramTracker::Register(GpuInstanceBuffer.Get(), EVramCategory::Water);
        GpuInstanceBufferState = D3D12_RESOURCE_STATE_COMMON;
        GpuInstanceVBView.BufferLocation = GpuInstanceBuffer->GetGPUVirtualAddress();
        GpuInstanceVBView.StrideInBytes  = sizeof(TileInstance);
        GpuInstanceVBView.SizeInBytes    = GpuInstanceFrameSize;

        DefaultDesc.Width = GpuIndirectBufferSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &DefaultDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&GpuIndirectArgsBuffer)));
        VramTracker::Register(GpuIndirectArgsBuffer.Get(), EVramCategory::Water);
        GpuIndirectArgsBufferState = D3D12_RESOURCE_STATE_COMMON;

        DefaultDesc.Width = TileSourceBufferSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &DefaultDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&GpuTileSourceBuffer)));
        VramTracker::Register(GpuTileSourceBuffer.Get(), EVramCategory::Water);
        GpuTileSourceBufferState = D3D12_RESOURCE_STATE_COMMON;

        DefaultDesc.Width = GpuDrawBucketScratchBufferSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &DefaultDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&GpuDrawBucketScratchBuffer)));
        VramTracker::Register(GpuDrawBucketScratchBuffer.Get(), EVramCategory::Water);
        GpuDrawBucketScratchState = D3D12_RESOURCE_STATE_COMMON;

        DefaultDesc.Width = GpuDebugCounterBufferSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &DefaultDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&GpuDebugCounterBuffer)));
        VramTracker::Register(GpuDebugCounterBuffer.Get(), EVramCategory::Water);
        GpuDebugCounterState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_HEAP_PROPERTIES ReadbackHeap{};
        ReadbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC ReadbackDesc = Desc;
        ReadbackDesc.Width = GpuDebugCounterBufferSize;
        ReadbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        SMILE_HR(_Device->CreateCommittedResource(
            &ReadbackHeap, D3D12_HEAP_FLAG_NONE, &ReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&GpuDebugReadbackBuffer)));
        D3D12_RANGE DebugReadRange{ 0, GpuDebugCounterBufferSize };
        SMILE_HR(GpuDebugReadbackBuffer->Map(
            0, &DebugReadRange, reinterpret_cast<void**>(&MappedGpuDebugCountersBase)));

        GenerateDrawsHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7, true);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_UNKNOWN;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Buffer.FirstElement     = 0;
        SRVDesc.Buffer.NumElements      = kMaxTileInstances * FCommandQueue::kFramesInFlight;
        SRVDesc.Buffer.StructureByteStride = sizeof(TileSource);
        _Device->CreateShaderResourceView(GpuTileSourceBuffer.Get(), &SRVDesc,
                                          GenerateDrawsHeap.CpuHandle(0));

        SRVDesc.Buffer.NumElements         = kSubsetRangeCount * FCommandQueue::kFramesInFlight;
        SRVDesc.Buffer.StructureByteStride = sizeof(DrawBucketSource);
        _Device->CreateShaderResourceView(DrawBucketSourceBuffer.Get(), &SRVDesc,
                                          GenerateDrawsHeap.CpuHandle(1));

        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
        UAVDesc.Format                    = DXGI_FORMAT_UNKNOWN;
        UAVDesc.ViewDimension             = D3D12_UAV_DIMENSION_BUFFER;
        UAVDesc.Buffer.FirstElement       = 0;
        UAVDesc.Buffer.NumElements        = kMaxTileInstances * FCommandQueue::kFramesInFlight;
        UAVDesc.Buffer.StructureByteStride = sizeof(TileInstance);
        _Device->CreateUnorderedAccessView(GpuInstanceBuffer.Get(), nullptr, &UAVDesc,
                                           GenerateDrawsHeap.CpuHandle(2));

        UAVDesc.Buffer.NumElements         = kSubsetRangeCount * FCommandQueue::kFramesInFlight;
        UAVDesc.Buffer.StructureByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        _Device->CreateUnorderedAccessView(GpuIndirectArgsBuffer.Get(), nullptr, &UAVDesc,
                                           GenerateDrawsHeap.CpuHandle(3));

        UAVDesc.Buffer.NumElements         = kSubsetRangeCount * 3u * FCommandQueue::kFramesInFlight;
        UAVDesc.Buffer.StructureByteStride = sizeof(u32);
        _Device->CreateUnorderedAccessView(GpuDrawBucketScratchBuffer.Get(), nullptr, &UAVDesc,
                                           GenerateDrawsHeap.CpuHandle(4));

        UAVDesc.Buffer.NumElements         = kMaxTileInstances * FCommandQueue::kFramesInFlight;
        UAVDesc.Buffer.StructureByteStride = sizeof(TileSource);
        _Device->CreateUnorderedAccessView(GpuTileSourceBuffer.Get(), nullptr, &UAVDesc,
                                           GenerateDrawsHeap.CpuHandle(5));

        UAVDesc.Buffer.NumElements         = kGpuDebugCounterCount * FCommandQueue::kFramesInFlight;
        UAVDesc.Buffer.StructureByteStride = sizeof(u32);
        _Device->CreateUnorderedAccessView(GpuDebugCounterBuffer.Get(), nullptr, &UAVDesc,
                                           GenerateDrawsHeap.CpuHandle(6));
    }

    void FWaterRenderer::PrepareGpuTileSources(const Mat44& _ViewProj, const Vec3& _CameraPos) {
        DebugStats = {};
        DebugStats.GpuDrawPath = true;
        DebugStats.GpuTileBuild = true;
        DebugStats.BucketCount = kSubsetRangeCount;
        InstanceCount = 0;
        GpuTileSourceCandidateCount = 0;

        const f32 BaseSize = std::max(TileBaseSize, 1.0f);
        const u32 Depth = std::min(TileMaxDepth, 10u);
        const u32 RootCells = 1u << Depth;
        const f32 RootSize = BaseSize * static_cast<f32>(RootCells);
        const f32 SnapX = std::floor(_CameraPos.X / BaseSize) * BaseSize;
        const f32 SnapZ = std::floor(_CameraPos.Z / BaseSize) * BaseSize;
        const f32 RootX = SnapX - RootSize * 0.5f;
        const f32 RootZ = SnapZ - RootSize * 0.5f;

        if (MappedCBV) {
            MappedCBV->QuadTreeParams = { RootX, RootZ, BaseSize, RootSize };
        }
        GpuTileBuildRootX = RootX;
        GpuTileBuildRootZ = RootZ;
        GpuTileBuildBaseSize = BaseSize;
        GpuTileBuildBoundsPad = std::max(BaseSize * 2.0f, FFTDispScale * 4.0f + 16.0f);
        GpuTileBuildViewProj = _ViewProj;

        const i32 CameraCellX = std::clamp(
            static_cast<i32>(std::floor((_CameraPos.X - RootX) / BaseSize)), 0,
            static_cast<i32>(RootCells) - 1);
        const i32 CameraCellZ = std::clamp(
            static_cast<i32>(std::floor((_CameraPos.Z - RootZ) / BaseSize)), 0,
            static_cast<i32>(RootCells) - 1);

        u32 RingRadius = GpuTileBuildRequestedRingRadius;
        u32 CellsPerSide = RingRadius * 2u + 1u;
        u32 LevelCount = 1u;
        for (u32 CoverCells = CellsPerSide; LevelCount <= Depth && CoverCells < RootCells; CoverCells <<= 1u) {
            ++LevelCount;
        }

        while (CellsPerSide * CellsPerSide * LevelCount > kMaxTileInstances && RingRadius > 4u) {
            --RingRadius;
            CellsPerSide = RingRadius * 2u + 1u;
            LevelCount = 1u;
            for (u32 CoverCells = CellsPerSide; LevelCount <= Depth && CoverCells < RootCells; CoverCells <<= 1u) {
                ++LevelCount;
            }
        }

        GpuTileBuildDepth = Depth;
        GpuTileBuildLevelCount = LevelCount;
        GpuTileBuildRingRadius = RingRadius;
        GpuTileBuildCameraCellX = static_cast<u32>(CameraCellX);
        GpuTileBuildCameraCellZ = static_cast<u32>(CameraCellZ);
        GpuTileSourceCandidateCount = std::min(kMaxTileInstances, CellsPerSide * CellsPerSide * LevelCount);
        InstanceCount = GpuTileSourceCandidateCount;
        DebugStats.CandidateCount = GpuTileSourceCandidateCount;
        DebugStats.LevelCount = GpuTileBuildLevelCount;
        DebugStats.RingRadius = GpuTileBuildRingRadius;
        DebugStats.Depth = GpuTileBuildDepth;
        DebugStats.RootX = RootX;
        DebugStats.RootZ = RootZ;
        DebugStats.BaseSize = BaseSize;
        DebugStats.RootSize = RootSize;

        for (u32 RangeIndex = 0; RangeIndex < kSubsetRangeCount; ++RangeIndex) {
            if (MappedDrawBuckets) {
                const IndexRange& Range = PatternRanges[RangeIndex];
                MappedDrawBuckets[RangeIndex] = {
                    Range.Start,
                    Range.Count,
                    0u,
                    0u
                };
            }
        }
        ReadGpuDebugCounters(FrameSlot);
    }

    void FWaterRenderer::ReadGpuDebugCounters(u32 _FrameSlot) {
        if (!MappedGpuDebugCountersBase) {
            return;
        }

        const u32* Counters = MappedGpuDebugCountersBase +
            static_cast<size_t>(_FrameSlot) * kGpuDebugCounterCount;
        const u32 ReadbackTileCount = Counters[GpuCounterTileCount];

        DebugStats.ValidTileCount = Counters[GpuCounterValidTiles];
        DebugStats.CountedTileCount = Counters[GpuCounterCountedTiles];
        DebugStats.ScatteredTileCount = Counters[GpuCounterScatteredTiles];
        DebugStats.DrawCommandCount = Counters[GpuCounterDrawCommands];
        DebugStats.OutOfBoundsCount = Counters[GpuCounterOutOfBounds];
        DebugStats.CoveredByFinerCount = Counters[GpuCounterCoveredByFiner];
        DebugStats.FrustumCulledCount = Counters[GpuCounterFrustumCulled];

        if (ReadbackTileCount > 0) {
            DebugStats.CandidateCount = ReadbackTileCount;
            DebugStats.LevelCount = Counters[GpuCounterLevelCount];
            DebugStats.RingRadius = Counters[GpuCounterRingRadius];
            DebugStats.Depth = Counters[GpuCounterDepth];
        }
    }

    void FWaterRenderer::Initialize(ID3D12Device* _Device,
                                    DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat,
                                    DXGI_FORMAT _VelocityFormat) {
        BuildRootSignature(_Device);
        BuildPSO(_Device, _RTFormat, _DSFormat, _VelocityFormat);
        BuildGenerateDrawsPipeline(_Device);
        BuildGrid(_Device);
        BuildInstanceBuffer(_Device);

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

    void FWaterRenderer::Recreate(ID3D12Device* _Device,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat,
                                  DXGI_FORMAT _VelocityFormat) {
        BuildPSO(_Device, _RTFormat, _DSFormat, _VelocityFormat);
    }

    void FWaterRenderer::Resize(ID3D12Device*, u32, u32) {
    }

    void FWaterRenderer::UpdatePerFrame(u32 _FrameSlot, const Mat44& _ViewProj, const Mat44& _Projection,
                                        const Mat44& _InvViewProj,
                                        const Mat44& _ViewProjNoJitter, const Mat44& _PrevViewProjNoJitter,
                                        const Vec3& _CameraPos, const Vec3& _SunDir, f32 _SunIntensity,
                                        const Vec3& _SunColor, f32 _ElapsedTime,
                                        bool _IBLEnabled, f32 _IBLIntensity,
                                        u32 _ScreenW, u32 _ScreenH, f32 _NearZ, f32 _FarZ,
                                        bool _HasSceneCopies, bool _UseAtmosphereSky) {
        (void)_Projection;
        if (!MappedCBVBase || !MappedDrawBucketsBase) return;

        FrameSlot = _FrameSlot;
        MappedCBV = reinterpret_cast<WaterConstants*>(
            MappedCBVBase + static_cast<size_t>(FrameSlot) * sizeof(WaterConstants));
        MappedDrawBuckets = MappedDrawBucketsBase
            ? reinterpret_cast<DrawBucketSource*>(
                MappedDrawBucketsBase + static_cast<size_t>(FrameSlot) *
                sizeof(DrawBucketSource) * kSubsetRangeCount)
            : nullptr;
        GpuInstanceVBView.BufferLocation = GpuInstanceBuffer
            ? GpuInstanceBuffer->GetGPUVirtualAddress() +
                static_cast<UINT64>(FrameSlot) * sizeof(TileInstance) * kMaxTileInstances
            : 0;

        const f32 Cos = std::cos(WindDir);
        const f32 Sin = std::sin(WindDir);

        MappedCBV->ViewProj         = _ViewProj;
        MappedCBV->InvViewProj      = _InvViewProj;
        MappedCBV->ViewProjNoJitter = _ViewProjNoJitter;
        MappedCBV->PrevViewProj     = _PrevViewProjNoJitter;
        MappedCBV->CameraPos    = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        MappedCBV->SunDirection = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        MappedCBV->SunColor     = { _SunColor.X, _SunColor.Y, _SunColor.Z, WaterClarity };
        MappedCBV->OceanParams0    = { WindDir, WindSpeed, UseFoam ? ShoreFoamIntensity : 0.0f, WavesAmount };
        MappedCBV->OceanParams1    = { WavesSize, Cos, Sin, WaterLevel };
        MappedCBV->Misc = { _ElapsedTime, _IBLEnabled ? 1.0f : 0.0f, _IBLIntensity, kSpecularMaxMip };
        MappedCBV->WaterFXParams = { SSSStrength, SSSPower, SSSHeightScale, ShoreFoamWidth };
        MappedCBV->ShadeParams  = { FresnelGloss, ReflectionScale, kSunShininess, kSunSpecScale };
        MappedCBV->OceanFFT     = { UseFFT ? 1.0f : 0.0f, FFTDispScale, FFTChoppyScale, FFTNormalUp };
        MappedCBV->OceanFade    = { FFTFadeStart, FFTFadeRange, 0.0f, 0.0f };
        MappedCBV->BumpParams   = { BumpTilling, BumpDetailTilling, BumpNormalsScale, BumpDetailScale };
        MappedCBV->BumpParams2  = { BumpStrength, ParallaxHeight, UseBump ? 1.0f : 0.0f, BumpFadeDist };

        const f32 W = (f32)(_ScreenW ? _ScreenW : 1), H = (f32)(_ScreenH ? _ScreenH : 1);
        MappedCBV->ScreenParams     = { W, H, 1.0f / W, 1.0f / H };
        MappedCBV->DepthParams      = { _NearZ, _FarZ, _HasSceneCopies ? 1.0f : 0.0f,
                                        _UseAtmosphereSky ? 1.0f : 0.0f };
        MappedCBV->RefractionParams = { RefractionBumpScale, SoftIntersectionFactor, FogDensity, ReflectionBumpScale };
        MappedCBV->DebugParams      = { static_cast<f32>(static_cast<u32>(DebugMode)), 0.0f, 0.0f, 0.0f };
        MappedCBV->DeepColorDensity = { DeepColor.X, DeepColor.Y, DeepColor.Z, FogDensity };
        MappedCBV->InScatterColor  = { InScatterColor.X, InScatterColor.Y, InScatterColor.Z, InScatterDensity };
        MappedCBV->AbsorptionColor = { AbsorptionColor.X, AbsorptionColor.Y, AbsorptionColor.Z, 0.0f };
        MappedCBV->FoamParams = { FoamCoverage, FoamSharpness,
                                  UseFoam ? FoamIntensity : 0.0f, FoamFadeDist };
        MappedCBV->FoamColor  = { FoamColor.X, FoamColor.Y, FoamColor.Z, FoamSpecSuppress };

        const bool CanBuildTileSourcesOnGpu =
            GenerateDrawsPSO && GpuTileSourceBuffer && GpuInstanceBuffer &&
            GpuIndirectArgsBuffer && GpuDrawBucketScratchBuffer &&
            GpuDebugCounterBuffer && MappedDrawBuckets;
        if (CanBuildTileSourcesOnGpu) {
            PrepareGpuTileSources(_ViewProj, _CameraPos);
        } else {
            DebugStats = {};
            InstanceCount = 0;
            GpuTileSourceCandidateCount = 0;
        }
    }

    void FWaterRenderer::DispatchGenerateDraws(ID3D12GraphicsCommandList* _CommandList) {
        const u32 SourceCount = GpuTileSourceCandidateCount;

        if (!GenerateDrawsPSO || !GenerateDrawsRootSignature ||
            !GpuInstanceBuffer || !GpuIndirectArgsBuffer || !GpuDrawBucketScratchBuffer ||
            !GpuTileSourceBuffer ||
            !GpuDebugCounterBuffer ||
            SourceCount == 0) {
            return;
        }

        auto AppendTransition = [](D3D12_RESOURCE_BARRIER* _Barriers, UINT& _Count,
                                   ID3D12Resource* _Resource,
                                   D3D12_RESOURCE_STATES _Before,
                                   D3D12_RESOURCE_STATES _After) {
            if (!_Resource || _Before == _After) {
                return;
            }

            D3D12_RESOURCE_BARRIER& Barrier = _Barriers[_Count++];
            Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barrier.Transition.pResource = _Resource;
            Barrier.Transition.StateBefore = _Before;
            Barrier.Transition.StateAfter = _After;
            Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        };

        D3D12_RESOURCE_BARRIER ToUAV[5]{};
        UINT ToUAVCount = 0;
        AppendTransition(ToUAV, ToUAVCount, GpuTileSourceBuffer.Get(),
                         GpuTileSourceBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AppendTransition(ToUAV, ToUAVCount, GpuInstanceBuffer.Get(),
                         GpuInstanceBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AppendTransition(ToUAV, ToUAVCount, GpuIndirectArgsBuffer.Get(),
                         GpuIndirectArgsBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AppendTransition(ToUAV, ToUAVCount, GpuDrawBucketScratchBuffer.Get(),
                         GpuDrawBucketScratchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AppendTransition(ToUAV, ToUAVCount, GpuDebugCounterBuffer.Get(),
                         GpuDebugCounterState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (ToUAVCount > 0) {
            _CommandList->ResourceBarrier(ToUAVCount, ToUAV);
        }
        GpuTileSourceBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        GpuInstanceBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        GpuIndirectArgsBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        GpuDrawBucketScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        GpuDebugCounterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        ID3D12DescriptorHeap* Heaps[] = { GenerateDrawsHeap.Native() };
        _CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        _CommandList->SetComputeRootSignature(GenerateDrawsRootSignature.Get());
        _CommandList->SetPipelineState(GenerateDrawsPSO.Get());

        const u32 TileBase = FrameSlot * kMaxTileInstances;
        const u32 BucketBase = FrameSlot * kSubsetRangeCount;
        const u32 ScratchBase = FrameSlot * kSubsetRangeCount * 3u;
        const u32 OutputBase = TileBase;
        const u32 DebugBase = FrameSlot * kGpuDebugCounterCount;

        auto DispatchPass = [&](u32 _PassMode, u32 _WorkItems) {
            u32 Constants[40]{};
            Constants[0]  = SourceCount;
            Constants[1]  = TileBase;
            Constants[2]  = BucketBase;
            Constants[3]  = kSubsetRangeCount;
            Constants[4]  = _PassMode;
            Constants[5]  = ScratchBase;
            Constants[6]  = OutputBase;
            Constants[7]  = GpuTileBuildLevelCount;
            Constants[8]  = GpuTileBuildRingRadius;
            Constants[9]  = GpuTileBuildDepth;
            Constants[10] = GpuTileBuildCameraCellX;
            Constants[11] = GpuTileBuildCameraCellZ;
            Constants[12] = DebugBase;
            for (u32 Row = 0; Row < 4; ++Row) {
                for (u32 Col = 0; Col < 4; ++Col) {
                    Constants[16u + Row * 4u + Col] =
                        FloatBits(GpuTileBuildViewProj.M[Row][Col]);
                }
            }
            Constants[32] = FloatBits(GpuTileBuildRootX);
            Constants[33] = FloatBits(GpuTileBuildRootZ);
            Constants[34] = FloatBits(GpuTileBuildBaseSize);
            Constants[35] = FloatBits(WaterLevel);
            Constants[36] = FloatBits(GpuTileBuildBoundsPad);
            Constants[37] = FloatBits(UseGpuFrustumCull ? 1.0f : 0.0f);
            _CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);
            _CommandList->Dispatch((std::max(_WorkItems, 1u) + 63u) / 64u, 1, 1);

            D3D12_RESOURCE_BARRIER Barrier{};
            Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            Barrier.UAV.pResource = nullptr;
            _CommandList->ResourceBarrier(1, &Barrier);
        };
        _CommandList->SetComputeRootDescriptorTable(2, GenerateDrawsHeap.GpuHandle(2));

        _CommandList->SetComputeRootDescriptorTable(1, GenerateDrawsHeap.GpuHandle(0));
        DispatchPass(0u, kSubsetRangeCount); 

        DispatchPass(4u, SourceCount); 

        D3D12_RESOURCE_BARRIER ToTileSourceSRV[1]{};
        UINT ToTileSourceSRVCount = 0;
        AppendTransition(ToTileSourceSRV, ToTileSourceSRVCount, GpuTileSourceBuffer.Get(),
                         GpuTileSourceBufferState,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (ToTileSourceSRVCount > 0) {
            _CommandList->ResourceBarrier(ToTileSourceSRVCount, ToTileSourceSRV);
        }
        GpuTileSourceBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        DispatchPass(1u, SourceCount);      
        DispatchPass(2u, 1u);               
        DispatchPass(3u, SourceCount);      

        D3D12_RESOURCE_BARRIER ToCopy[1]{};
        UINT ToCopyCount = 0;
        AppendTransition(ToCopy, ToCopyCount, GpuDebugCounterBuffer.Get(),
                         GpuDebugCounterState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (ToCopyCount > 0) {
            _CommandList->ResourceBarrier(ToCopyCount, ToCopy);
        }
        GpuDebugCounterState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (GpuDebugReadbackBuffer) {
            const UINT64 DebugOffset = static_cast<UINT64>(DebugBase) * sizeof(u32);
            _CommandList->CopyBufferRegion(
                GpuDebugReadbackBuffer.Get(), DebugOffset,
                GpuDebugCounterBuffer.Get(), DebugOffset,
                static_cast<UINT64>(kGpuDebugCounterCount) * sizeof(u32));
        }

        D3D12_RESOURCE_BARRIER ToRead[2]{};
        UINT ToReadCount = 0;
        AppendTransition(ToRead, ToReadCount, GpuInstanceBuffer.Get(),
                         GpuInstanceBufferState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        AppendTransition(ToRead, ToReadCount, GpuIndirectArgsBuffer.Get(),
                         GpuIndirectArgsBufferState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (ToReadCount > 0) {
            _CommandList->ResourceBarrier(ToReadCount, ToRead);
        }
        GpuInstanceBufferState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        GpuIndirectArgsBufferState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }

    void FWaterRenderer::RenderSurface(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                                       u32 _SpecularCubeSRVSlot, u32 _FFTDisplacementSRVSlot,
                                       u32 _FFTNormalSRVSlot, u32 _SceneCopyTableStart,
                                       u32 _AtmosphereSkyViewSRVSlot,
                                       D3D12_GPU_VIRTUAL_ADDRESS _CSMConstantsAddress,
                                       u32 _SunShadowSRVSlot) {
        if (!PSO || InstanceCount == 0) return;

        const bool CanRenderGpuDraws =
            GenerateDrawsPSO && IndirectCommandSignature && GpuInstanceBuffer &&
            GpuIndirectArgsBuffer && GpuDrawBucketScratchBuffer &&
            GpuDebugCounterBuffer &&
            MappedDrawBuckets && GpuTileSourceCandidateCount > 0;
        if (!CanRenderGpuDraws) {
            return;
        }
        DispatchGenerateDraws(_CommandList);

        ID3D12DescriptorHeap* GraphicsHeaps[] = { _SRVHeap.Native() };
        _CommandList->SetDescriptorHeaps(_countof(GraphicsHeaps), GraphicsHeaps);
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
        _CommandList->SetGraphicsRootConstantBufferView(6, _CSMConstantsAddress);
        _CommandList->SetGraphicsRootDescriptorTable(7, _SRVHeap.GpuHandle(_SunShadowSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW Views[] = { VBView, GpuInstanceVBView };
        _CommandList->IASetVertexBuffers(0, _countof(Views), Views);
        _CommandList->IASetIndexBuffer(&IBView);
        const UINT64 ArgsOffset = static_cast<UINT64>(FrameSlot) *
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) * kSubsetRangeCount;
        _CommandList->ExecuteIndirect(
            IndirectCommandSignature.Get(), kSubsetRangeCount,
            GpuIndirectArgsBuffer.Get(), ArgsOffset,
            nullptr, 0);
    }
}
