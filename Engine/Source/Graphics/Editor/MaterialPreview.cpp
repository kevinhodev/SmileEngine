#include "Smile/Graphics/Editor/MaterialPreview.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        constexpr f32         kFovY        = 0.6981317f; // 40 graus
        constexpr u32         kMaxRowPitch = FMaterialPreview::kSize * 4;
    }

    bool FMaterialPreview::LoadEnvironment(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                           FTextureSRVHeap& _SRVHeap, const std::wstring& _Path) {
        if (!EnsureInitialized(_Device, _CmdQueue, _SRVHeap)) return false;
        if (!Env.LoadFromFile(_Device, _CmdQueue, _SRVHeap, _Path)) return false;

        // Os SRVs de irradiance/specular/BRDF LUT nascem em slots avulsos; a tabela do
        // shader (t8..t10) precisa de descritores CONTIGUOS — copia pros 3 slots reservados.
        const u32 Slots[3] = { Env.IrradianceSRV(), Env.SpecularSRV(), Env.BRDFLutSRV() };
        for (u32 i = 0; i < 3; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(IBLTableStart + i);
            D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(Slots[i]);
            UINT One = 1;
            _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        IBLTableWritten = true;
        return true;
    }

    bool FMaterialPreview::EnsureInitialized(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                             FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return true;
        if (InitFailed)  return false;

        BuildRootSignatures(_Device);
        if (!BuildPSOs(_Device)) {
            InitFailed = true;
            LogError("FMaterialPreview: shaders do preview ausentes (target Shaders?)");
            return false;
        }
        CreateTargets(_Device);
        CreateMeshes(_Device);
        Env.Initialize(_Device, _CmdQueue, _SRVHeap);
        IBLTableStart = _SRVHeap.Allocate(3);

        Initialized = true;
        LogDebug("FMaterialPreview (preview offscreen ate 1024x1024, ring async x3) inicializado");
        return true;
    }

    void FMaterialPreview::BuildRootSignatures(ID3D12Device* _Device) {
        // ---- Mesh: b0 preview CB, b1 material CB, t0-t7 material, t8-t10 IBL.
        // Params 1 e 2 casam com FMaterial::Bind (CBV + tabela) — o material se binda igual
        // ao passe real.
        {
            D3D12_DESCRIPTOR_RANGE MatRange{};
            MatRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            MatRange.NumDescriptors     = kMaterialTextureSlots;
            MatRange.BaseShaderRegister = 0;
            MatRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_DESCRIPTOR_RANGE IBLRange{};
            IBLRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            IBLRange.NumDescriptors     = 3;
            IBLRange.BaseShaderRegister = 8;
            IBLRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER P[4]{};
            P[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            P[0].Descriptor.ShaderRegister = 0;
            P[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
            P[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            P[1].Descriptor.ShaderRegister = 1;
            P[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            P[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            P[2].DescriptorTable.NumDescriptorRanges = 1;
            P[2].DescriptorTable.pDescriptorRanges   = &MatRange;
            P[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            P[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            P[3].DescriptorTable.NumDescriptorRanges = 1;
            P[3].DescriptorTable.pDescriptorRanges   = &IBLRange;
            P[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
            Samplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
            Samplers[0].MaxAnisotropy    = 8;
            Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
            Samplers[0].ShaderRegister   = 0;
            Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            Samplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
            Samplers[1].ShaderRegister   = 1;
            Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters     = _countof(P);
            Desc.pParameters       = P;
            Desc.NumStaticSamplers = _countof(Samplers);
            Desc.pStaticSamplers   = Samplers;
            Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> Blob, Err;
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
            if (FAILED(Hr)) {
                if (Err) LogError(std::string("Preview Mesh Root Sig: ") +
                                  static_cast<const char*>(Err->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                     IID_PPV_ARGS(&MeshRootSig)));
        }

        // ---- Sky: b0 CB, t0 env cube.
        {
            D3D12_DESCRIPTOR_RANGE EnvRange{};
            EnvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            EnvRange.NumDescriptors     = 1;
            EnvRange.BaseShaderRegister = 0;
            EnvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER P[2]{};
            P[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            P[0].Descriptor.ShaderRegister = 0;
            P[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            P[1].DescriptorTable.NumDescriptorRanges = 1;
            P[1].DescriptorTable.pDescriptorRanges   = &EnvRange;
            P[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC Sampler{};
            Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
            Sampler.ShaderRegister   = 0;
            Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters     = _countof(P);
            Desc.pParameters       = P;
            Desc.NumStaticSamplers = 1;
            Desc.pStaticSamplers   = &Sampler;
            Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> Blob, Err;
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
            if (FAILED(Hr)) {
                if (Err) LogError(std::string("Preview Sky Root Sig: ") +
                                  static_cast<const char*>(Err->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                     IID_PPV_ARGS(&SkyRootSig)));
        }
    }

    bool FMaterialPreview::BuildPSOs(ID3D12Device* _Device) {
        auto MeshVS = LoadShaderBytecode("MaterialPreview.vs_6_0.cso");
        auto MeshPS = LoadShaderBytecode("MaterialPreview.ps_6_0.cso");
        auto SkyVS  = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto SkyPS  = LoadShaderBytecode("PreviewSky.ps_6_0.cso");
        if (MeshVS.empty() || MeshPS.empty() || SkyVS.empty() || SkyPS.empty()) return false;

        {
            D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };

            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode        = D3D12_FILL_MODE_SOLID;
            // Cull NONE sempre: cobre two-sided/folhagem e o plano visto por baixo; o PS
            // vira a normal via SV_IsFrontFace (mesmo contrato do GBuffer).
            Raster.CullMode        = D3D12_CULL_MODE_NONE;
            Raster.DepthClipEnable = TRUE;

            D3D12_DEPTH_STENCIL_DESC Depth{};
            Depth.DepthEnable    = TRUE;
            Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS; // preview nao usa reverse-Z

            D3D12_BLEND_DESC Blend{};
            Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature        = MeshRootSig.Get();
            PSODesc.VS                    = { MeshVS.data(), MeshVS.size() };
            PSODesc.PS                    = { MeshPS.data(), MeshPS.size() };
            PSODesc.BlendState            = Blend;
            PSODesc.SampleMask            = UINT_MAX;
            PSODesc.RasterizerState       = Raster;
            PSODesc.DepthStencilState     = Depth;
            PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
            PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            PSODesc.NumRenderTargets      = 1;
            PSODesc.RTVFormats[0]         = kColorFormat;
            PSODesc.DSVFormat             = kDepthFormat;
            PSODesc.SampleDesc            = { 1, 0 };
            SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&MeshPSO)));
        }

        {
            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode        = D3D12_FILL_MODE_SOLID;
            Raster.CullMode        = D3D12_CULL_MODE_NONE;
            Raster.DepthClipEnable = TRUE;

            D3D12_DEPTH_STENCIL_DESC Depth{};
            Depth.DepthEnable    = FALSE;
            Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

            D3D12_BLEND_DESC Blend{};
            Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature        = SkyRootSig.Get();
            PSODesc.VS                    = { SkyVS.data(), SkyVS.size() };
            PSODesc.PS                    = { SkyPS.data(), SkyPS.size() };
            PSODesc.BlendState            = Blend;
            PSODesc.SampleMask            = UINT_MAX;
            PSODesc.RasterizerState       = Raster;
            PSODesc.DepthStencilState     = Depth;
            PSODesc.InputLayout           = { nullptr, 0 };
            PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            PSODesc.NumRenderTargets      = 1;
            PSODesc.RTVFormats[0]         = kColorFormat;
            PSODesc.SampleDesc            = { 1, 0 };
            SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&SkyPSO)));
        }
        return true;
    }

    void FMaterialPreview::CreateTargets(ID3D12Device* _Device) {
        {
            D3D12_CLEAR_VALUE Clear{};
            Clear.Format = kColorFormat;

            ColorTarget = GpuResources::CreateTex2D(
                _Device, kSize, kSize, kColorFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET, EVramCategory::Misc, &Clear,
                1, 1, "Preview de material");

            RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
            _Device->CreateRenderTargetView(ColorTarget.Get(), nullptr, RTVHeap.CpuHandle(0));
        }

        {
            D3D12_CLEAR_VALUE Clear{};
            Clear.Format             = kDepthFormat;
            Clear.DepthStencil.Depth = 1.0f;

            DepthTarget = GpuResources::CreateTex2D(
                _Device, kSize, kSize, kDepthFormat,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, EVramCategory::Misc, &Clear,
                1, 1, "Preview de material");

            DSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
            _Device->CreateDepthStencilView(DepthTarget.Get(), nullptr, DSVHeap.CpuHandle(0));
        }

        {
            const u64 ReadbackBytes = u64(kMaxRowPitch) * kSize;

            for (FReadbackSlot& Slot : ReadbackSlots) {
                SMILE_HR(_Device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Slot.Allocator)));
                SMILE_HR(_Device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, Slot.Allocator.Get(), nullptr,
                    IID_PPV_ARGS(&Slot.CommandList)));
                SMILE_HR(Slot.CommandList->Close());
                Slot.Readback = GpuResources::CreateReadbackBuffer(_Device, ReadbackBytes);
            }
            SMILE_HR(_Device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ReadbackFence)));
        }

        {
            // Bloco unico com os tres CBs por slot em sequencia (layout proprio, escrito a
            // mao no Submit), nao um slice por frame — dai SliceCount 1.
            const u64 Bytes = static_cast<u64>(kReadbackSlots) *
                              (sizeof(FMeshCB) + sizeof(FSkyCB) + sizeof(MaterialConstants));
            const GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(_Device, Bytes, 1, false);
            ConstantBuffer = Upload.Resource;
            MappedCB       = Upload.Mapped;
        }
    }

    void FMaterialPreview::CreateMeshes(ID3D12Device* _Device) {
        Meshes[PrimSphere].Upload(_Device, FMesh::CreateSphere());
        Meshes[PrimCube].Upload(_Device, FMesh::CreateCube());
        Meshes[PrimPlane].Upload(_Device, FMesh::CreatePlane());
        Meshes[PrimCylinder].Upload(_Device, FMesh::CreateCylinder());
    }

    FMaterialPreview::ESubmitResult FMaterialPreview::Submit(
        ID3D12Device* _Device, FCommandQueue& _CmdQueue, FTextureSRVHeap& _SRVHeap,
        FMaterial& _Material, const FParams& _Params, u64 _RequestId,
        const FGpuMesh* _SceneMesh, const Mat44& _SceneModel) {
        if (!EnsureInitialized(_Device, _CmdQueue, _SRVHeap) ||
            !_Material.IsFinalized() || _RequestId == 0)
            return ESubmitResult::Failed;

        u32 SlotIndex = kReadbackSlots;
        for (u32 Offset = 0; Offset < kReadbackSlots; ++Offset) {
            const u32 Candidate = (NextSubmitSlot + Offset) % kReadbackSlots;
            if (ReadbackSlots[Candidate].Pending) continue;
            SlotIndex = Candidate;
            break;
        }
        if (SlotIndex == kReadbackSlots) return ESubmitResult::Busy;

        FReadbackSlot& Slot = ReadbackSlots[SlotIndex];
        SMILE_HR(Slot.Allocator->Reset());
        SMILE_HR(Slot.CommandList->Reset(Slot.Allocator.Get(), nullptr));
        ID3D12GraphicsCommandList* Cl = Slot.CommandList.Get();

        const u32 RenderSize = std::clamp(_Params.RenderSize, 1u, kSize);
        const u32 RowPitch =
            (RenderSize * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

        const bool UseSceneMesh = _Params.Primitive == PrimSceneMesh &&
                                  _SceneMesh && _SceneMesh->IsValid();

        // ---- Camera orbital (LH, origem no centro da primitiva) ----
        const f32 CosP = std::cos(_Params.Pitch);
        const Vec3 Eye = { _Params.Dist * CosP * std::sin(_Params.Yaw),
                           _Params.Dist * std::sin(_Params.Pitch),
                          -_Params.Dist * CosP * std::cos(_Params.Yaw) };
        const Mat44 View = Mat44::LookAtLH(Eye, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        const Mat44 Proj  = Mat44::PerspectiveFovLH(kFovY, 1.0f, 0.05f, 50.0f);
        const Mat44 Model = UseSceneMesh ? _SceneModel : Mat44::Identity();

        FMeshCB MeshCB{};
        MeshCB.MVP             = Model * View * Proj;
        MeshCB.Model           = Model;
        MeshCB.CameraPos       = Vec4(Eye, 1.0f);
        // Sol fixo alto-lateral (posicao classica de estudio); o HDRI da o resto.
        const Vec3 SunDir = { -0.45f, 0.75f, -0.55f };
        const f32  InvLen = 1.0f / std::sqrt(SunDir.X * SunDir.X + SunDir.Y * SunDir.Y +
                                             SunDir.Z * SunDir.Z);
        MeshCB.SunDirIntensity = { SunDir.X * InvLen, SunDir.Y * InvLen, SunDir.Z * InvLen,
                                   _Params.SunIntensity };
        MeshCB.SunColor        = { 1.0f, 0.96f, 0.9f, 0.0f };
        MeshCB.IBLParams       = { 1.0f, _Params.EnvRotation,
                                   f32(FHDREnvironment::kSpecularMips - 1),
                                   Env.HasHDRLoaded() ? 1.0f : 0.0f };

        const f32 TanHalf = std::tan(kFovY * 0.5f);
        const Vec3 Fwd    = { -Eye.X / _Params.Dist, -Eye.Y / _Params.Dist, -Eye.Z / _Params.Dist };
        const Vec3 Right  = { std::cos(_Params.Yaw), 0.0f, std::sin(_Params.Yaw) };
        const Vec3 Up     = { Fwd.Y * Right.Z - Fwd.Z * Right.Y,
                              Fwd.Z * Right.X - Fwd.X * Right.Z,
                              Fwd.X * Right.Y - Fwd.Y * Right.X };
        FSkyCB SkyCB{};
        SkyCB.CamRight   = Vec4(Right, TanHalf);
        SkyCB.CamUp      = Vec4(Up, TanHalf);
        SkyCB.CamForward = Vec4(Fwd, _Params.EnvRotation);
        SkyCB.Params     = { 1.5f, 1.0f, 0.0f, 0.0f }; // mip suave, intensidade 1

        const u64 CBStride = sizeof(FMeshCB) + sizeof(FSkyCB) + sizeof(MaterialConstants);
        const u64 CBOffset = static_cast<u64>(SlotIndex) * CBStride;
        std::memcpy(MappedCB + CBOffset, &MeshCB, sizeof(MeshCB));
        std::memcpy(MappedCB + CBOffset + sizeof(FMeshCB), &SkyCB, sizeof(SkyCB));
        std::memcpy(MappedCB + CBOffset + sizeof(FMeshCB) + sizeof(FSkyCB),
                    &_Material.Constants, sizeof(MaterialConstants));

        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        Cl->SetDescriptorHeaps(1, Heaps);

        D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, f32(RenderSize), f32(RenderSize), 0.0f, 1.0f };
        D3D12_RECT Scissor{ 0, 0, LONG(RenderSize), LONG(RenderSize) };
        Cl->RSSetViewports(1, &Viewport);
        Cl->RSSetScissorRects(1, &Scissor);

        D3D12_CPU_DESCRIPTOR_HANDLE RTV = RTVHeap.CpuHandle(0);
        D3D12_CPU_DESCRIPTOR_HANDLE DSV = DSVHeap.CpuHandle(0);
        Cl->OMSetRenderTargets(1, &RTV, FALSE, &DSV);

        const f32 ClearDark[4]  = { 0.08f, 0.08f, 0.07f, 1.0f };
        const f32 ClearClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cl->ClearRenderTargetView(RTV, _Params.TransparentBackground ? ClearClear : ClearDark,
                                  1, &Scissor);
        Cl->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &Scissor);

        const D3D12_GPU_VIRTUAL_ADDRESS CBBase =
            ConstantBuffer->GetGPUVirtualAddress() + CBOffset;

        // Fundo (so com HDRI carregado; sem ele fica o clear escuro).
        if (Env.HasHDRLoaded() && !_Params.TransparentBackground) {
            Cl->SetGraphicsRootSignature(SkyRootSig.Get());
            Cl->SetPipelineState(SkyPSO.Get());
            Cl->SetGraphicsRootConstantBufferView(0, CBBase + sizeof(FMeshCB));
            Cl->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(Env.EnvCubeSRV()));
            Cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Cl->DrawInstanced(3, 1, 0, 0);
        }

        // Primitiva (ou mesh da cena) com o material.
        const int Prim = (_Params.Primitive >= 0 && _Params.Primitive < 4) ? _Params.Primitive : 0;
        const FGpuMesh& DrawMesh = UseSceneMesh ? *_SceneMesh : Meshes[Prim];
        Cl->SetGraphicsRootSignature(MeshRootSig.Get());
        Cl->SetPipelineState(MeshPSO.Get());
        Cl->SetGraphicsRootConstantBufferView(0, CBBase);
        Cl->SetGraphicsRootConstantBufferView(
            1, CBBase + sizeof(FMeshCB) + sizeof(FSkyCB));
        Cl->SetGraphicsRootDescriptorTable(
            2, _SRVHeap.GpuHandle(_Material.AlbedoDescriptorIndex()));
        if (IBLTableWritten)
            Cl->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(IBLTableStart));
        else
            Cl->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(0)); // IBL off no CB
        DrawMesh.Draw(Cl);

        // ---- Readback ----
        D3D12_RESOURCE_BARRIER ToCopy{};
        ToCopy.Transition.pResource   = ColorTarget.Get();
        ToCopy.Transition.Subresource = 0;
        ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Cl->ResourceBarrier(1, &ToCopy);

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource        = ColorTarget.Get();
        Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource                          = Slot.Readback.Get();
        Dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint.Offset             = 0;
        Dst.PlacedFootprint.Footprint.Format   = kColorFormat;
        Dst.PlacedFootprint.Footprint.Width    = RenderSize;
        Dst.PlacedFootprint.Footprint.Height   = RenderSize;
        Dst.PlacedFootprint.Footprint.Depth    = 1;
        Dst.PlacedFootprint.Footprint.RowPitch = RowPitch;
        const D3D12_BOX SourceBox{ 0, 0, 0, RenderSize, RenderSize, 1 };
        Cl->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &SourceBox);

        ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Cl->ResourceBarrier(1, &ToCopy);

        SMILE_HR(Cl->Close());
        ID3D12CommandList* Lists[] = { Cl };
        _CmdQueue.Native()->ExecuteCommandLists(1, Lists);
        const u64 FenceValue = ++NextFenceValue;
        SMILE_HR(_CmdQueue.Native()->Signal(ReadbackFence.Get(), FenceValue));

        Slot.RequestId  = _RequestId;
        Slot.FenceValue = FenceValue;
        Slot.Size       = RenderSize;
        Slot.RowPitch   = RowPitch;
        Slot.Pending    = true;
        NextSubmitSlot  = (SlotIndex + 1) % kReadbackSlots;
        return ESubmitResult::Submitted;
    }

    bool FMaterialPreview::ConsumeCompleted(FResult& _Out) {
        if (!ReadbackFence) return false;

        const u64 Completed = ReadbackFence->GetCompletedValue();
        FReadbackSlot* Ready = nullptr;
        for (FReadbackSlot& Slot : ReadbackSlots) {
            if (!Slot.Pending || Slot.FenceValue > Completed) continue;
            if (!Ready || Slot.FenceValue < Ready->FenceValue) Ready = &Slot;
        }
        if (!Ready) return false;

        _Out.RequestId = Ready->RequestId;
        _Out.Size      = Ready->Size;
        const size_t TightRow = size_t(Ready->Size) * 4u;
        _Out.Pixels.resize(TightRow * Ready->Size);
        void* Mapped = nullptr;
        D3D12_RANGE ReadRange{ 0, size_t(Ready->RowPitch) * Ready->Size };
        SMILE_HR(Ready->Readback->Map(0, &ReadRange, &Mapped));
        if (Ready->RowPitch == TightRow) {
            std::memcpy(_Out.Pixels.data(), Mapped, _Out.Pixels.size());
        } else {
            const auto* SrcRow = static_cast<const u8*>(Mapped);
            for (u32 Y = 0; Y < Ready->Size; ++Y)
                std::memcpy(_Out.Pixels.data() + size_t(Y) * TightRow,
                            SrcRow + size_t(Y) * Ready->RowPitch, TightRow);
        }
        D3D12_RANGE WrittenRange{ 0, 0 };
        Ready->Readback->Unmap(0, &WrittenRange);
        Ready->Pending    = false;
        Ready->RequestId  = 0;
        Ready->FenceValue = 0;
        Ready->Size       = 0;
        Ready->RowPitch   = 0;
        return true;
    }

    FPassShaderStems FMaterialPreview::ShaderStems() const {
        static const char* const kStems[] = { "MaterialPreview.vs", "MaterialPreview.ps", "PreviewSky.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FMaterialPreview::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (MeshPSO) BuildPSOs(_Ctx.Device);
    }

}
