#include "Smile/Graphics/DebugDraw.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <algorithm>
#include <string>
#include <iterator>

namespace Smile {
    static constexpr u32 kFIF = FCommandQueue::kFramesInFlight;
    // Bias em NDC do teste manual de depth das linhas ocluiveis: sem ele, linha encostada numa
    // superficie serrilha (o fragmento e a cena caem no mesmo depth e o resultado vira ruido).
    // Empirico, calibrado com o wire do volume de luz colado no chao.
    static constexpr f32 kDepthTestBiasNdc = 2e-5f;

    // RESSALVA CONHECIDA: depth WRITE e alpha nao se dao bem, e o overlay escreve depth em tudo
    // menos no icone. Duas consequencias, ambas so DENTRO DE UM MESMO GRUPO de modo:
    //   (a) a franja de AA da linha escreve depth como qualquer pixel, entao um fragmento de
    //       ~30% de cobertura bloqueia o que vier atras;
    //   (b) primitiva translucida (XRay, ou qualquer cor com alpha < 1) tambem grava o DSV: se a
    //       mais PERTO for submetida primeiro, ela elimina as de tras em vez de compor; na ordem
    //       inversa as duas compoem. Ou seja, translucido depende da ordem de submissao.
    // Nenhuma das duas afeta o que existe hoje — o gizmo e opaco e a franja e de 1px. A saida
    // certa e a mesma pros dois casos (ordenar de tras pra frente e/ou prepass de depth com
    // alpha test), e ela se paga quando Scene/XRay ganharem consumidores de verdade. Ate la,
    // fica registrado e nao esquecido.

    void FDebugDraw::Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat) {
        if (Initialized) return;
        Device_   = Device;
        RTFormat_ = RTFormat;
        BuildRootSignature(Device);
        BuildPSOs(Device, RTFormat);
        CreateBuffers(Device);
        Initialized = true;
        LogDebug("DebugDraw Inicializado");
    }

    void FDebugDraw::EnsureOverlayDepth(u32 Width, u32 Height) {
        if (!Device_ || Width == 0 || Height == 0) return;
        if (OverlayDepth && OverlayWidth == Width && OverlayHeight == Height) return;

        if (OverlayDSVHeap.Native() == nullptr)
            OverlayDSVHeap.Initialize(Device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = Width;
        Desc.Height           = Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = kOverlayDepthFormat;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format               = kOverlayDepthFormat;
        Clear.DepthStencil.Depth   = kClearDepth;
        Clear.DepthStencil.Stencil = 0;

        // Sai do escopo antes de criar o novo: o Renderer da Flush na fila antes do resize, entao
        // ninguem esta lendo o antigo.
        OverlayDepth.Reset();
        SMILE_HR(Device_->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE, &Clear, IID_PPV_ARGS(&OverlayDepth)));
        VramTracker::Register(OverlayDepth.Get(), EVramCategory::Misc);

        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = kOverlayDepthFormat;
        DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        Device_->CreateDepthStencilView(OverlayDepth.Get(), &DSVDesc,
                                        OverlayDSVHeap.CpuHandle(0));

        OverlayWidth  = Width;
        OverlayHeight = Height;
    }

    void FDebugDraw::RecreatePSOs(ID3D12Device* Device) {
        if (!Initialized) return;
        // Quem chama ja deu Flush na fila (ReloadShaders). So os PSOs: a root signature nao muda
        // com o shader, e os buffers nao tem nada a ver com isso.
        BuildPSOs(Device, RTFormat_);
    }

    u32 FDebugDraw::PackColor(const Vec4& C) {
        auto Byte = [](f32 V) -> u32 {
            return static_cast<u32>(std::clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return Byte(C.X) | (Byte(C.Y) << 8) | (Byte(C.Z) << 16) | (Byte(C.W) << 24);
    }

    void FDebugDraw::Line(const Vec3& A, const Vec3& B, const Vec4& Color,
                          EDebugDepthMode Mode, f32 Thickness) {
        if (Mode != EDebugDepthMode::Foreground) ++SceneDepthPrims;
        LineCmds.push_back({ A, B, Color, Thickness, Mode });
    }

    void FDebugDraw::Triangle(const Vec3& A, const Vec3& B, const Vec3& C, const Vec4& Color,
                              EDebugDepthMode Mode) {
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
        // b0 (por frame) visivel em VS (matriz, tamanho de tela) e PS (params/bias); t0 = depth
        // da cena pros PSOs que testam depth (os outros nao referenciam a tabela); b1 (por draw)
        // = root constants com o OccludedAlpha, que e a UNICA diferenca entre Scene e XRay.
        D3D12_ROOT_PARAMETER P[3]{};
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

        P[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        P[2].Constants.ShaderRegister = 1;
        P[2].Constants.Num32BitValues = 4;
        P[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

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
        auto TriVS       = LoadShaderBytecode("DebugDraw.vs_6_0.cso");
        auto TriPS       = LoadShaderBytecode("DebugDraw.ps_6_0.cso");
        auto TriDepthPS  = LoadShaderBytecode("DebugDrawOccluded.ps_6_0.cso");
        auto LineVS      = LoadShaderBytecode("DebugDrawLine.vs_6_0.cso");
        auto LinePS      = LoadShaderBytecode("DebugDrawLine.ps_6_0.cso");
        auto LineDepthPS = LoadShaderBytecode("DebugDrawLineDepth.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC TriLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        // Linha: TUDO por instancia (um segmento = uma instancia). Os 4 cantos do quad saem do
        // SV_VertexID no VS, entao nao ha stream por vertice nenhum.
        D3D12_INPUT_ELEMENT_DESC LineLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        // Depth do OVERLAY (buffer proprio, limpo entre grupos): serve pro debug se ordenar
        // contra SI MESMO. Sem ele, a ponta do gizmo vista quase de frente vira um bloco
        // chapado — as 4 faces da piramide se sobrepondo na ordem de submissao.
        // WRITE_MASK_ALL vale tambem pro XRay e pra qualquer cor com alpha < 1 — ver a RESSALVA
        // CONHECIDA no topo do arquivo antes de dar translucido a um consumidor novo.
        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = kDepthFuncLessEqual;

        // Alpha blend em TUDO agora: e o que faz o AA da borda da linha e o XRay existirem.
        // Com alpha 1 no miolo, o pixel do centro sai igual ao do caminho opaco antigo.
        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].BlendEnable           = TRUE;
        Blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        Blend.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        Blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        Blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = RTFormat;
        PSODesc.DSVFormat             = kOverlayDepthFormat;
        PSODesc.SampleDesc            = { 1, 0 };

        // Dois eixos: primitiva (linha instanciada / triangulo) x PS (Foreground = sem teste,
        // Depth = teste manual contra o depth da cena, com OccludedAlpha decidindo Scene vs
        // XRay). Cada PSO diz TODO o seu estado — nada de herdar campo do anterior em silencio.
        auto Make = [&](const std::vector<u8>& VS, const std::vector<u8>& PS,
                        D3D12_INPUT_ELEMENT_DESC* Layout, u32 LayoutCount,
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE Topo, ID3D12PipelineState** Out) {
            PSODesc.VS                    = { VS.data(), VS.size() };
            PSODesc.PS                    = { PS.data(), PS.size() };
            PSODesc.InputLayout           = { Layout, LayoutCount };
            PSODesc.PrimitiveTopologyType = Topo;
            SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(Out)));
        };
        Make(LineVS, LinePS,      LineLayout, _countof(LineLayout),
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &LinePSO);
        Make(LineVS, LineDepthPS, LineLayout, _countof(LineLayout),
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &LineDepthPSO);
        Make(TriVS,  TriPS,       TriLayout,  _countof(TriLayout),
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &TriPSO);
        Make(TriVS,  TriDepthPS,  TriLayout,  _countof(TriLayout),
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &TriDepthPSO);

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

        // Icone fica FORA do depth do overlay — nem testa nem escreve. E politica, nao descuido:
        // billboard alpha-blended nao se resolve com depth (escrever chapa o alpha da borda no
        // buffer e esconder o que vem depois; testar exige ordenar por distancia, que ninguem
        // faz ainda). Ate haver sort de transparencia, icone e a camada de cima e ponto.
        D3D12_DEPTH_STENCIL_DESC IconDepth{};
        IconDepth.DepthEnable    = FALSE;
        IconDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        PSODesc.BlendState        = IconBlend;
        PSODesc.DepthStencilState = IconDepth;
        Make(IconVS, IconPS, IconLayout, _countof(IconLayout),
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &IconPSO);
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

        Desc.Width = static_cast<u64>(kMaxLines) * kLineStride * kFIF;
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&LineVB)));
        SMILE_HR(LineVB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedLineVB)));

        Desc.Width = static_cast<u64>(kMaxTriVerts) * kVBStride * kFIF;
        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TriVB)));
        SMILE_HR(TriVB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedTriVB)));

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

        // Achatamento comando -> dado de GPU, um bucket por modo. Linha vira UMA INSTANCIA
        // (A, B, cor, espessura) e o VS gera os 4 cantos do quad; triangulo segue vertice a
        // vertice. Sem SRV de depth nao ha como testar: os modos Scene/XRay caem fora neste
        // frame — isso NAO e estouro de orcamento (e o fallback documentado), fica fora do aviso.
        const bool CanTestDepth = DepthSRV.ptr != 0;
        for (auto& B : LineBuckets) B.clear();
        for (auto& B : TriBuckets)  B.clear();

        auto BucketOf = [&](EDebugDepthMode Mode) { return static_cast<size_t>(Mode); };
        auto Drops    = [&](EDebugDepthMode Mode) {
            return Mode != EDebugDepthMode::Foreground && !CanTestDepth;
        };
        for (const FLineCmd& Cmd : LineCmds) {
            if (Drops(Cmd.Mode)) continue;
            LineBuckets[BucketOf(Cmd.Mode)].push_back({
                { Cmd.A.X, Cmd.A.Y, Cmd.A.Z }, { Cmd.B.X, Cmd.B.Y, Cmd.B.Z },
                PackColor(Cmd.Color), Cmd.Thickness });
        }
        for (const FTriCmd& Cmd : TriCmds) {
            if (Drops(Cmd.Mode)) continue;
            auto& Dst = TriBuckets[BucketOf(Cmd.Mode)];
            const u32 Packed = PackColor(Cmd.Color);
            Dst.push_back({ { Cmd.A.X, Cmd.A.Y, Cmd.A.Z }, Packed });
            Dst.push_back({ { Cmd.B.X, Cmd.B.Y, Cmd.B.Z }, Packed });
            Dst.push_back({ { Cmd.C.X, Cmd.C.Y, Cmd.C.Z }, Packed });
        }

        // Orcamento: linha conta em INSTANCIAS (nao precisa alinhar — uma instancia e uma
        // primitiva inteira); triangulo conta em vertices e PRECISA cair em multiplo de 3, senao
        // o corte deixa uma primitiva pela metade e o debug aparece mordido sem explicacao.
        // A prioridade segue a MESMA ordem do upload/draw: uma ordem so na funcao.
        auto AlignDown = [](u32 V, u32 N) { return V - (V % N); };
        u32 LineCount[kModeCount]{}, TriCount[kModeCount]{};
        u32 LineBudget = kMaxLines, TriBudget = kMaxTriVerts, Dropped = 0;
        for (u32 i = 0; i < kModeCount; ++i) {
            const size_t M = BucketOf(kDrawOrder[i]);
            const u32 WantL = static_cast<u32>(LineBuckets[M].size());
            const u32 WantT = static_cast<u32>(TriBuckets[M].size());
            LineCount[M] = std::min(WantL, LineBudget);
            TriCount[M]  = AlignDown(std::min(WantT, TriBudget), 3);
            LineBudget -= LineCount[M];
            TriBudget  -= TriCount[M];
            Dropped += (WantL - LineCount[M]) + (WantT - TriCount[M]);
        }
        const u32 WantIcon = static_cast<u32>(IconVerts.size());
        const u32 numIcon  = AlignDown(std::min(WantIcon, kMaxIconVerts), 6);
        Dropped += WantIcon - numIcon;

        if (Dropped && !OverflowLogged) {
            OverflowLogged = true;
            LogWarning("DebugDraw estourou o orcamento: " + std::to_string(Dropped) +
                       " primitivas/vertices descartados (linhas " +
                       std::to_string(LineCmds.size()) + "/" + std::to_string(kMaxLines) +
                       ", vertices de triangulo " + std::to_string(TriCmds.size() * 3) + "/" +
                       std::to_string(kMaxTriVerts) + ", icones " + std::to_string(WantIcon) +
                       "/" + std::to_string(kMaxIconVerts) +
                       "). O debug desenhado esta INCOMPLETO");
        } else if (!Dropped) {
            OverflowLogged = false; // coube de novo: rearma p/ o proximo episodio avisar
        }

        // Buffers do frame na ordem de desenho (kDrawOrder): TODO o Scene, depois XRay, depois
        // Foreground. Agrupar por MODO e nao por topologia e o que sustenta a regra "Foreground
        // fica por cima" — intercalar deixaria um triangulo Scene sobrescrever uma linha
        // Foreground desenhada antes. DENTRO de cada grupo quem ordena e o depth do overlay
        // (limpo a cada grupo, ver o Draw mais abaixo), entao a ordem de submissao so decide
        // empate exato de profundidade.
        u8* LineSlot = MappedLineVB + static_cast<size_t>(Slot) * kMaxLines * kLineStride;
        u8* TriSlot  = MappedTriVB  + static_cast<size_t>(Slot) * kMaxTriVerts * kVBStride;
        u32 LineFirst[kModeCount]{}, TriFirst[kModeCount]{};
        u32 LineCursor = 0, TriCursor = 0;
        for (u32 i = 0; i < kModeCount; ++i) {
            const size_t M = BucketOf(kDrawOrder[i]);
            LineFirst[M] = LineCursor;
            TriFirst[M]  = TriCursor;
            if (LineCount[M])
                std::memcpy(LineSlot + LineCursor * kLineStride, LineBuckets[M].data(),
                            LineCount[M] * kLineStride);
            if (TriCount[M])
                std::memcpy(TriSlot + TriCursor * kVBStride, TriBuckets[M].data(),
                            TriCount[M] * kVBStride);
            LineCursor += LineCount[M];
            TriCursor  += TriCount[M];
        }

        u8* IconSlot = MappedIconVB + static_cast<size_t>(Slot) * kMaxIconVerts * sizeof(IconVertex);
        if (numIcon) std::memcpy(IconSlot, IconVerts.data(), numIcon * sizeof(IconVertex));

        struct { Mat44 M; f32 Params[4]; f32 CamR[4]; f32 CamU[4]; f32 Screen[4]; } CBData;
        CBData.M = ViewProj;
        CBData.Params[0] = Width  > 0 ? 1.0f / static_cast<f32>(Width)  : 0.0f;
        CBData.Params[1] = Height > 0 ? 1.0f / static_cast<f32>(Height) : 0.0f;
        CBData.Params[2] = kDepthTestBiasNdc;
        CBData.Params[3] = 0.0f;
        CBData.CamR[0] = CamRight.X; CBData.CamR[1] = CamRight.Y; CBData.CamR[2] = CamRight.Z; CBData.CamR[3] = 0.0f;
        CBData.CamU[0] = CamUp.X;    CBData.CamU[1] = CamUp.Y;    CBData.CamU[2] = CamUp.Z;    CBData.CamU[3] = 0.0f;
        // Tamanho em pixels: o VS da linha converte a espessura de px pra NDC com isto.
        CBData.Screen[0] = static_cast<f32>(Width);  CBData.Screen[1] = static_cast<f32>(Height);
        CBData.Screen[2] = 0.0f;                     CBData.Screen[3] = 0.0f;
        std::memcpy(MappedCB + static_cast<size_t>(Slot) * 256, &CBData, sizeof(CBData));

        EnsureOverlayDepth(Width, Height);
        const bool HasOverlayDepth = OverlayDepth != nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE OverlayDSV = HasOverlayDepth
            ? OverlayDSVHeap.CpuHandle(0) : D3D12_CPU_DESCRIPTOR_HANDLE{};

        CmdList->OMSetRenderTargets(1, &BackbufferRTV, FALSE,
                                    HasOverlayDepth ? &OverlayDSV : nullptr);
        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(Width); VP.Height = static_cast<FLOAT>(Height);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sci{}; Sci.right = static_cast<LONG>(Width); Sci.bottom = static_cast<LONG>(Height);
        CmdList->RSSetViewports(1, &VP);
        CmdList->RSSetScissorRects(1, &Sci);

        CmdList->SetGraphicsRootSignature(RootSig.Get());
        CmdList->SetGraphicsRootConstantBufferView(0, CB->GetGPUVirtualAddress() + static_cast<u64>(Slot) * 256);

        if (CanTestDepth) CmdList->SetGraphicsRootDescriptorTable(1, DepthSRV);

        D3D12_VERTEX_BUFFER_VIEW LineVBV{};
        LineVBV.BufferLocation = LineVB->GetGPUVirtualAddress() +
                                 static_cast<u64>(Slot) * kMaxLines * kLineStride;
        LineVBV.StrideInBytes  = kLineStride;
        LineVBV.SizeInBytes    = LineCursor * kLineStride;

        D3D12_VERTEX_BUFFER_VIEW TriVBV{};
        TriVBV.BufferLocation = TriVB->GetGPUVirtualAddress() +
                                static_cast<u64>(Slot) * kMaxTriVerts * kVBStride;
        TriVBV.StrideInBytes  = kVBStride;
        TriVBV.SizeInBytes    = TriCursor * kVBStride;

        // O quad da linha e um triangle STRIP de 4 vertices gerados no VS: 4 vertices x N
        // instancias, sem VB por vertice. Triangulo continua TRIANGLELIST comum.
        ID3D12PipelineState* BoundPSO = nullptr;
        for (u32 i = 0; i < kModeCount; ++i) {
            const EDebugDepthMode Mode = kDrawOrder[i];
            const size_t M = BucketOf(Mode);
            if (!LineCount[M] && !TriCount[M]) continue;

            // Depth do overlay LIMPO A CADA GRUPO. E o que concilia duas regras que brigariam:
            // dentro do grupo a geometria se ordena em 3D (a piramide da ponta vira ponta), mas
            // o grupo seguinte continua desenhando POR CIMA do anterior — Foreground nao pode
            // ser ocluido por uma linha Scene que por acaso esta mais perto. Mesma ideia do
            // SDPG_World/SDPG_Foreground da Unreal, que tambem sao grupos de depth separados.
            if (HasOverlayDepth)
                CmdList->ClearDepthStencilView(OverlayDSV, D3D12_CLEAR_FLAG_DEPTH,
                                               kClearDepth, 0, 0, nullptr);

            // OccludedAlpha e a UNICA diferenca entre Scene e XRay — mesmo PSO, root constant
            // diferente. Foreground nem le (o PS dele nao tem o caminho de depth).
            const f32 Occluded = OccludedAlphaFor(Mode);
            CmdList->SetGraphicsRoot32BitConstants(2, 1, &Occluded, 0);
            const bool TestsDepth = Mode != EDebugDepthMode::Foreground;

            if (LineCount[M]) {
                ID3D12PipelineState* PSO = TestsDepth ? LineDepthPSO.Get() : LinePSO.Get();
                if (PSO != BoundPSO) { CmdList->SetPipelineState(PSO); BoundPSO = PSO; }
                CmdList->IASetVertexBuffers(0, 1, &LineVBV);
                CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                CmdList->DrawInstanced(4, LineCount[M], 0, LineFirst[M]);
            }
            if (TriCount[M]) {
                ID3D12PipelineState* PSO = TestsDepth ? TriDepthPSO.Get() : TriPSO.Get();
                if (PSO != BoundPSO) { CmdList->SetPipelineState(PSO); BoundPSO = PSO; }
                CmdList->IASetVertexBuffers(0, 1, &TriVBV);
                CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                CmdList->DrawInstanced(TriCount[M], 1, TriFirst[M], 0);
            }
        }
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

    FPassShaderStems FDebugDraw::ShaderStems() const {
        static const char* const kStems[] = { "DebugDraw.vs", "DebugDraw.ps", "DebugDrawOccluded.ps", "DebugDrawLine.vs", "DebugDrawLine.ps", "DebugDrawLineDepth.ps", "LightIcon.vs", "LightIcon.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FDebugDraw::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) RecreatePSOs(_Ctx.Device);
    }

}
