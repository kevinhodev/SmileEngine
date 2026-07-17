#include "Smile/Graphics/Terrain.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

namespace Smile {
    namespace {
        // Vertice do grid compartilhado: so UV [0,1] no chunk + pesos de morph por quadrante
        // (a altura vem da heightmap no VS). Espelha o input layout dos PSOs abaixo.
        struct TerrainVertex {
            f32 U, V;
            u8  Morph[4];
        };

        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(
            ID3D12Device* _Device, const void* _Src, UINT64 _Size) {
            D3D12_HEAP_PROPERTIES HeapProps{};
            HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                     &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                     IID_PPV_ARGS(&Buffer)));
            if (_Src) {
                D3D12_RANGE NoRead{ 0, 0 };
                void* Mapped = nullptr;
                SMILE_HR(Buffer->Map(0, &NoRead, &Mapped));
                std::memcpy(Mapped, _Src, _Size);
                Buffer->Unmap(0, nullptr);
            }
            return Buffer;
        }
    }

    void FTerrain::Initialize(ID3D12Device* _Device) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        RecreatePSOs(_Device);
        BuildGrids(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FTerrain::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_ROOT_PARAMETER RootParams[5]{};

        // b0: TerrainCB (matrizes + parametros do terreno)
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        // b1: constantes por chunk (root constants — 8 dwords, sem CB por chunk)
        RootParams[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[1].Constants.ShaderRegister = 1;
        RootParams[1].Constants.Num32BitValues = sizeof(ChunkConstants) / 4;
        RootParams[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        // t0: heightmap (VS ergue o vertice, PS deriva a normal)
        D3D12_DESCRIPTOR_RANGE HeightmapRange{};
        HeightmapRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        HeightmapRange.NumDescriptors                    = 1;
        HeightmapRange.BaseShaderRegister                = 0;
        HeightmapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &HeightmapRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        // b2: CB da cascata do CSM (so o passe de sombra liga)
        RootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[3].Descriptor.ShaderRegister = 2;
        RootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        // t1-t8: camadas do material F2 (4 albedo + 4 normal), tabela contigua
        D3D12_DESCRIPTOR_RANGE LayerRange{};
        LayerRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        LayerRange.NumDescriptors                    = 2 * FTerrainDesc::kLayers;
        LayerRange.BaseShaderRegister                = 1;
        LayerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &LayerRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0: bilinear clamp (normal do heightfield no PS; o VS usa Load — texel exato)
        // s1: aniso wrap (texturas das camadas, tiling em mundo)
        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        Samplers[1] = Samplers[0];
        Samplers[1].Filter           = D3D12_FILTER_ANISOTROPIC;
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].MaxAnisotropy    = 8;
        Samplers[1].ShaderRegister   = 1;
        Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, Error;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error);
        if (FAILED(Hr)) {
            if (Error)
                LogError(std::string("Terrain root signature: ") +
                         static_cast<const char*>(Error->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FTerrain::RecreatePSOs(ID3D12Device* _Device) {
        auto VSBlob        = LoadShaderBytecode("Terrain.vs_6_0.cso");
        auto ShadowVSBlob  = LoadShaderBytecode("TerrainShadow.vs_6_0.cso");
        auto GBufferPSBlob = LoadShaderBytecode("TerrainGBuffer.ps_6_0.cso");
        auto NormalPSBlob  = LoadShaderBytecode("TerrainDepthNormal.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_BACK;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
        Blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        Blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC DepthWrite{};
        DepthWrite.DepthEnable    = TRUE;
        DepthWrite.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthWrite.DepthFunc      = kDepthFuncLess; // reverse-Z: GREATER

        D3D12_DEPTH_STENCIL_DESC DepthEqual{};
        DepthEqual.DepthEnable    = TRUE;
        DepthEqual.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        DepthEqual.DepthFunc      = D3D12_COMPARISON_FUNC_EQUAL; // le o depth do prepass

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = RootSig.Get();
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.InputLayout           = { InputLayout, _countof(InputLayout) };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        Desc.SampleDesc            = { 1, 0 };

        // Z-prepass depth-only (AO off): mesmo RT do prepass principal com writemask 0.
        D3D12_BLEND_DESC NoColor = Blend;
        NoColor.RenderTarget[0].RenderTargetWriteMask = 0;
        Desc.VS                = { VSBlob.data(), VSBlob.size() };
        Desc.PS                = { nullptr, 0 };
        Desc.BlendState        = NoColor;
        Desc.DepthStencilState = DepthWrite;
        Desc.NumRenderTargets  = 1;
        Desc.RTVFormats[0]     = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> NewDepthOnly;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&NewDepthOnly)));
        PSODepthOnly = NewDepthOnly;

        // Z-prepass depth+normal (GTAO on): escreve o NormalBuffer R10G10B10A2.
        Desc.PS            = { NormalPSBlob.data(), NormalPSBlob.size() };
        Desc.BlendState    = Blend;
        Desc.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> NewDepthNormal;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&NewDepthNormal)));
        PSODepthNormal = NewDepthNormal;

        // Geometry pass: MRT do G-buffer + velocity, depth EQUAL (terreno sempre no prepass).
        Desc.PS                = { GBufferPSBlob.data(), GBufferPSBlob.size() };
        Desc.DepthStencilState = DepthEqual;
        Desc.NumRenderTargets  = FGBuffer::kTargetCount + 1;
        Desc.RTVFormats[0]     = FGBuffer::kFormatA;
        Desc.RTVFormats[1]     = FGBuffer::kFormatB;
        Desc.RTVFormats[2]     = FGBuffer::kFormatC;
        Desc.RTVFormats[3]     = DXGI_FORMAT_R16G16_FLOAT;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> NewGBuffer;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&NewGBuffer)));
        PSOGBuffer = NewGBuffer;

        // CSM: forward-Z LESS, pancaking (depth clip off) e slope bias — espelha o
        // OpaquePSO do FSunShadows.
        D3D12_RASTERIZER_DESC ShadowRaster = Raster;
        ShadowRaster.CullMode             = D3D12_CULL_MODE_NONE;
        ShadowRaster.DepthClipEnable      = FALSE;
        ShadowRaster.SlopeScaledDepthBias = 1.0f;

        D3D12_DEPTH_STENCIL_DESC ShadowDepth{};
        ShadowDepth.DepthEnable    = TRUE;
        ShadowDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ShadowDepth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

        Desc.VS                = { ShadowVSBlob.data(), ShadowVSBlob.size() };
        Desc.PS                = { nullptr, 0 };
        Desc.RasterizerState   = ShadowRaster;
        Desc.DepthStencilState = ShadowDepth;
        Desc.BlendState        = NoColor;
        Desc.NumRenderTargets  = 0;
        Desc.RTVFormats[0]     = DXGI_FORMAT_UNKNOWN;
        Desc.RTVFormats[1]     = DXGI_FORMAT_UNKNOWN;
        Desc.RTVFormats[2]     = DXGI_FORMAT_UNKNOWN;
        Desc.RTVFormats[3]     = DXGI_FORMAT_UNKNOWN;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> NewShadow;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&NewShadow)));
        PSOShadow = NewShadow;
    }

    void FTerrain::BuildGrids(ID3D12Device* _Device) {
        // Um grid por LOD, compartilhado por todos os chunks (estilo Flax): (quads+1)^2
        // vertices com UV + pesos de morph baricentricos (pow 0.3 concentra o morph na borda).
        for (u32 Lod = 0; Lod < kMaxLods; ++Lod) {
            const u32 Quads = std::max(kChunkQuads >> Lod, 1u);
            const u32 Verts = Quads + 1;

            std::vector<TerrainVertex> VB(static_cast<size_t>(Verts) * Verts);
            for (u32 z = 0; z < Verts; ++z) {
                for (u32 x = 0; x < Verts; ++x) {
                    TerrainVertex& V = VB[static_cast<size_t>(z) * Verts + x];
                    V.U = static_cast<f32>(x) / Quads;
                    V.V = static_cast<f32>(z) / Quads;
                    constexpr f32 kAdjust = 0.3f;
                    const f32 C[4] = { V.V, V.U, 1.0f - V.U, 1.0f - V.V };
                    for (int i = 0; i < 4; ++i) {
                        const f32 W = std::pow(C[i], kAdjust);
                        V.Morph[i] = static_cast<u8>(std::clamp(W, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                }
            }

            std::vector<u16> IB;
            IB.reserve(static_cast<size_t>(Quads) * Quads * 6);
            for (u32 z = 0; z < Quads; ++z) {
                for (u32 x = 0; x < Quads; ++x) {
                    const u16 I00 = static_cast<u16>((z + 0) * Verts + (x + 0));
                    const u16 I10 = static_cast<u16>((z + 0) * Verts + (x + 1));
                    const u16 I01 = static_cast<u16>((z + 1) * Verts + (x + 0));
                    const u16 I11 = static_cast<u16>((z + 1) * Verts + (x + 1));
                    IB.push_back(I00); IB.push_back(I11); IB.push_back(I10);
                    IB.push_back(I00); IB.push_back(I01); IB.push_back(I11);
                }
            }

            FGrid& G = Grids[Lod];
            G.VB = CreateUploadBuffer(_Device, VB.data(), VB.size() * sizeof(TerrainVertex));
            G.IB = CreateUploadBuffer(_Device, IB.data(), IB.size() * sizeof(u16));
            G.VBV.BufferLocation = G.VB->GetGPUVirtualAddress();
            G.VBV.StrideInBytes  = sizeof(TerrainVertex);
            G.VBV.SizeInBytes    = static_cast<UINT>(VB.size() * sizeof(TerrainVertex));
            G.IBV.BufferLocation = G.IB->GetGPUVirtualAddress();
            G.IBV.Format         = DXGI_FORMAT_R16_UINT;
            G.IBV.SizeInBytes    = static_cast<UINT>(IB.size() * sizeof(u16));
            G.IndexCount         = static_cast<u32>(IB.size());
        }
    }

    void FTerrain::CreateConstantBuffer(ID3D12Device* _Device) {
        ConstantBuffer = CreateUploadBuffer(
            _Device, nullptr,
            static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(TerrainConstants));
        D3D12_RANGE NoRead{ 0, 0 };
        void* p = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &p));
        MappedCB = reinterpret_cast<u8*>(p);
    }

    bool FTerrain::Load(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                        FTextureSRVHeap& _SRVHeap, const FTerrainDesc& _Desc) {
        std::ifstream File(_Desc.HeightmapPath, std::ios::binary | std::ios::ate);
        if (!File) {
            LogError("Terreno: nao abriu a heightmap (esperado RAW u16 .r16)");
            return false;
        }
        const std::streamoff Bytes = File.tellg();
        File.seekg(0, std::ios::beg);

        u32 Size = _Desc.HeightmapSize;
        if (Size == 0)
            Size = static_cast<u32>(std::sqrt(static_cast<f64>(Bytes) / 2.0) + 0.5);
        if (Size < kChunkQuads || (Size & (Size - 1)) != 0 ||
            static_cast<std::streamoff>(Size) * Size * 2 != Bytes) {
            LogError("Terreno: heightmap invalida (precisa ser RAW u16 quadrado, potencia de 2, >= " +
                     std::to_string(kChunkQuads) + " texels por lado)");
            return false;
        }

        std::vector<u16> Mip0(static_cast<size_t>(Size) * Size);
        File.read(reinterpret_cast<char*>(Mip0.data()), static_cast<std::streamsize>(Bytes));
        if (!File) {
            LogError("Terreno: leitura da heightmap falhou");
            return false;
        }

        Unload(_SRVHeap);

        // Mips por DECIMACAO (mip N pega o texel de indice par do mip N-1, nunca media):
        // a altura do alvo do morph no VS fica IDENTICA a que o proximo LOD renderiza —
        // costura sem crack vertical por construcao.
        FTextureCPUData CPU;
        CPU.Width  = Size;
        CPU.Height = Size;
        CPU.Format = DXGI_FORMAT_R16_UNORM;
        u32 MipCount = 1;
        for (u32 s = Size; s > 1; s >>= 1) ++MipCount;
        CPU.Mips.resize(MipCount);
        {
            FMipData& M0 = CPU.Mips[0];
            M0.Width = M0.Height = Size;
            M0.Pixels.resize(Mip0.size() * 2);
            std::memcpy(M0.Pixels.data(), Mip0.data(), M0.Pixels.size());
        }
        for (u32 m = 1; m < MipCount; ++m) {
            const u32 SrcSize = Size >> (m - 1);
            const u32 DstSize = std::max(Size >> m, 1u);
            const u16* Src = reinterpret_cast<const u16*>(CPU.Mips[m - 1].Pixels.data());
            FMipData& Dst = CPU.Mips[m];
            Dst.Width = Dst.Height = DstSize;
            Dst.Pixels.resize(static_cast<size_t>(DstSize) * DstSize * 2);
            u16* DstPx = reinterpret_cast<u16*>(Dst.Pixels.data());
            for (u32 z = 0; z < DstSize; ++z)
                for (u32 x = 0; x < DstSize; ++x)
                    DstPx[static_cast<size_t>(z) * DstSize + x] =
                        Src[static_cast<size_t>(z) * 2 * SrcSize + x * 2];
        }

        Heightmap = FTexture::CreateFromCPU(_Device, _UploadQueue, _SRVHeap, CPU,
                                            EVramCategory::Terrain);
        if (!Heightmap.IsValid()) {
            LogError("Terreno: upload da heightmap falhou");
            return false;
        }

        // F2: camadas de material (4 albedo sRGB + 4 normal linear). Decode WIC em paralelo
        // (mesmo padrao do SceneLoader), upload em batch, e faltante vira fallback 1x1
        // (branco/flat) — a tabela t1-t8 SEMPRE tem descritor valido.
        {
            constexpr u32 kSlots = 2 * FTerrainDesc::kLayers;
            std::vector<FTextureCPUData> LayerCPU(kSlots);
            {
                std::vector<std::thread> Pool;
                for (u32 i = 0; i < kSlots; ++i) {
                    const bool IsNormal = i >= FTerrainDesc::kLayers;
                    const std::wstring& Path = IsNormal
                        ? _Desc.LayerNormal[i - FTerrainDesc::kLayers]
                        : _Desc.LayerAlbedo[i];
                    if (Path.empty()) continue;
                    Pool.emplace_back([&LayerCPU, i, IsNormal, Path]() {
                        LayerCPU[i] = FTexture::LoadCPU(Path, IsNormal, !IsNormal);
                    });
                }
                for (auto& T : Pool) T.join();
            }
            HasLayers = false;
            for (u32 i = 0; i < kSlots; ++i) {
                if (LayerCPU[i].Valid()) { HasLayers = true; continue; }
                // Fallback 1x1: branco (albedo) / normal flat (0.5, 0.5, 1)
                const bool IsNormal = i >= FTerrainDesc::kLayers;
                FMipData Mip;
                Mip.Width = Mip.Height = 1;
                Mip.Pixels = IsNormal ? std::vector<u8>{ 128, 128, 255, 255 }
                                      : std::vector<u8>{ 255, 255, 255, 255 };
                LayerCPU[i].Mips = { std::move(Mip) };
                LayerCPU[i].Width = LayerCPU[i].Height = 1;
                LayerCPU[i].Format = IsNormal ? DXGI_FORMAT_R8G8B8A8_UNORM
                                              : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }
            for (u32 i = 0; i < kSlots; ++i)
                LayerTex[i] = FTexture::CreateFromCPU(_Device, _UploadQueue, _SRVHeap,
                                                      LayerCPU[i], EVramCategory::Terrain);

            // Tabela contigua t1-t8 (padrao FMaterial: SRVs re-criados nos slots da tabela;
            // os slots individuais das FTextures seguem donos do lifetime).
            LayerTableStart = _SRVHeap.Allocate(kSlots);
            for (u32 i = 0; i < kSlots; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
                SRV.Format                        = LayerTex[i].Format();
                SRV.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
                SRV.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                SRV.Texture2D.MipLevels           = LayerTex[i].MipCount();
                SRV.Texture2D.MostDetailedMip     = 0;
                SRV.Texture2D.ResourceMinLODClamp = 0.0f;
                _SRVHeap.CreateSRV(_Device, LayerTex[i].Resource(), SRV, LayerTableStart + i);
            }
        }

        Desc_          = _Desc;
        Desc_.HeightmapSize = Size;
        HeightmapSize  = Size;
        ChunksPerSide  = Size / kChunkQuads;
        // LOD mais alto limitado pelo grid (1 quad) e pela existencia do mip do alvo do morph.
        MaxLod = std::min<u32>(kMaxLods - 1, MipCount >= 2 ? MipCount - 2 : 0);

        // Min/max de altura por chunk (mip 0, com a borda compartilhada) — AABB do culling
        // e esfera do LOD. Altura normalizada [0,1].
        ChunkMinH.assign(static_cast<size_t>(ChunksPerSide) * ChunksPerSide, 1.0f);
        ChunkMaxH.assign(static_cast<size_t>(ChunksPerSide) * ChunksPerSide, 0.0f);
        for (u32 cz = 0; cz < ChunksPerSide; ++cz) {
            for (u32 cx = 0; cx < ChunksPerSide; ++cx) {
                f32 Mn = 1.0f, Mx = 0.0f;
                const u32 X1 = std::min(cx * kChunkQuads + kChunkQuads, Size - 1);
                const u32 Z1 = std::min(cz * kChunkQuads + kChunkQuads, Size - 1);
                for (u32 z = cz * kChunkQuads; z <= Z1; ++z) {
                    const u16* Row = Mip0.data() + static_cast<size_t>(z) * Size;
                    for (u32 x = cx * kChunkQuads; x <= X1; ++x) {
                        const f32 H = Row[x] * (1.0f / 65535.0f);
                        Mn = std::min(Mn, H);
                        Mx = std::max(Mx, H);
                    }
                }
                ChunkMinH[static_cast<size_t>(cz) * ChunksPerSide + cx] = Mn;
                ChunkMaxH[static_cast<size_t>(cz) * ChunksPerSide + cx] = Mx;
            }
        }

        ChunkLods.assign(static_cast<size_t>(ChunksPerSide) * ChunksPerSide, 0);
        Visible.clear();

        const f32 SizeWorld = Size * Desc_.UnitsPerTexel;
        BoundsMin = { Desc_.Origin.X, Desc_.Origin.Y, Desc_.Origin.Z };
        BoundsMax = { Desc_.Origin.X + SizeWorld, Desc_.Origin.Y + Desc_.HeightScale,
                      Desc_.Origin.Z + SizeWorld };

        LogInfo("Terreno carregado: " + std::to_string(Size) + "^2 texels, " +
                std::to_string(ChunksPerSide) + "x" + std::to_string(ChunksPerSide) +
                " chunks, maxLod=" + std::to_string(MaxLod));
        return true;
    }

    void FTerrain::Unload(FTextureSRVHeap& _SRVHeap) {
        if (Heightmap.IsValid())
            Heightmap.Release(_SRVHeap);
        for (FTexture& T : LayerTex)
            if (T.IsValid()) T.Release(_SRVHeap);
        if (LayerTableStart != 0xFFFFFFFFu) {
            _SRVHeap.Free(LayerTableStart, 2 * FTerrainDesc::kLayers);
            LayerTableStart = 0xFFFFFFFFu;
        }
        HasLayers = false;
        HeightmapSize = 0;
        ChunksPerSide = 0;
        ChunkMinH.clear();
        ChunkMaxH.clear();
        ChunkLods.clear();
        Visible.clear();
    }

    void FTerrain::UpdatePerFrame(u32 _FrameSlot, const Mat44& _ViewProj,
                                  const Mat44& _ViewProjNoJitter, const Mat44& _PrevViewProj,
                                  const Vec3& _CameraPos, f32 _FovYRadians, f32 _MipBias) {
        if (!IsLoaded()) return;
        FrameSlot_ = _FrameSlot;

        TerrainConstants CB{};
        CB.ViewProj         = _ViewProj;
        CB.ViewProjNoJitter = _ViewProjNoJitter;
        CB.PrevViewProj     = _PrevViewProj;
        CB.OriginUnits      = { Desc_.Origin.X, Desc_.Origin.Y, Desc_.Origin.Z,
                                Desc_.UnitsPerTexel };
        CB.Params           = { Desc_.HeightScale, static_cast<f32>(HeightmapSize),
                                static_cast<f32>(kChunkQuads), static_cast<f32>(MaxLod) };
        CB.Params2          = { DebugLodColors ? 1.0f : 0.0f, BaseGray, Roughness,
                                HasLayers ? 1.0f : 0.0f };
        CB.Params3          = { _MipBias, Desc_.BlendContrast, Desc_.DirtScale,
                                Desc_.DirtAmount };
        CB.Params4          = { Desc_.RockSlopeStart, Desc_.RockSlopeEnd,
                                Desc_.HighStart, Desc_.HighEnd };
        // Tiling em 1/metros (o shader multiplica worldXZ direto)
        CB.LayerTiling      = { 1.0f / Desc_.LayerTile[0], 1.0f / Desc_.LayerTile[1],
                                1.0f / Desc_.LayerTile[2], 1.0f / Desc_.LayerTile[3] };
        CB.LayerRough       = { Desc_.LayerRough[0], Desc_.LayerRough[1],
                                Desc_.LayerRough[2], Desc_.LayerRough[3] };
        std::memcpy(MappedCB + static_cast<size_t>(_FrameSlot) * sizeof(TerrainConstants),
                    &CB, sizeof(CB));

        // LOD por screen-size (estilo UE): fracao da altura da tela que a esfera do chunk
        // ocupa; cada halving sobe um LOD. Continuo -> o morph do VS suaviza a transicao.
        const f32 ChunkWorld  = kChunkQuads * Desc_.UnitsPerTexel;
        const f32 TanHalfFov  = std::tan(0.5f * _FovYRadians);

        for (u32 cz = 0; cz < ChunksPerSide; ++cz) {
            for (u32 cx = 0; cx < ChunksPerSide; ++cx) {
                const size_t Idx = static_cast<size_t>(cz) * ChunksPerSide + cx;
                const f32 MinH = ChunkMinH[Idx] * Desc_.HeightScale;
                const f32 MaxH = ChunkMaxH[Idx] * Desc_.HeightScale;
                const f32 Cx = Desc_.Origin.X + (cx + 0.5f) * ChunkWorld;
                const f32 Cy = Desc_.Origin.Y + 0.5f * (MinH + MaxH);
                const f32 Cz = Desc_.Origin.Z + (cz + 0.5f) * ChunkWorld;
                const f32 Ex = 0.5f * ChunkWorld;
                const f32 Ey = 0.5f * (MaxH - MinH);
                const f32 Radius = std::sqrt(Ex * Ex + Ex * Ex + Ey * Ey);

                const f32 Dx = Cx - _CameraPos.X;
                const f32 Dy = Cy - _CameraPos.Y;
                const f32 Dz = Cz - _CameraPos.Z;
                const f32 Dist = std::max(std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz), 1e-3f);

                const f32 ScreenSize = Radius / (Dist * TanHalfFov);
                i32 Lod = 0;
                if (ScreenSize < Lod0ScreenSize)
                    Lod = static_cast<i32>(std::log2(Lod0ScreenSize / ScreenSize)) + 1;
                Lod = std::clamp(Lod + LodBias, 0, static_cast<i32>(MaxLod));
                ChunkLods[Idx] = static_cast<u8>(Lod);
            }
        }

        // Frustum cull da vista (planos da VP, colunas — mesma convencao do FSunShadows).
        const Mat44& M = _ViewProj;
        const Vec4 c0{ M.M[0][0], M.M[1][0], M.M[2][0], M.M[3][0] };
        const Vec4 c1{ M.M[0][1], M.M[1][1], M.M[2][1], M.M[3][1] };
        const Vec4 c2{ M.M[0][2], M.M[1][2], M.M[2][2], M.M[3][2] };
        const Vec4 c3{ M.M[0][3], M.M[1][3], M.M[2][3], M.M[3][3] };
        const Vec4 Planes[6] = {
            { c3.X + c0.X, c3.Y + c0.Y, c3.Z + c0.Z, c3.W + c0.W },
            { c3.X - c0.X, c3.Y - c0.Y, c3.Z - c0.Z, c3.W - c0.W },
            { c3.X + c1.X, c3.Y + c1.Y, c3.Z + c1.Z, c3.W + c1.W },
            { c3.X - c1.X, c3.Y - c1.Y, c3.Z - c1.Z, c3.W - c1.W },
            { c2.X,        c2.Y,        c2.Z,        c2.W        },
            { c3.X - c2.X, c3.Y - c2.Y, c3.Z - c2.Z, c3.W - c2.W },
        };

        Visible.clear();
        for (u32 cz = 0; cz < ChunksPerSide; ++cz) {
            for (u32 cx = 0; cx < ChunksPerSide; ++cx) {
                const size_t Idx = static_cast<size_t>(cz) * ChunksPerSide + cx;
                const Vec3 Mn{ Desc_.Origin.X + cx * ChunkWorld,
                               Desc_.Origin.Y + ChunkMinH[Idx] * Desc_.HeightScale,
                               Desc_.Origin.Z + cz * ChunkWorld };
                const Vec3 Mx{ Mn.X + ChunkWorld,
                               Desc_.Origin.Y + ChunkMaxH[Idx] * Desc_.HeightScale,
                               Mn.Z + ChunkWorld };
                bool Outside = false;
                for (int p = 0; p < 6 && !Outside; ++p) {
                    const Vec4& P = Planes[p];
                    const f32 px = (P.X >= 0.0f) ? Mx.X : Mn.X;
                    const f32 py = (P.Y >= 0.0f) ? Mx.Y : Mn.Y;
                    const f32 pz = (P.Z >= 0.0f) ? Mx.Z : Mn.Z;
                    Outside = (P.X * px + P.Y * py + P.Z * pz + P.W < 0.0f);
                }
                if (!Outside)
                    Visible.push_back(static_cast<u32>(Idx));
            }
        }
    }

    void FTerrain::DrawChunks(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                              ID3D12PipelineState* _PSO, const std::vector<u32>& _List,
                              D3D12_GPU_VIRTUAL_ADDRESS _CascadeCB) {
        if (_List.empty()) return;

        _Cmd->SetGraphicsRootSignature(RootSig.Get());
        _Cmd->SetPipelineState(_PSO);
        _Cmd->SetGraphicsRootConstantBufferView(
            0, ConstantBuffer->GetGPUVirtualAddress() +
               static_cast<u64>(FrameSlot_) * sizeof(TerrainConstants));
        _Cmd->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(Heightmap.SRVSlot()));
        if (LayerTableStart != 0xFFFFFFFFu)
            _Cmd->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(LayerTableStart));
        if (_CascadeCB)
            _Cmd->SetGraphicsRootConstantBufferView(3, _CascadeCB);
        _Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        u32 BoundLod = 0xFFFFFFFFu;
        for (const u32 Idx : _List) {
            const u32 cx  = Idx % ChunksPerSide;
            const u32 cz  = Idx / ChunksPerSide;
            const u32 Lod = ChunkLods[Idx];

            // Vizinho mais grosso puxa a borda pro grid dele (morph); mais fino ou igual
            // nao morfa (clamp [lod, lod+1] — quem morfa e sempre o lado mais fino).
            auto NeighborLod = [&](i32 nx, i32 nz) -> f32 {
                u32 N = Lod;
                if (nx >= 0 && nz >= 0 && nx < static_cast<i32>(ChunksPerSide) &&
                    nz < static_cast<i32>(ChunksPerSide))
                    N = ChunkLods[static_cast<size_t>(nz) * ChunksPerSide + nx];
                const u32 Hi = std::min(Lod + 1u, MaxLod);
                return static_cast<f32>(std::clamp(N, Lod, Hi));
            };

            ChunkConstants CC{};
            CC.ChunkX = cx;
            CC.ChunkZ = cz;
            CC.Lod    = Lod;
            CC.NeighborLod[0] = NeighborLod(static_cast<i32>(cx), static_cast<i32>(cz) - 1);
            CC.NeighborLod[1] = NeighborLod(static_cast<i32>(cx) - 1, static_cast<i32>(cz));
            CC.NeighborLod[2] = NeighborLod(static_cast<i32>(cx) + 1, static_cast<i32>(cz));
            CC.NeighborLod[3] = NeighborLod(static_cast<i32>(cx), static_cast<i32>(cz) + 1);
            _Cmd->SetGraphicsRoot32BitConstants(1, sizeof(ChunkConstants) / 4, &CC, 0);

            if (Lod != BoundLod) {
                _Cmd->IASetVertexBuffers(0, 1, &Grids[Lod].VBV);
                _Cmd->IASetIndexBuffer(&Grids[Lod].IBV);
                BoundLod = Lod;
            }
            _Cmd->DrawIndexedInstanced(Grids[Lod].IndexCount, 1, 0, 0, 0);
        }
    }

    void FTerrain::RenderDepthPrepass(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                      bool _WithNormal) {
        if (!IsLoaded()) return;
        DrawChunks(_Cmd, _SRVHeap, _WithNormal ? PSODepthNormal.Get() : PSODepthOnly.Get(),
                   Visible);
    }

    void FTerrain::RenderGBuffer(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap) {
        if (!IsLoaded()) return;
        DrawChunks(_Cmd, _SRVHeap, PSOGBuffer.Get(), Visible);
    }

    void FTerrain::RenderShadowCascade(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                       D3D12_GPU_VIRTUAL_ADDRESS _CascadeCB,
                                       const Mat44& _CascadeVP) {
        if (!IsLoaded()) return;

        // Culling da cascata: 5 planos (sem o near — pancaking, igual ao FSunShadows).
        const Mat44& M = _CascadeVP;
        const Vec4 c0{ M.M[0][0], M.M[1][0], M.M[2][0], M.M[3][0] };
        const Vec4 c1{ M.M[0][1], M.M[1][1], M.M[2][1], M.M[3][1] };
        const Vec4 c2{ M.M[0][2], M.M[1][2], M.M[2][2], M.M[3][2] };
        const Vec4 c3{ M.M[0][3], M.M[1][3], M.M[2][3], M.M[3][3] };
        const Vec4 Planes[5] = {
            { c3.X + c0.X, c3.Y + c0.Y, c3.Z + c0.Z, c3.W + c0.W },
            { c3.X - c0.X, c3.Y - c0.Y, c3.Z - c0.Z, c3.W - c0.W },
            { c3.X + c1.X, c3.Y + c1.Y, c3.Z + c1.Z, c3.W + c1.W },
            { c3.X - c1.X, c3.Y - c1.Y, c3.Z - c1.Z, c3.W - c1.W },
            { c3.X - c2.X, c3.Y - c2.Y, c3.Z - c2.Z, c3.W - c2.W },
        };

        const f32 ChunkWorld = kChunkQuads * Desc_.UnitsPerTexel;
        std::vector<u32> List;
        List.reserve(ChunkLods.size());
        for (u32 cz = 0; cz < ChunksPerSide; ++cz) {
            for (u32 cx = 0; cx < ChunksPerSide; ++cx) {
                const size_t Idx = static_cast<size_t>(cz) * ChunksPerSide + cx;
                const Vec3 Mn{ Desc_.Origin.X + cx * ChunkWorld,
                               Desc_.Origin.Y + ChunkMinH[Idx] * Desc_.HeightScale,
                               Desc_.Origin.Z + cz * ChunkWorld };
                const Vec3 Mx{ Mn.X + ChunkWorld,
                               Desc_.Origin.Y + ChunkMaxH[Idx] * Desc_.HeightScale,
                               Mn.Z + ChunkWorld };
                bool Outside = false;
                for (int p = 0; p < 5 && !Outside; ++p) {
                    const Vec4& P = Planes[p];
                    const f32 px = (P.X >= 0.0f) ? Mx.X : Mn.X;
                    const f32 py = (P.Y >= 0.0f) ? Mx.Y : Mn.Y;
                    const f32 pz = (P.Z >= 0.0f) ? Mx.Z : Mn.Z;
                    Outside = (P.X * px + P.Y * py + P.Z * pz + P.W < 0.0f);
                }
                if (!Outside)
                    List.push_back(static_cast<u32>(Idx));
            }
        }
        DrawChunks(_Cmd, _SRVHeap, PSOShadow.Get(), List, _CascadeCB);
    }
}
