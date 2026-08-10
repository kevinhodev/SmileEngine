#include "Smile/Graphics/SunShadows.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <iterator>

namespace Smile {
    void FSunShadows::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;
        CreateResources(_Device, _SRVHeap);
        BuildRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffers(_Device);
        Initialized = true;
        LogDebug("CSM (sombra do sol) inicializado: 4 cascatas 2048^2 D16, runtime + cache "
                 "estatico (64 MB)");
    }

    void FSunShadows::CreateResources(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        // O comentario do formato mora aqui porque a escolha e do PASSE, nao da fabrica:
        // D16 e o formato das tres referencias (a UE define PF_ShadowDepth como R16_TYPELESS,
        // a Flax escolhe D16_UNorm no primeiro suportado, a Cry usa D16 no cache). Metade da
        // VRAM: 4 x 2048^2 cai de 64 MB para 32 MB, e aparece na janela de Estatisticas.
        // Precisao: 65536 niveis sobre o range do ortho da cascata dao 1,95 mm na cascata 0 e
        // 3,3 cm na 3 — o bias da F2 vale 24 e 62 niveis respectivamente, folga de sobra. O
        // DepthBias do rasterizer e 0, entao a mudanca da unidade `r` do formato UNORM (que
        // multiplica so aquele termo) nao afeta nada; o slope bias e imune ao formato.
        D3D12_CLEAR_VALUE Clear{};
        Clear.Format               = DXGI_FORMAT_D16_UNORM;
        Clear.DepthStencil.Depth   = 1.0f;
        Clear.DepthStencil.Stencil = 0;

        ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        DepthArray = GpuResources::CreateTex2D(
            _Device, kResolution, kResolution, DXGI_FORMAT_R16_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, ArrayState, EVramCategory::Shadows,
            &Clear, 1, kNumCascades, "CSM · runtime");

        DSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kNumCascades, false);
        for (u32 c = 0; c < kNumCascades; ++c) {
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format                         = DXGI_FORMAT_D16_UNORM;
            DSVDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            DSVDesc.Texture2DArray.MipSlice        = 0;
            DSVDesc.Texture2DArray.FirstArraySlice = c;
            DSVDesc.Texture2DArray.ArraySize       = 1;
            _Device->CreateDepthStencilView(DepthArray.Get(), &DSVDesc, DSVHeap.CpuHandle(c));
        }

        // Gemeo estatico: mesma desc, mesmo clear, DSV por fatia. Nao ganha SRV — nada o
        // amostra, ele so alimenta o array de runtime por copia.
        StaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        StaticArray = GpuResources::CreateTex2D(
            _Device, kResolution, kResolution, DXGI_FORMAT_R16_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, StaticState, EVramCategory::Shadows,
            &Clear, 1, kNumCascades, "CSM · cache estatico");

        StaticDSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kNumCascades, false);
        for (u32 c = 0; c < kNumCascades; ++c) {
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format                         = DXGI_FORMAT_D16_UNORM;
            DSVDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            DSVDesc.Texture2DArray.MipSlice        = 0;
            DSVDesc.Texture2DArray.FirstArraySlice = c;
            DSVDesc.Texture2DArray.ArraySize       = 1;
            _Device->CreateDepthStencilView(StaticArray.Get(), &DSVDesc, StaticDSVHeap.CpuHandle(c));
        }

        ShadowSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                          = DXGI_FORMAT_R16_UNORM;
        SRVDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        SRVDesc.Texture2DArray.MostDetailedMip  = 0;
        SRVDesc.Texture2DArray.MipLevels        = 1;
        SRVDesc.Texture2DArray.FirstArraySlice  = 0;
        SRVDesc.Texture2DArray.ArraySize        = kNumCascades;
        _SRVHeap.CreateSRV(_Device, DepthArray.Get(), SRVDesc, ShadowSRVSlot_);
    }

    void FSunShadows::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE MatRange{};
        MatRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        MatRange.NumDescriptors                    = 8; 
        MatRange.BaseShaderRegister                = 0;
        MatRange.RegisterSpace                     = 0;
        MatRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[4]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; 
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        RootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[1].Descriptor.ShaderRegister = 1; 
        RootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &MatRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[3].Descriptor.ShaderRegister = 2; 
        RootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_ANISOTROPIC;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Sampler.MaxAnisotropy    = 8;
        Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Sampler.MinLOD           = 0.0f;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderRegister   = 0; 
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Shadow root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FSunShadows::BuildPSOs(ID3D12Device* _Device) {
        auto VS = LoadShaderBytecode("ShadowDepth.vs_6_0.cso");
        auto PS = LoadShaderBytecode("ShadowDepth.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        // Shadow casters are treated as two-sided occluders. Several Bistro interior shells are
        // single-sided with normals facing inward; backface-culling them in the light view lets
        // sunlight pass through walls even though the RT/DDGI path can still hit those triangles.
        Raster.CullMode              = D3D12_CULL_MODE_NONE;
        Raster.FrontCounterClockwise = FALSE;
        // Shadow pancaking: casters entre o sol e o near plane do ortho seriam clipados
        // (vazando luz com sol baixo); com depth clip off o hardware clampa a profundidade
        // deles pra 0, achatando-os no near plane — mesma solucao do Flax (PSO de shadow
        // depth) e equivalente ao clamp de VS da Unreal (bClampToNearPlane).
        Raster.DepthClipEnable       = FALSE;
        Raster.DepthBias             = 0;
        Raster.SlopeScaledDepthBias  = 1.0f;
        // TETO do slope bias. No D3D12 um DepthBiasClamp de 0 significa SEM CLAMP, e o termo
        // do slope e SlopeScaledDepthBias * max(|dz/dx|, |dz/dy|): num poligono quase paralelo
        // a direcao da luz esse maximo explode e o caster e empurrado para longe da superficie,
        // vazando luz. A doc da MS descreve o caso ("pushes the polygon extremely far away").
        // Todas as referencias clampam: a UE em r.Shadow.ShadowMaxSlopeScaleDepthBias = 1.0
        // (ela nem usa slope bias de hardware — faz analitico no VS com tan(theta) clampado),
        // a Cry em fDepthBiasClamp = 0.001 no raster E fSlopeClamp = 0.001 no PS. O valor
        // abaixo e o da Cry; e um TETO, nao o bias tipico (o constante do shader e 6e-4).
        Raster.DepthBiasClamp        = 0.001f;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { nullptr, 0 }; 
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 0; 
        PSODesc.DSVFormat             = DXGI_FORMAT_D16_UNORM;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OpaquePSO)));

        Raster.CullMode         = D3D12_CULL_MODE_NONE;
        PSODesc.RasterizerState = Raster;
        PSODesc.PS              = { PS.data(), PS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&MaskedPSO)));
    }

    void FSunShadows::CreateConstantBuffers(ID3D12Device* _Device) {
        auto MakeBuffer = [&](u64 SliceBytes, u32 SliceCount,
                              Microsoft::WRL::ComPtr<ID3D12Resource>& Out, u8*& Mapped) {
            const GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(_Device, SliceBytes, SliceCount);
            Out    = Upload.Resource;
            Mapped = Upload.Mapped;
        };

        // Os dois indexadores usam sizeof() como passo (ver o memcpy do CSM logo abaixo e o
        // CascadeCBAddr), entao o passo tem que ser 256-alinhado — que e o que o root CBV
        // exige do endereco de cada slot.
        static_assert(sizeof(ShadowCascadeConstants) % 256 == 0 &&
                      sizeof(CSMConstants) % 256 == 0,
                      "passo do CB do CSM tem que ser 256-alinhado");

        MakeBuffer(sizeof(ShadowCascadeConstants),
                   FCommandQueue::kFramesInFlight * kNumCascades, CascadeCB, MappedCascade);
        MakeBuffer(sizeof(CSMConstants),
                   FCommandQueue::kFramesInFlight, CSMCB, MappedCSM);

        CPUConstants.Params  = { static_cast<f32>(kNumCascades), DepthBiasTexels,
                                 1.0f / static_cast<f32>(kResolution), 0.0f };
        CPUConstants.Params2 = { NormalOffsetTexels, PcfRadiusTexels,
                                 std::clamp(BlendBand, 0.0f, 0.5f),
                                 DebugCascades ? 1.0f : 0.0f };
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedCSM + static_cast<size_t>(i) * sizeof(CSMConstants),
                        &CPUConstants, sizeof(CSMConstants));
    }

    void FSunShadows::UpdatePerFrame(u32 _FrameSlot, bool _Enabled, const Mat44& _View,
                                     const Vec3& _CamPos, f32 _FovYRadians, f32 _Aspect,
                                     const Vec3& _DirToSun, f32 _NearZ, f32 _NoiseFrame,
                                     u64 _StaticCastersVersion) {
        FrameSlot = _FrameSlot;
        CPUConstants.Params  = { static_cast<f32>(kNumCascades), DepthBiasTexels,
                                 1.0f / static_cast<f32>(kResolution), _Enabled ? 1.0f : 0.0f };
        const f32 TransitionFraction = std::clamp(BlendBand, 0.0f, 0.5f);
        CPUConstants.Params2 = { NormalOffsetTexels, PcfRadiusTexels, TransitionFraction,
                                 DebugCascades ? 1.0f : 0.0f };
        const f32 PcssTan = SunAngularSizeDeg > 0.0f
            ? std::tan(0.5f * SunAngularSizeDeg * 3.14159265f / 180.0f) : 0.0f;
        CPUConstants.Params3 = { _NoiseFrame, PcssTan, MaxPenumbraTexels, 0.0f };

        // Direcao PARA a key light (o Renderer ja alterna sol->lua de noite). O normal offset
        // do receptor escala por sin(alfa) = sqrt(1 - N.L^2) e precisa dela por pixel.
        const Vec3 ToLight = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 1.0f, 0.0f });
        CPUConstants.SunDirection = { ToLight.X, ToLight.Y, ToLight.Z, 0.0f };

        // O UpdateMask do frame ANTERIOR: quem o preenche e o RecordDepthPass (so la se sabe
        // se houve dinamico a desenhar), que roda depois daqui. O historico o consome com um
        // frame de atraso, o que numa janela de 64 nao muda leitura nenhuma.
        const u32 PrevUpdateMask = UpdateMask;
        UpdateMask = 0;
        StaticRefreshMask = 0;
        if (!_Enabled) {
            InvalidateCache(); // cena/estado podem mudar enquanto off; re-fita tudo ao religar
        } else {
            ++UpdateCounter;

            // Velocidade angular do sol NESTE frame. Se ela sozinha ja estoura a tolerancia
            // mais frouxa, nenhum mapa estatico chega inteiro ao proximo frame — manter o
            // gemeo seria pagar rasterizacao E copia para jogar fora. Nesse caso o passe volta
            // a desenhar tudo direto no alvo de runtime, que e o custo de antes do cache: a
            // degradacao e para IGUAL, nao para pior. Um ciclo dia/noite normal nem chega
            // perto disto — a 24 min por dia o sol anda 0,004 graus por frame.
            {
                const Vec3 f{ -_DirToSun.X, -_DirToSun.Y, -_DirToSun.Z };
                const Vec3 fN = f.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                // A cascata 3 e a mais tolerante (maior razao texel/range), entao e ela que
                // define "rapido demais para qualquer uma".
                const f32 dr3 = CPUConstants.DepthRangeWorld.W;
                const f32 sf3 = CPUConstants.CascadeTexelWorld.W;
                const f32 MaxTol = (dr3 > 0.0f && sf3 > 0.0f)
                    ? SunToleranceTexels * sf3 / dr3 : 1.0f;
                CacheBypass = HasLastSunFwd &&
                              LastFrameSunFwd.Dot(fN) < std::cos(std::min(MaxTol, 1.0f));
                LastFrameSunFwd = fN;
                HasLastSunFwd   = true;
            }
            if (CacheBypass) InvalidateCache();
            auto Accum = [](f32 E, int Idx, int Count) -> f32 {
                f32 Cur = 1.0f, Total = 0.0f, Ret = 0.0f;
                for (int i = 0; i < Count; ++i) { if (i < Idx) Ret += Cur; Total += Cur; Cur *= E; }
                return Total > 0.0f ? Ret / Total : 0.0f;
            };
            const int   numC = static_cast<int>(kNumCascades);
            const f32   maxD = ShadowMaxDistance;
            f32 Splits[kNumCascades + 1];
            for (int i = 0; i <= numC; ++i)
                Splits[i] = _NearZ + Accum(DistributionExponent, i, numC) * (maxD - _NearZ);

            CPUConstants.CascadeSplits = { Splits[1], Splits[2], Splits[3], Splits[4] };
            CPUConstants.CameraPosition = { _CamPos.X, _CamPos.Y, _CamPos.Z, 0.0f };

            const f32 tanV = std::tan(_FovYRadians * 0.5f);
            const f32 tanH = tanV * _Aspect;
            const Mat44 InvView = _View.Inverse();
            const Vec3 CameraForward{
                InvView.M[2][0], InvView.M[2][1], InvView.M[2][2]
            };
            const Vec3 CameraForwardN = CameraForward.NormalizedSafe(Vec3{ 0.0f, 0.0f, 1.0f });
            CPUConstants.CameraForwardNear = {
                CameraForwardN.X, CameraForwardN.Y, CameraForwardN.Z, _NearZ
            };

            auto TransformPoint = [](const Mat44& M, const Vec3& p) -> Vec3 {
                return Vec3{
                    p.X*M.M[0][0] + p.Y*M.M[1][0] + p.Z*M.M[2][0] + M.M[3][0],
                    p.X*M.M[0][1] + p.Y*M.M[1][1] + p.Z*M.M[2][1] + M.M[3][1],
                    p.X*M.M[0][2] + p.Y*M.M[1][2] + p.Z*M.M[2][2] + M.M[3][2] };
            };

            const Vec3 fwd = Vec3{ -_DirToSun.X, -_DirToSun.Y, -_DirToSun.Z }
                                 .NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
            const Vec3 up0   = (std::fabs(fwd.Y) > 0.99f) ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                          : Vec3{ 0.0f, 1.0f, 0.0f };
            const Vec3 right = up0.Cross(fwd).Normalized();
            const Vec3 up    = fwd.Cross(right);

            Mat44 BiasUV = Mat44::Identity();
            BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
            BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;

            f32* sf = &CPUConstants.CascadeTexelWorld.X;
            f32* dr = &CPUConstants.DepthRangeWorld.X;
            for (int c = 0; c < numC; ++c) {
                const f32 splitNear = Splits[c];
                const f32 df = Splits[c + 1];
                // O crossfade ocorre ANTES do split. Portanto a cascata seguinte precisa
                // conter fisicamente esse trecho da cascata atual; depender apenas da
                // sobreposicao acidental das esferas reintroduz uma borda em FOVs largos.
                const f32 previousSpan = c > 0 ? (Splits[c] - Splits[c - 1]) : 0.0f;
                const f32 dn = c > 0
                    ? std::max(_NearZ, splitNear - previousSpan * TransitionFraction)
                    : splitNear;
                const f32 FarX = tanH*df, FarY = tanV*df, NearX = tanH*dn, NearY = tanV*dn;
                const f32 diagFar2 = FarX*FarX + FarY*FarY, diagNear2 = NearX*NearX + NearY*NearY;
                const f32 len = df - dn;
                f32 offset = (diagNear2 - diagFar2) / (2.0f * len) + len * 0.5f;
                f32 czv = df - offset;
                if (czv < dn) czv = dn; if (czv > df) czv = df;

                f32 r2 = 0.0f;
                const f32 cz[2] = { dn, df }, ex[2] = { NearX, FarX }, ey[2] = { NearY, FarY };
                for (int pl = 0; pl < 2; ++pl)
                    for (int sx = -1; sx <= 1; sx += 2)
                        for (int sy = -1; sy <= 1; sy += 2) {
                            const f32 x = sx*ex[pl], y = sy*ey[pl], z = cz[pl] - czv;
                            const f32 d = x*x + y*y + z*z;
                            if (d > r2) r2 = d;
                        }
                f32 idealRadius = std::ceil(std::sqrt(r2));
                if (idealRadius < 1.0f) idealRadius = 1.0f;

                // Folga na esfera SO nas cascatas distantes. Nas proximas ela custaria
                // resolucao — 10% de raio a mais e 10% de texel a mais — e nao compra nada que
                // importe: com o centro snapado na grade de texels, camera parada ja produz
                // fit IDENTICO, que foi o que a medida da F0 mostrou (delta = 0 texels nas
                // quatro). Quem resolve camera em MOVIMENTO na cascata proxima e a F3
                // (scrolling ou ancora de mundo), nao folga de esfera.
                constexpr f32 kCacheSlack = 1.10f;
                const bool HasSlack  = c >= 2;
                const bool Cacheable = CacheEnabled && !CacheBypass;
                const f32  radius = HasSlack ? std::ceil(idealRadius * kCacheSlack) : idealRadius;

                const Vec3 centerWorld = TransformPoint(InvView, Vec3{ 0.0f, 0.0f, czv });

                const f32 texel = 2.0f * radius / static_cast<f32>(kResolution);
                const f32 cx = std::floor(centerWorld.Dot(right) / texel) * texel;
                const f32 cy = std::floor(centerWorld.Dot(up)    / texel) * texel;
                const f32 cf = centerWorld.Dot(fwd);
                const Vec3 snapped = right*cx + up*cy + fwd*cf;

                // Deslocamento do fit em texels desta cascata (ver FCascadeStats). Medido
                // contra o fit que gerou o mapa que esta la, e ANTES da decisao de reuso —
                // cascata que reusa tem delta 0 por construcao, e e isso que se quer ver.
                Stats[c].SnapDeltaTexels = HasPrevSnap[c]
                    ? std::max(std::fabs(cx - PrevSnapR[c]), std::fabs(cy - PrevSnapU[c])) / texel
                    : 0.0f;
                PrevSnapR[c] = cx; PrevSnapU[c] = cy; HasPrevSnap[c] = true;

                if (Cacheable && CacheValid[c]) {
                    // (1) O FIT congelado ainda cobre o que precisa cobrir? Duas politicas,
                    // deliberadamente diferentes. Com folga, vale enquanto a esfera ideal
                    // couber DENTRO da congelada — tolera a camera andar sem refazer nada. Sem
                    // folga, vale enquanto o fit recalculado der exatamente os mesmos numeros,
                    // isto e, enquanto a matriz for a mesma; qualquer deslocamento que cruze um
                    // texel derruba. Comparacao exata de float e o que se quer aqui: os dois
                    // lados saem do mesmo `floor`, entao ou sao bit a bit iguais ou o mapa
                    // realmente descreve outro recorte.
                    bool FitOk;
                    if (HasSlack) {
                        const Vec3 d{ centerWorld.X - CachedCenter[c].X,
                                      centerWorld.Y - CachedCenter[c].Y,
                                      centerWorld.Z - CachedCenter[c].Z };
                        const f32 slack = CachedRadius[c] - idealRadius;
                        FitOk = slack >= 0.0f && d.Dot(d) <= slack * slack;
                    } else {
                        FitOk = radius == CachedRadius[c] &&
                                snapped.X == CachedCenter[c].X &&
                                snapped.Y == CachedCenter[c].Y &&
                                snapped.Z == CachedCenter[c].Z;
                    }

                    // (2) O sol girou o bastante para a foto ser outra? Tolerancia derivada
                    // por cascata: o deslocamento da sombra vale range * delta, entao o mesmo
                    // angulo custa 4,8 texels na cascata 0 e 1,9 na 3. Um limiar unico em
                    // graus protegeria a distante demais e a proxima de menos.
                    const f32 dr = (&CPUConstants.DepthRangeWorld.X)[c];
                    const f32 SunTol = dr > 0.0f
                        ? std::cos(std::min(SunToleranceTexels * texel / dr, 1.0f)) : 1.0f;
                    const bool SunOk = CachedFwd[c].Dot(fwd) >= SunTol;

                    // (3) O conteudo estatico mudou? Este e o sinal que a F1 criou e que
                    // substitui o redesenho periodico "por garantia" que morava aqui.
                    const bool ContentOk = CachedContentVersion[c] == _StaticCastersVersion;

                    if (FitOk && SunOk && ContentOk)
                        continue; // reusa: WorldToShadow/texel/range do CB ficam como estao

                    // A fila (ver StaticRefreshMask) so vale quando o UNICO motivo e o sol.
                    //
                    // Sol desatualiza de forma graciosa: a sombra fica alguns frames com a
                    // direcao velha e ninguem enxerga. As outras duas nao. Fit invalido e
                    // cobertura — a cascata deixa de conter o que precisa sombrear, e o
                    // sintoma e buraco. E conteudo invalido inclui o caso que mais aparece:
                    // ao soltar o gizmo o objeto sai do conjunto dinamico e passa a depender
                    // do mapa estatico, do qual ele foi retirado no inicio do arraste — adiar
                    // aqui e a sombra dele SUMIR pelos frames de espera, na hora exata em que
                    // o usuario esta olhando para ela.
                    if (FitOk && ContentOk && (UpdateCounter % kNumCascades) != c)
                        continue;
                }

                const Mat44 LightView = Mat44::LookAtLH(snapped, snapped + fwd, up0);
                const Mat44 LightProj = Mat44::OrthographicLH(2.0f*radius, 2.0f*radius,
                                                              -(radius + CasterPullback), radius);
                const Mat44 LightViewProj = LightView * LightProj;

                CPUConstants.WorldToShadow[c] = LightViewProj * BiasUV;
                CascadeViewProj[c]            = LightViewProj;
                sf[c] = texel;
                dr[c] = 2.0f * radius + CasterPullback; // range do ortho em mundo (PCSS)

                // VOLUME DE CULLING da cascata, em planos de MUNDO. Antes ele saia da matriz
                // do ortho, que e a caixa da esfera de FITTING — muito maior que a fatia do
                // frustum que ela precisa cobrir: de 24x a 46x o volume, com lado de ate
                // 2076 m na cascata 3. Numa cena do tamanho da Bistro os planos laterais nao
                // cortavam absolutamente nada, e so o filtro de caster pequeno filtrava.
                //
                // Os laterais agora vem da extensao REAL da fatia em espaco de luz, o que e
                // exato: um caster shadow-eia um receptor apenas se estiver sobre o segmento
                // que vai do receptor ate o sol, logo precisa compartilhar a coordenada
                // (right, up) dele. Quem esta fora da extensao da fatia nao pode sombrear
                // ninguem dentro dela, por mais longe que esteja na direcao da luz. A UE faz
                // o mesmo em ComputeShadowCullingVolume, com o hull-silhueta, que e ainda
                // mais apertado que esta AABB. O plano near segue fora (pancaking).
                {
                    f32 minR =  3.4e38f, maxR = -3.4e38f;
                    f32 minU =  3.4e38f, maxU = -3.4e38f;
                    for (int pl = 0; pl < 2; ++pl)
                        for (int sx = -1; sx <= 1; sx += 2)
                            for (int sy = -1; sy <= 1; sy += 2) {
                                const Vec3 w = TransformPoint(
                                    InvView, Vec3{ sx * ex[pl], sy * ey[pl], cz[pl] });
                                const f32 rr = w.Dot(right), uu = w.Dot(up);
                                minR = std::min(minR, rr); maxR = std::max(maxR, rr);
                                minU = std::min(minU, uu); maxU = std::max(maxU, uu);
                            }

                    // Folga: normal-offset (ate 2,5 texels) e kernel do PCF (5x5 = 2 texels)
                    // deslocam o lookup do receptor alguns texels do ponto geometrico, entao
                    // um caster logo fora da fatia ainda pode cair nesses taps.
                    const f32 margin = 8.0f * texel;
                    minR -= margin; maxR += margin;
                    minU -= margin; maxU += margin;

                    Vec4* P = CullPlanes[c];
                    P[0] = {  right.X,  right.Y,  right.Z, -minR };
                    P[1] = { -right.X, -right.Y, -right.Z,  maxR };
                    P[2] = {  up.X,     up.Y,     up.Z,    -minU };
                    P[3] = { -up.X,    -up.Y,    -up.Z,     maxU };
                    P[4] = { -fwd.X,   -fwd.Y,   -fwd.Z,    cf + radius }; // far do ortho
                }

                StaticRefreshMask |= (1u << c);
                CacheValid[c]   = Cacheable;
                CachedFwd[c]    = fwd;
                CachedCenter[c] = snapped;
                CachedRadius[c] = radius;
                CachedContentVersion[c] = _StaticCastersVersion;
            }
        }

        // Matriz de cascata no CB do slot ATUAL, para TODA cascata, refazendo o fit ou nao.
        //
        // O CB e por frame-in-flight, mas CascadeViewProj[] e retido: escrever so no caminho
        // que refaz o fit deixava o slot alternado com a matriz de um fit ANTIGO. Enquanto a
        // cascata congelada nao era desenhada isso era inofensivo — ninguem lia. A partir do
        // momento em que os dinamicos passaram a ser rasterizados sobre a cascata que REUSA o
        // mapa estatico, esse endereco voltou a ser lido, e com a matriz errada: um unico
        // caster projetado por um fit alheio cobre uma area enorme do shadow map com
        // profundidade proxima, e a cena inteira atras dele le como sombreada. Como o slot
        // alterna a cada frame, o defeito pisca — foi o "a sombra fica se mexendo".
        for (u32 c = 0; c < kNumCascades; ++c) {
            ShadowCascadeConstants Cascade{ CascadeViewProj[c] };
            std::memcpy(MappedCascade +
                            (static_cast<size_t>(FrameSlot) * kNumCascades + c) *
                                sizeof(ShadowCascadeConstants),
                        &Cascade, sizeof(ShadowCascadeConstants));
        }

        // Bias por cascata, resolvido aqui e nao no shader. O bias precisa ser constante em
        // TEXELS da cascata, senao a proxima cascata (texel 4x maior) fica sub-biasada e a
        // anterior super-biasada; com um escalar unico em NDC isso valia 3,28 texels na
        // cascata 0 contra 1,28 na 3. Convertendo por cascata,
        //     ndc = texels * texelWorld[c] / rangeWorld[c],
        // o resultado e o mesmo numero de texels em todas — a identidade que a Unreal obtem
        // com ShadowCascadeBiasDistribution = 1 e a Cry com e_ShadowsAutoBias.
        //
        // FORA do laco de propósito: cascata congelada pelo cache faz `continue` e nao
        // reescreve texel/range, mas os valores retidos no CB continuam validos e sao
        // exatamente os que descrevem o mapa que ainda esta la.
        {
            const f32* sfAll = &CPUConstants.CascadeTexelWorld.X;
            const f32* drAll = &CPUConstants.DepthRangeWorld.X;
            f32*       bias  = &CPUConstants.BiasNdc.X;
            for (u32 c = 0; c < kNumCascades; ++c) {
                bias[c] = drAll[c] > 0.0f
                    ? DepthBiasTexels * CascadeBiasScale[c] * (sfAll[c] / drAll[c])
                    : 0.0f;
            }
        }

        // Historico de atualizacao, deslocado uma vez por frame para TODA cascata — a
        // congelada grava 0. E aqui, e nao no RecordDepthPass, porque o UpdatePerFrame roda
        // mesmo com a sombra desligada e mesmo em frame sem passe de profundidade; ancorar a
        // janela de 64 no frame mantem o popcount comparavel entre cascatas.
        for (u32 c = 0; c < kNumCascades; ++c) {
            const bool Updated  = (PrevUpdateMask & (1u << c)) != 0;
            const bool Refresh  = (StaticRefreshMask & (1u << c)) != 0;
            Stats[c].UpdatedThisFrame = Refresh;
            Stats[c].UpdateHistory    = (Stats[c].UpdateHistory << 1) | (Updated ? 1ull : 0ull);
            Stats[c].StaticHistory    = (Stats[c].StaticHistory << 1) | (Refresh ? 1ull : 0ull);
        }

        std::memcpy(MappedCSM + static_cast<size_t>(FrameSlot) * sizeof(CSMConstants),
                    &CPUConstants, sizeof(CSMConstants));
    }

    void FSunShadows::TransitionArray(ID3D12GraphicsCommandList* _CommandList,
                                      D3D12_RESOURCE_STATES _After) {
        if (ArrayState == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = DepthArray.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = ArrayState;
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        ArrayState = _After;
    }

    void FSunShadows::TransitionStatic(ID3D12GraphicsCommandList* _CommandList,
                                       D3D12_RESOURCE_STATES _After) {
        if (StaticState == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = StaticArray.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = StaticState;
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        StaticState = _After;
    }

    bool FSunShadows::CasterVisible(const FShadowDrawItem& _It, u32 _Cascade, f32 _MinExtent,
                                    FCascadeStats* _St) const {
        if (!_It.Mesh) return false;
        // Caster menor que N texels da cascata nao contribui sombra legivel (min caster size
        // da Cry/UE). Vem antes dos planos porque e mais barato e, nas cascatas distantes, e o
        // filtro que corta quase tudo — na 3 sao 669 de 1542 contra 0 dos planos.
        if (_MinExtent > 0.0f) {
            const f32 ExX = _It.AABBMax.X - _It.AABBMin.X;
            const f32 ExY = _It.AABBMax.Y - _It.AABBMin.Y;
            const f32 ExZ = _It.AABBMax.Z - _It.AABBMin.Z;
            f32 MaxExt = ExX > ExY ? ExX : ExY;
            if (ExZ > MaxExt) MaxExt = ExZ;
            if (MaxExt < _MinExtent) { if (_St) ++_St->CulledSize; return false; }
        }
        // Planos da fatia, montados no UpdatePerFrame (ver a nota longa la). Nao saem da
        // matriz do ortho: aquela e a caixa da esfera de fitting e nao corta nada nas cascatas
        // distantes. Sem o plano near — com pancaking (depth clip off) casters atras do near
        // ainda projetam sombra, achatados nele.
        const Vec4* Planes = CullPlanes[_Cascade];
        for (int i = 0; i < 5; ++i) {
            const Vec4& p = Planes[i];
            const f32 px = (p.X >= 0.0f) ? _It.AABBMax.X : _It.AABBMin.X;
            const f32 py = (p.Y >= 0.0f) ? _It.AABBMax.Y : _It.AABBMin.Y;
            const f32 pz = (p.Z >= 0.0f) ? _It.AABBMax.Z : _It.AABBMin.Z;
            if (p.X*px + p.Y*py + p.Z*pz + p.W < 0.0f) {
                if (_St) ++_St->CulledPlanes;
                return false;
            }
        }
        return true;
    }

    u32 FSunShadows::DrawCasters(ID3D12GraphicsCommandList* _CommandList,
                                 FTextureSRVHeap& _SRVHeap, const FShadowDrawItem* _Items,
                                 size_t _Begin, size_t _End, u32 _Cascade, FCascadeStats* _St) {
        const f32 MinExtent = MinCasterTexels * (&CPUConstants.CascadeTexelWorld.X)[_Cascade];
        // Cur/LastMat zeram por CHAMADA de proposito: entre duas fases o chamador troca de DSV
        // e re-liga a root signature, o que invalida as descriptor tables — o primeiro
        // material precisa religar mesmo que seja o mesmo do fim da fase anterior.
        ID3D12PipelineState* Cur     = nullptr;
        const FMaterial*     LastMat = nullptr;
        u32 Drawn = 0;
        for (size_t k = _Begin; k < _End; ++k) {
            const FShadowDrawItem& It = _Items[k];
            if (!CasterVisible(It, _Cascade, MinExtent, _St)) continue;
            ++Drawn;
            const bool AlphaTested = It.Mat && (It.Mat->Constants.AlphaTest != 0);
            ID3D12PipelineState* Want = AlphaTested ? MaskedPSO.Get() : OpaquePSO.Get();
            if (Want != Cur) { _CommandList->SetPipelineState(Want); Cur = Want; }
            _CommandList->SetGraphicsRootConstantBufferView(3, It.ObjectCB);
            // A lista chega ordenada por (mobilidade, alpha-test, material) do Renderer, entao
            // os itens que compartilham material sao adjacentes e o Bind repetido some.
            if (AlphaTested && It.Mat != LastMat) {
                It.Mat->Bind(_CommandList, _SRVHeap);
                LastMat = It.Mat;
            }
            It.Mesh->Draw(_CommandList);
        }
        return Drawn;
    }

    void FSunShadows::RecordDepthPass(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap,
                                      const FShadowDrawItem* _Items, size_t _Count,
                                      const FExtraCascadeDraw& _ExtraDraw,
                                      FGpuProfiler* _Profiler) {
        D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kResolution),
                           static_cast<FLOAT>(kResolution), 0.0f, 1.0f };
        D3D12_RECT     SC{ 0, 0, static_cast<LONG>(kResolution), static_cast<LONG>(kResolution) };
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &SC);

        LastCasterCount = static_cast<u32>(_Count);

        // Fronteira estatico/dinamico. A lista chega ordenada com mobilidade como chave
        // primaria, entao ela e um unico ponto de corte, achado por busca linear a partir do
        // fim — o normal e haver pouquissimo dinamico, e nesse caso o laco para de imediato.
        size_t FirstDynamic = _Count;
        while (FirstDynamic > 0 && _Items[FirstDynamic - 1].Dynamic) --FirstDynamic;

        static constexpr const char* CascadeScopes[kNumCascades] = {
            "Cascata 0", "Cascata 1", "Cascata 2", "Cascata 3" };

        // ---- Caminho sem cache -------------------------------------------------------
        // Sol rapido demais (ver CacheBypass) ou cache desligado: manter o gemeo estatico so
        // pagaria rasterizacao e copia para jogar fora no frame seguinte. Rasteriza tudo
        // direto no alvo de runtime, que e EXATAMENTE o passe de antes do cache — a
        // degradacao e para igual, nunca para pior.
        if (!CacheEnabled || CacheBypass) {
            TransitionArray(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            for (u32 c = 0; c < kNumCascades; ++c) {
                if (_Profiler) _Profiler->Begin(_CommandList, CascadeScopes[c]);
                if (!(StaticRefreshMask & (1u << c))) {
                    if (_Profiler) _Profiler->End(_CommandList);
                    continue;
                }
                FCascadeStats& St = Stats[c];
                St.SubmittedStatic = 0; St.SubmittedDynamic = 0;
                St.CulledPlanes = 0; St.CulledSize = 0;

                _CommandList->SetGraphicsRootSignature(RootSig.Get());
                auto DSV = DSVHeap.CpuHandle(c);
                _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
                _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                _CommandList->SetGraphicsRootConstantBufferView(0, CascadeCBAddr(c));

                // Duas chamadas e nao uma sobre a lista inteira: o total nao diria quanto e
                // dinamico, que e a medida que interessa. Sao contiguas, entao nao ha custo.
                St.SubmittedStatic  = DrawCasters(_CommandList, _SRVHeap, _Items,
                                                  0, FirstDynamic, c, &St);
                St.SubmittedDynamic = DrawCasters(_CommandList, _SRVHeap, _Items,
                                                  FirstDynamic, _Count, c, nullptr);
                St.Submitted        = St.SubmittedStatic + St.SubmittedDynamic;
                LastDynamicCount[c] = St.SubmittedDynamic;
                if (_ExtraDraw)
                    _ExtraDraw(_CommandList, c, CascadeCBAddr(c), CascadeViewProj[c]);
                if (_Profiler) _Profiler->End(_CommandList);
            }
            UpdateMask = StaticRefreshMask;
            TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return;
        }

        // ---- Caminho com cache -------------------------------------------------------
        // Quantos dinamicos cada cascata desenharia. Precisa vir antes das barreiras porque e
        // o que decide se a cascata tem QUALQUER trabalho: com zero dinamicos agora e zero no
        // frame anterior, o alvo de runtime ja esta correto e nem a copia acontece. E o caso
        // da cena inteiramente estatica, onde o passe passa a custar zero.
        u32 DynVisible[kNumCascades] = {};
        for (u32 c = 0; c < kNumCascades; ++c) {
            const f32 MinExtent = MinCasterTexels * (&CPUConstants.CascadeTexelWorld.X)[c];
            for (size_t k = FirstDynamic; k < _Count; ++k)
                if (CasterVisible(_Items[k], c, MinExtent, nullptr)) ++DynVisible[c];
        }

        u32 CopyMask = 0;
        UpdateMask = 0;
        for (u32 c = 0; c < kNumCascades; ++c) {
            const bool StaticRefresh = (StaticRefreshMask & (1u << c)) != 0;
            // Copia quando: o mapa estatico foi refeito (o runtime tem de recebe-lo), ha
            // dinamico agora (o runtime precisa voltar a ser so o estatico antes de recebe-lo)
            // ou HAVIA dinamico antes (apagar de onde ele estava).
            if (StaticRefresh || DynVisible[c] > 0 || LastDynamicCount[c] > 0)
                CopyMask |= (1u << c);
            if (CopyMask & (1u << c)) UpdateMask |= (1u << c);
        }

        // FASE A — rasteriza o mapa estatico das cascatas que invalidaram.
        if (StaticRefreshMask) {
            TransitionStatic(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            for (u32 c = 0; c < kNumCascades; ++c) {
                if (!(StaticRefreshMask & (1u << c))) continue;
                FCascadeStats& St = Stats[c];
                St.SubmittedStatic = 0; St.CulledPlanes = 0; St.CulledSize = 0;

                _CommandList->SetGraphicsRootSignature(RootSig.Get());
                auto DSV = StaticDSVHeap.CpuHandle(c);
                _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
                _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                _CommandList->SetGraphicsRootConstantBufferView(0, CascadeCBAddr(c));

                St.SubmittedStatic = DrawCasters(_CommandList, _SRVHeap, _Items,
                                                 0, FirstDynamic, c, &St);
                // O terreno e caster estatico por definicao e entra AQUI, no mapa cacheado.
                // Antes ele redesenhava em toda cascata que acordava — e nas cascatas 2 e 3 da
                // Bistro ele e praticamente o unico conteudo do mapa.
                if (_ExtraDraw)
                    _ExtraDraw(_CommandList, c, CascadeCBAddr(c), CascadeViewProj[c]);
            }
        }

        // FASE B — copia estatico -> runtime. Mesmo formato e mesmas dimensoes, entao e uma
        // copia de subrecurso exata: sem shader, sem reamostragem, sem erro de profundidade.
        if (CopyMask) {
            TransitionStatic(_CommandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionArray(_CommandList, D3D12_RESOURCE_STATE_COPY_DEST);
            for (u32 c = 0; c < kNumCascades; ++c) {
                if (!(CopyMask & (1u << c))) continue;
                D3D12_TEXTURE_COPY_LOCATION Dst{};
                Dst.pResource        = DepthArray.Get();
                Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Dst.SubresourceIndex = c; // 1 mip: subrecurso == fatia
                D3D12_TEXTURE_COPY_LOCATION Src{};
                Src.pResource        = StaticArray.Get();
                Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Src.SubresourceIndex = c;
                _CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            }
        }

        // FASE C — dinamicos por cima, no alvo de runtime. Sem clear: o depth que veio da
        // copia e o do estatico, e e contra ele que o teste LESS tem de rodar.
        //
        // A transicao para DEPTH_WRITE so acontece se houver dinamico a desenhar. Com a cena
        // toda estatica sao duas barreiras por frame que nao protegem escrita nenhuma, e o
        // ponto desse caminho e justamente custar zero nesse caso.
        u32 AnyDynamic = 0;
        for (u32 c = 0; c < kNumCascades; ++c) AnyDynamic |= DynVisible[c];
        if (AnyDynamic) TransitionArray(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        for (u32 c = 0; c < kNumCascades; ++c) {
            // O escopo abre SEMPRE, inclusive na cascata que nao fez nada. Antes ele ficava
            // depois do `continue` e a linha sumia da janela nos frames em que o cache pegava:
            // um ranking que pisca nao da para ler, e "0,00 ms" e o que se quer ver.
            if (_Profiler) _Profiler->Begin(_CommandList, CascadeScopes[c]);
            FCascadeStats& St = Stats[c];
            St.SubmittedDynamic = 0;
            if (DynVisible[c] > 0) {
                _CommandList->SetGraphicsRootSignature(RootSig.Get());
                auto DSV = DSVHeap.CpuHandle(c);
                _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
                _CommandList->SetGraphicsRootConstantBufferView(0, CascadeCBAddr(c));
                St.SubmittedDynamic = DrawCasters(_CommandList, _SRVHeap, _Items,
                                                  FirstDynamic, _Count, c, nullptr);
            }
            // Recomposto do retido, nunca acumulado: SubmittedStatic so muda quando o mapa
            // estatico e refeito, e somar o dinamico nele cresceria sem limite.
            St.Submitted = St.SubmittedStatic + St.SubmittedDynamic;
            LastDynamicCount[c] = St.SubmittedDynamic;
            if (_Profiler) _Profiler->End(_CommandList);
        }

        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void FSunShadows::EnsureReadable(ID3D12GraphicsCommandList* _CommandList) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void FSunShadows::EnsureReadableCompute(ID3D12GraphicsCommandList* _CommandList) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    FPassShaderStems FSunShadows::ShaderStems() const {
        static const char* const kStems[] = { "ShadowDepth.vs", "ShadowDepth.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FSunShadows::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSOs(_Ctx.Device);
    }

}
