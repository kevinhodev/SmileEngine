#include "Smile/Graphics/DebugDraw.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <algorithm>
#include <string>

namespace Smile {
    static constexpr u32 kFIF = FCommandQueue::kFramesInFlight;
    // Bias em NDC do teste manual de depth das linhas ocluiveis: sem ele, linha encostada numa
    // superficie serrilha (o fragmento e a cena caem no mesmo depth e o resultado vira ruido).
    // Empirico, calibrado com o wire do volume de luz colado no chao.
    static constexpr f32 kDepthTestBiasNdc = 2e-5f;

    void FDebugDraw::Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat) {
        if (Initialized) return;
        BuildRootSignature(Device);
        BuildPSOs(Device, RTFormat);
        CreateBuffers(Device);
        Initialized = true;
        LogDebug("DebugDraw Inicializado");
    }

    void FDebugDraw::WarnXRayOnce() {
        if (XRayWarned) return;
        XRayWarned = true;
        LogWarning("DebugDraw: modo XRay pedido, mas ele so existe a partir da F1b (precisa do "
                   "alpha blend). Desenhando como Scene — a parte oculta SOME em vez de ficar "
                   "translucida");
    }

    void FDebugDraw::Line(const Vec3& A, const Vec3& B, const Vec4& Color,
                          EDebugDepthMode Mode, f32 Thickness) {
        if (Mode == EDebugDepthMode::XRay) WarnXRayOnce();
        if (Mode != EDebugDepthMode::Foreground) ++SceneDepthPrims;
        LineCmds.push_back({ A, B, Color, Thickness, Mode });
    }

    void FDebugDraw::Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec4& Color,
                              EDebugDepthMode Mode) {
        if (Mode == EDebugDepthMode::XRay) WarnXRayOnce();
        if (Mode != EDebugDepthMode::Foreground) ++SceneDepthPrims;
        TriCmds.push_back({ A, B, C, Color, Mode });
    }

    void FDebugDraw::Icon(const Vec3& Center, f32 HalfSize, const Vec3& Color, u32 Type,
                          bool Selected) {
        const f32 TypeF = static_cast<f32>(Type);
        const f32 SelF  = Selected ? 1.0f : 0.0f;
        auto Push = [&](f32 Cx, f32 Cy) {
            IconVerts.push_back({ { Center.X, Center.Y, Center.Z },
                                  { Color.X, Color.Y, Color.Z },
                                  { Cx, Cy }, { HalfSize, TypeF, SelF } });
        };
        Push(-1.0f, -1.0f); Push(1.0f, -1.0f); Push(1.0f, 1.0f);
        Push(-1.0f, -1.0f); Push(1.0f,  1.0f); Push(-1.0f, 1.0f);
    }

    void FDebugDraw::BuildRootSignature(ID3D12Device* Device) {
        // b0 visivel em VS (matriz) e PS (params/bias do teste de depth); t0 = depth da cena
        // pro caminho ocluivel (os PSOs sem teste simplesmente nao referenciam a tabela).
        D3D12_ROOT_PARAMETER P[2]{};
        P[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        P[0].Descriptor.ShaderRegister = 0;
        P[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE DepthRange{};
        DepthRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DepthRange.NumDescriptors                    = 1;
        DepthRange.BaseShaderRegister                = 0;
        DepthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        P[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P[1].DescriptorTable.NumDescriptorRanges = 1;
        P[1].DescriptorTable.pDescriptorRanges   = &DepthRange;
        P[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC PointClamp{};
        PointClamp.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        PointClamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        PointClamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        PointClamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        PointClamp.MaxAnisotropy    = 1;
        PointClamp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        PointClamp.MinLOD           = 0.0f;
        PointClamp.MaxLOD           = D3D12_FLOAT32_MAX;
        PointClamp.ShaderRegister   = 0;
        PointClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(P);
        Desc.pParameters       = P;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &PointClamp;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("DebugDraw Root Sig: ") + static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                 IID_PPV_ARGS(&RootSig)));
    }

    void FDebugDraw::BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat) {
        auto VS    = LoadShaderBytecode("DebugDraw.vs_6_0.cso");
        auto PS    = LoadShaderBytecode("DebugDraw.ps_6_0.cso");
        auto PSOcc = LoadShaderBytecode("DebugDrawOccluded.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

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
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = RTFormat;
        PSODesc.SampleDesc            = { 1, 0 };

        // Dois eixos: topologia (linha/triangulo) x PS (Foreground = sem teste, Scene = teste
        // manual contra o depth da cena). Os 4 PSOs sao o produto dos dois.
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&LinePSO)));

        PSODesc.PS = { PSOcc.data(), PSOcc.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&LineScenePSO)));

        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TriScenePSO)));

        PSODesc.PS = { PS.data(), PS.size() };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TriPSO)));

        // Icones de luz: billboards alpha-blended com glifo SDF (layout de vertice proprio).
        auto IconVS = LoadShaderBytecode("LightIcon.vs_6_0.cso");
        auto IconPS = LoadShaderBytecode("LightIcon.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC IconLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_BLEND_DESC IconBlend{};
        IconBlend.RenderTarget[0].BlendEnable           = TRUE;
        IconBlend.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        IconBlend.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        IconBlend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        IconBlend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        IconBlend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        IconBlend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        IconBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        PSODesc.VS          = { IconVS.data(), IconVS.size() };
        PSODesc.PS          = { IconPS.data(), IconPS.size() };
        PSODesc.BlendState  = IconBlend;
        PSODesc.InputLayout = { IconLayout, _countof(IconLayout) };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&IconPSO)));
    }

    void FDebugDraw::CreateBuffers(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE NoRead{ 0, 0 };

        Desc.Width = 256ull * kFIF;
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));

        Desc.Width = static_cast<u64>(kMaxVerts) * kVBStride * kFIF;
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VB)));
        SMILE_HR(VB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedVB)));

        Desc.Width = static_cast<u64>(kMaxIconVerts) * sizeof(IconVertex) * kFIF;
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&IconVB)));
        SMILE_HR(IconVB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedIconVB)));
    }

    void FDebugDraw::Render(ID3D12GraphicsCommandList* CmdList, u32 FrameSlot, const Mat44& ViewProj,
                            D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV, u32 Width, u32 Height,
                            const Vec3& CamRight, const Vec3& CamUp,
                            D3D12_GPU_DESCRIPTOR_HANDLE DepthSRV) {
        if (!Initialized || Empty()) return;
        const u32 Slot = FrameSlot % kFIF;

        // Achatamento comando -> vertice, um bucket por PSO. O alpha da cor e o Thickness ficam
        // de fora: o vertice ainda e pos+rgb e o render e LINELIST de 1px. Guardar sem usar e o
        // que permite trocar so o renderizador na F1b, sem tocar em API nem em caller.
        // Sem SRV de depth nao ha como testar: os modos Scene/XRay caem fora neste frame. Isso
        // NAO conta como estouro de orcamento (e o fallback documentado), fica fora do aviso.
        const bool CanTestDepth = DepthSRV.ptr != 0;
        SceneLineVerts.clear(); FgLineVerts.clear();
        SceneTriVerts.clear();  FgTriVerts.clear();

        auto PushVert = [](std::vector<DDVertex>& Dst, const Vec3& P, const Vec4& C) {
            Dst.push_back({ { P.X, P.Y, P.Z }, { C.X, C.Y, C.Z } });
        };
        for (const FLineCmd& Cmd : LineCmds) {
            const bool TestsDepth = Cmd.Mode != EDebugDepthMode::Foreground;
            if (TestsDepth && !CanTestDepth) continue;
            auto& Dst = TestsDepth ? SceneLineVerts : FgLineVerts;
            PushVert(Dst, Cmd.A, Cmd.Color);
            PushVert(Dst, Cmd.B, Cmd.Color);
        }
        for (const FTriCmd& Cmd : TriCmds) {
            const bool TestsDepth = Cmd.Mode != EDebugDepthMode::Foreground;
            if (TestsDepth && !CanTestDepth) continue;
            auto& Dst = TestsDepth ? SceneTriVerts : FgTriVerts;
            PushVert(Dst, Cmd.A, Cmd.Color);
            PushVert(Dst, Cmd.B, Cmd.Color);
            PushVert(Dst, Cmd.C, Cmd.Color);
        }

        const u32 WantOcc  = static_cast<u32>(SceneLineVerts.size());
        const u32 WantLine = static_cast<u32>(FgLineVerts.size());
        const u32 WantSTri = static_cast<u32>(SceneTriVerts.size());
        const u32 WantTri  = static_cast<u32>(FgTriVerts.size());
        const u32 WantIcon = static_cast<u32>(IconVerts.size());

        // Clamp ALINHADO POR PRIMITIVA: linha precisa de pares, triangulo de trincas. Cortar no
        // meio nao explode (o IA descarta a primitiva incompleta), mas o debug aparece mordido
        // sem nada dizendo por que. Linha ja cai par (kMaxVerts par, pushes aos pares); o
        // AlignDown existe pro caso de kMaxVerts/orcamento mudarem — e o triangulo PRECISA dele.
        auto AlignDown = [](u32 V, u32 N) { return V - (V % N); };
        const u32 numOcc  = AlignDown(std::min(WantOcc, kMaxVerts), 2);
        const u32 numLine = AlignDown(std::min(WantLine, kMaxVerts - numOcc), 2);
        const u32 numSTri = AlignDown(std::min(WantSTri, kMaxVerts - numOcc - numLine), 3);
        const u32 numTri  = AlignDown(std::min(WantTri, kMaxVerts - numOcc - numLine - numSTri), 3);
        const u32 numIcon = AlignDown(std::min(WantIcon, kMaxIconVerts), 6);

        const u32 Dropped = (WantOcc - numOcc) + (WantLine - numLine) + (WantSTri - numSTri) +
                            (WantTri - numTri) + (WantIcon - numIcon);
        if (Dropped && !OverflowLogged) {
            OverflowLogged = true;
            LogWarning("DebugDraw estourou o orcamento: " + std::to_string(Dropped) +
                       " vertices descartados (linhas+triangulos " +
                       std::to_string(WantOcc + WantLine + WantSTri + WantTri) + "/" +
                       std::to_string(kMaxVerts) + ", icones " + std::to_string(WantIcon) +
                       "/" + std::to_string(kMaxIconVerts) +
                       "). O debug desenhado esta INCOMPLETO");
        } else if (!Dropped) {
            OverflowLogged = false; // coube de novo: rearma p/ o proximo episodio avisar
        }

        // VB do frame, na ordem de desenho: [linhas Scene | linhas Fg | tris Scene | tris Fg].
        // Quem testa depth vai primeiro e o Foreground por cima — dentro de cada grupo a ordem
        // de submissao decide, porque nao ha depth entre primitivas de debug (F1c resolve).
        u8* VBSlot = MappedVB + static_cast<size_t>(Slot) * kMaxVerts * kVBStride;
        u32 Cursor = 0;
        auto Upload = [&](const std::vector<DDVertex>& Src, u32 Count) {
            if (Count) std::memcpy(VBSlot + Cursor * kVBStride, Src.data(), Count * kVBStride);
            Cursor += Count;
        };
        Upload(SceneLineVerts, numOcc);
        Upload(FgLineVerts,    numLine);
        Upload(SceneTriVerts,  numSTri);
        Upload(FgTriVerts,     numTri);

        u8* IconSlot = MappedIconVB + static_cast<size_t>(Slot) * kMaxIconVerts * sizeof(IconVertex);
        if (numIcon) std::memcpy(IconSlot, IconVerts.data(), numIcon * sizeof(IconVertex));

        struct { Mat44 M; f32 Params[4]; f32 CamR[4]; f32 CamU[4]; } CBData;
        CBData.M = ViewProj;
        CBData.Params[0] = Width  > 0 ? 1.0f / static_cast<f32>(Width)  : 0.0f;
        CBData.Params[1] = Height > 0 ? 1.0f / static_cast<f32>(Height) : 0.0f;
        CBData.Params[2] = kDepthTestBiasNdc;
        CBData.Params[3] = 0.0f;
        CBData.CamR[0] = CamRight.X; CBData.CamR[1] = CamRight.Y; CBData.CamR[2] = CamRight.Z; CBData.CamR[3] = 0.0f;
        CBData.CamU[0] = CamUp.X;    CBData.CamU[1] = CamUp.Y;    CBData.CamU[2] = CamUp.Z;    CBData.CamU[3] = 0.0f;
        std::memcpy(MappedCB + static_cast<size_t>(Slot) * 256, &CBData, sizeof(CBData));

        CmdList->OMSetRenderTargets(1, &BackbufferRTV, FALSE, nullptr);
        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(Width); VP.Height = static_cast<FLOAT>(Height);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = static_cast<LONG>(Width); Sci.bottom = static_cast<LONG>(Height);
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        CmdList->SetGraphicsRootSignature(RootSig.Get());
        CmdList->SetGraphicsRootConstantBufferView(0, CB->GetGPUVirtualAddress() + static_cast<u64>(Slot) * 256);

        D3D12_VERTEX_BUFFER_VIEW VBV{};
        VBV.BufferLocation = VB->GetGPUVirtualAddress() + static_cast<u64>(Slot) * kMaxVerts * kVBStride;
        VBV.StrideInBytes  = kVBStride;
        VBV.SizeInBytes    = (numOcc + numLine + numSTri + numTri) * kVBStride;
        CmdList->IASetVertexBuffers(0, 1, &VBV);

        if (numOcc || numSTri) CmdList->SetGraphicsRootDescriptorTable(1, DepthSRV);
        u32 First = 0;
        auto Draw = [&](u32 Count, ID3D12PipelineState* PSO, D3D12_PRIMITIVE_TOPOLOGY Topo) {
            if (!Count) return;
            CmdList->SetPipelineState(PSO);
            CmdList->IASetPrimitiveTopology(Topo);
            CmdList->DrawInstanced(Count, 1, First, 0);
            First += Count;
        };
        Draw(numOcc,  LineScenePSO.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        Draw(numLine, LinePSO.Get(),      D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        Draw(numSTri, TriScenePSO.Get(),  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Draw(numTri,  TriPSO.Get(),       D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (numIcon) {
            D3D12_VERTEX_BUFFER_VIEW IconVBV{};
            IconVBV.BufferLocation = IconVB->GetGPUVirtualAddress() +
                                     static_cast<u64>(Slot) * kMaxIconVerts * sizeof(IconVertex);
            IconVBV.StrideInBytes  = sizeof(IconVertex);
            IconVBV.SizeInBytes    = numIcon * static_cast<u32>(sizeof(IconVertex));
            CmdList->IASetVertexBuffers(0, 1, &IconVBV);
            CmdList->SetPipelineState(IconPSO.Get());
            CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CmdList->DrawInstanced(numIcon, 1, 0, 0);
        }
    }
}
