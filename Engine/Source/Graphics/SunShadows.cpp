#include "Smile/Graphics/SunShadows.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
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

namespace Smile {
    void FSunShadows::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;
        CreateResources(_Device, _SRVHeap);
        BuildRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffers(_Device);
        Initialized = true;
        LogDebug("CSM (sombra do sol) inicializado: 4 cascatas 2048^2 D16 (32 MB)");
    }

    void FSunShadows::CreateResources(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = kResolution;
        Desc.Height           = kResolution;
        Desc.DepthOrArraySize = static_cast<UINT16>(kNumCascades);
        Desc.MipLevels        = 1;
        // D16 e o formato das tres referencias (a UE define PF_ShadowDepth como R16_TYPELESS,
        // a Flax escolhe D16_UNorm no primeiro suportado, a Cry usa D16 no cache). Metade da
        // VRAM: 4 x 2048^2 cai de 64 MB para 32 MB, e aparece na janela de Estatisticas.
        // Precisao: 65536 niveis sobre o range do ortho da cascata dao 1,95 mm na cascata 0 e
        // 3,3 cm na 3 — o bias da F2 vale 24 e 62 niveis respectivamente, folga de sobra. O
        // DepthBias do rasterizer e 0, entao a mudanca da unidade `r` do formato UNORM (que
        // multiplica so aquele termo) nao afeta nada; o slope bias e imune ao formato.
        Desc.Format           = DXGI_FORMAT_R16_TYPELESS;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format               = DXGI_FORMAT_D16_UNORM;
        Clear.DepthStencil.Depth   = 1.0f;
        Clear.DepthStencil.Stencil = 0;

        ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            ArrayState, &Clear, IID_PPV_ARGS(&DepthArray)));
        VramTracker::Register(DepthArray.Get(), EVramCategory::Shadows);

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
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        auto MakeBuffer = [&](UINT64 Width, Microsoft::WRL::ComPtr<ID3D12Resource>& Out, u8*& Mapped) {
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = Width;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&Out)));
            D3D12_RANGE NoRead{ 0, 0 };
            void* Ptr = nullptr;
            SMILE_HR(Out->Map(0, &NoRead, &Ptr));
            Mapped = reinterpret_cast<u8*>(Ptr);
        };

        MakeBuffer(static_cast<UINT64>(FCommandQueue::kFramesInFlight) * kNumCascades *
                       sizeof(ShadowCascadeConstants), CascadeCB, MappedCascade);
        MakeBuffer(static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(CSMConstants),
                   CSMCB, MappedCSM);

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
                                     const Vec3& _DirToSun, f32 _NearZ, f32 _NoiseFrame) {
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

        UpdateMask = 0;
        if (!_Enabled) {
            InvalidateCache(); // cena/estado podem mudar enquanto off; re-fita tudo ao religar
        } else {
            ++UpdateCounter;
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

                // Cascatas distantes cacheadas ganham 10% de folga na esfera: tolera o
                // drift da camera entre re-renderizacoes sem perder cobertura.
                constexpr f32 kCacheSlack = 1.10f;
                const bool Cacheable = CacheEnabled && c >= 2;
                const f32  radius = Cacheable ? std::ceil(idealRadius * kCacheSlack) : idealRadius;

                const Vec3 centerWorld = TransformPoint(InvView, Vec3{ 0.0f, 0.0f, czv });

                if (Cacheable && CacheValid[c]) {
                    // Round-robin defasado: c2 nos frames pares, c3 a cada 4 (fase 1) —
                    // nunca as duas no mesmo frame (estilo update-rate do Flax).
                    const u64 Period = (c == 2) ? 2u : 4u;
                    const u64 Phase  = (c == 2) ? 0u : 1u;
                    bool Refresh = (UpdateCounter % Period) == Phase;
                    // Sol girou alem de ~0.05 graus (TOD rapido derruba o cache, correto).
                    constexpr f32 kSunDirCos = 0.99999962f;
                    if (!Refresh && CachedFwd[c].Dot(fwd) < kSunDirCos) Refresh = true;
                    if (!Refresh) {
                        // Esfera ideal escapou da congelada (teleporte/voo rapido).
                        const Vec3 d{ centerWorld.X - CachedCenter[c].X,
                                      centerWorld.Y - CachedCenter[c].Y,
                                      centerWorld.Z - CachedCenter[c].Z };
                        const f32 slack = CachedRadius[c] - idealRadius;
                        if (slack < 0.0f || d.Dot(d) > slack * slack) Refresh = true;
                    }
                    if (!Refresh) continue; // congelada: WorldToShadow/texel do CB ficam como estao
                }

                const f32 texel = 2.0f * radius / static_cast<f32>(kResolution);
                const f32 cx = std::floor(centerWorld.Dot(right) / texel) * texel;
                const f32 cy = std::floor(centerWorld.Dot(up)    / texel) * texel;
                const f32 cf = centerWorld.Dot(fwd);
                const Vec3 snapped = right*cx + up*cy + fwd*cf;

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

                UpdateMask |= (1u << c);
                CacheValid[c]   = Cacheable;
                CachedFwd[c]    = fwd;
                CachedCenter[c] = snapped;
                CachedRadius[c] = radius;

                ShadowCascadeConstants Cascade{ LightViewProj };
                std::memcpy(MappedCascade +
                                (static_cast<size_t>(FrameSlot) * kNumCascades + c) *
                                    sizeof(ShadowCascadeConstants),
                            &Cascade, sizeof(ShadowCascadeConstants));
            }
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

    void FSunShadows::RecordDepthPass(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap,
                                      const FShadowDrawItem* _Items, size_t _Count,
                                      const FExtraCascadeDraw& _ExtraDraw,
                                      FGpuProfiler* _Profiler) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kResolution),
                           static_cast<FLOAT>(kResolution), 0.0f, 1.0f };
        D3D12_RECT     SC{ 0, 0, static_cast<LONG>(kResolution), static_cast<LONG>(kResolution) };
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &SC);

        static constexpr const char* CascadeScopes[kNumCascades] = {
            "Cascata 0", "Cascata 1", "Cascata 2", "Cascata 3" };
        for (u32 c = 0; c < kNumCascades; ++c) {
            if (!(UpdateMask & (1u << c))) continue; // cascata congelada: depth do update anterior segue valido
            if (_Profiler) _Profiler->Begin(_CommandList, CascadeScopes[c]);
            // Root sig por cascata (nao uma vez fora do loop): o ExtraDraw (terreno) troca
            // root signature/PSO no fim da cascata anterior.
            _CommandList->SetGraphicsRootSignature(RootSig.Get());
            auto DSV = DSVHeap.CpuHandle(c);
            _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
            _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            _CommandList->SetGraphicsRootConstantBufferView(0, CascadeCBAddr(c));

            // Planos da fatia, montados no UpdatePerFrame (ver a nota longa la). Nao saem
            // mais da matriz do ortho: aquela era a caixa da esfera de fitting e nao cortava
            // nada nas cascatas distantes. Sem o plano near — com pancaking (depth clip off)
            // casters atras do near ainda projetam sombra, achatados nele.
            const Vec4* Planes = CullPlanes[c];
            auto Outside = [&](const Vec3& Mn, const Vec3& Mx) -> bool {
                for (int i = 0; i < 5; ++i) {
                    const Vec4& p = Planes[i];
                    const f32 px = (p.X >= 0.0f) ? Mx.X : Mn.X;
                    const f32 py = (p.Y >= 0.0f) ? Mx.Y : Mn.Y;
                    const f32 pz = (p.Z >= 0.0f) ? Mx.Z : Mn.Z;
                    if (p.X*px + p.Y*py + p.Z*pz + p.W < 0.0f) return true;
                }
                return false;
            };

            // Caster menor que N texels da cascata nao contribui sombra legivel — pula
            // (corta micro-objetos das cascatas distantes, estilo min caster size da Cry/UE).
            const f32 MinExtent = MinCasterTexels * (&CPUConstants.CascadeTexelWorld.X)[c];

            // Cur/LastMat zeram por cascata de propósito: o SetGraphicsRootSignature no topo
            // do laco invalida as descriptor tables ligadas, entao o primeiro material da
            // cascata precisa religar mesmo que seja o mesmo do fim da cascata anterior.
            ID3D12PipelineState* Cur     = nullptr;
            const FMaterial*     LastMat = nullptr;
            for (size_t k = 0; k < _Count; ++k) {
                const FShadowDrawItem& It = _Items[k];
                if (!It.Mesh) continue;
                if (MinExtent > 0.0f) {
                    const f32 ExX = It.AABBMax.X - It.AABBMin.X;
                    const f32 ExY = It.AABBMax.Y - It.AABBMin.Y;
                    const f32 ExZ = It.AABBMax.Z - It.AABBMin.Z;
                    f32 MaxExt = ExX > ExY ? ExX : ExY;
                    if (ExZ > MaxExt) MaxExt = ExZ;
                    if (MaxExt < MinExtent) continue;
                }
                if (Outside(It.AABBMin, It.AABBMax)) continue;
                const bool AlphaTested = It.Mat && (It.Mat->Constants.AlphaTest != 0);
                ID3D12PipelineState* Want = AlphaTested ? MaskedPSO.Get() : OpaquePSO.Get();
                if (Want != Cur) { _CommandList->SetPipelineState(Want); Cur = Want; }
                _CommandList->SetGraphicsRootConstantBufferView(3, It.ObjectCB);
                // A lista chega ordenada por (alpha-test, material) do Renderer, entao os
                // itens que compartilham material sao adjacentes e o Bind repetido some.
                if (AlphaTested && It.Mat != LastMat) {
                    It.Mat->Bind(_CommandList, _SRVHeap);
                    LastMat = It.Mat;
                }
                It.Mesh->Draw(_CommandList);
            }

            if (_ExtraDraw)
                _ExtraDraw(_CommandList, c, CascadeCBAddr(c), CascadeViewProj[c]);
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
}
