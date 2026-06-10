#include "Smile/Graphics/SunShadows.h"
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

namespace Smile {
    void FSunShadows::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;
        CreateResources(_Device, _SRVHeap);
        BuildRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffers(_Device);
        Initialized = true;
        LogInfo("CSM (sombra do sol) inicializado: 4 cascatas 2048^2");
    }

    void FSunShadows::CreateResources(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        // Texture2DArray de depth: R32_TYPELESS -> DSV (D32_FLOAT) por fatia + 1 SRV array.
        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = kResolution;
        Desc.Height           = kResolution;
        Desc.DepthOrArraySize = static_cast<UINT16>(kNumCascades);
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE Clear{};
        Clear.Format               = DXGI_FORMAT_D32_FLOAT;
        Clear.DepthStencil.Depth   = 1.0f;
        Clear.DepthStencil.Stencil = 0;

        ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            ArrayState, &Clear, IID_PPV_ARGS(&DepthArray)));

        // DSV heap: 1 view por fatia (FirstArraySlice = c).
        DSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kNumCascades, false);
        for (u32 c = 0; c < kNumCascades; ++c) {
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
            DSVDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            DSVDesc.Texture2DArray.MipSlice        = 0;
            DSVDesc.Texture2DArray.FirstArraySlice = c;
            DSVDesc.Texture2DArray.ArraySize       = 1;
            _Device->CreateDepthStencilView(DepthArray.Get(), &DSVDesc, DSVHeap.CpuHandle(c));
        }

        // SRV array (R32_FLOAT) cobrindo as kNumCascades fatias — 1 descritor (t11).
        ShadowSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                          = DXGI_FORMAT_R32_FLOAT;
        SRVDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        SRVDesc.Texture2DArray.MostDetailedMip  = 0;
        SRVDesc.Texture2DArray.MipLevels        = 1;
        SRVDesc.Texture2DArray.FirstArraySlice  = 0;
        SRVDesc.Texture2DArray.ArraySize        = kNumCascades;
        _SRVHeap.CreateSRV(_Device, DepthArray.Get(), SRVDesc, ShadowSRVSlot_);
    }

    void FSunShadows::BuildRootSignature(ID3D12Device* _Device) {
        // Layout casado com FMaterial::Bind (params 1 e 2) p/ reusar o bind do material:
        //   [0] CBV b0  ShadowCascadeCB (LightViewProj)  (VS)
        //   [1] CBV b1  MaterialConstants (alpha-test)    (PS)
        //   [2] SRV t0-t7  texturas do material            (PS)
        //   [3] CBV b2  ObjectConstants (Model)            (VS)
        //   Static sampler s0  anisotropic wrap            (PS)
        D3D12_DESCRIPTOR_RANGE MatRange{};
        MatRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        MatRange.NumDescriptors                    = 8; // t0..t7
        MatRange.BaseShaderRegister                = 0;
        MatRange.RegisterSpace                     = 0;
        MatRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[4]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; // b0
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        RootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[1].Descriptor.ShaderRegister = 1; // b1
        RootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &MatRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[3].Descriptor.ShaderRegister = 2; // b2
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
        Sampler.ShaderRegister   = 0; // s0
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

        // Bias de slope no rasterizer (mata acne em superfícies inclinadas; o PCF do PS
        // soma um bias constante NDC). Tunáveis de calibração.
        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        Raster.CullMode              = D3D12_CULL_MODE_BACK;
        Raster.FrontCounterClockwise = FALSE;
        Raster.DepthClipEnable       = TRUE;
        // O normal-offset (no PS) faz o grosso do anti-acne → rasterizer só com leve slope
        // bias (acne em superfícies muito inclinadas), sem const bias (evita peter-panning).
        Raster.DepthBias             = 0;
        Raster.SlopeScaledDepthBias  = 1.0f;
        Raster.DepthBiasClamp        = 0.0f;

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
        PSODesc.PS                    = { nullptr, 0 }; // opaco: só depth (sem PS)
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 0; // depth-only
        PSODesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OpaquePSO)));

        // Masked (folhagem): PS faz o alpha-test (clip); two-sided -> cull NONE.
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

        // Popula todas as regioes do CSM CB (qualquer slot valido antes do 1o UpdatePerFrame).
        CPUConstants.Params  = { static_cast<f32>(kNumCascades), DepthBias,
                                 1.0f / static_cast<f32>(kResolution), 0.0f };
        CPUConstants.Params2 = { NormalOffsetTexels, PcfRadiusTexels, BlendBand,
                                 DebugCascades ? 1.0f : 0.0f };
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedCSM + static_cast<size_t>(i) * sizeof(CSMConstants),
                        &CPUConstants, sizeof(CSMConstants));
    }

    void FSunShadows::UpdatePerFrame(u32 _FrameSlot, bool _Enabled, const Mat44& _View,
                                     const Vec3& _CamPos, f32 _FovYRadians, f32 _Aspect,
                                     const Vec3& _DirToSun, f32 _NearZ) {
        FrameSlot = _FrameSlot;
        (void)_CamPos; // o centro das cascatas vem do sub-frustum via InvView (reservado p/ modo "centered")
        CPUConstants.Params  = { static_cast<f32>(kNumCascades), DepthBias,
                                 1.0f / static_cast<f32>(kResolution), _Enabled ? 1.0f : 0.0f };
        CPUConstants.Params2 = { NormalOffsetTexels, PcfRadiusTexels, BlendBand,
                                 DebugCascades ? 1.0f : 0.0f };

        if (_Enabled) {
            // Splits exponenciais (Unreal ComputeAccumulatedScale): >1 => cascatas menores perto.
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

            const f32 tanV = std::tan(_FovYRadians * 0.5f);
            const f32 tanH = tanV * _Aspect;
            const Mat44 InvView = _View.Inverse();

            auto TransformPoint = [](const Mat44& M, const Vec3& p) -> Vec3 {
                return Vec3{
                    p.X*M.M[0][0] + p.Y*M.M[1][0] + p.Z*M.M[2][0] + M.M[3][0],
                    p.X*M.M[0][1] + p.Y*M.M[1][1] + p.Z*M.M[2][1] + M.M[3][1],
                    p.X*M.M[0][2] + p.Y*M.M[1][2] + p.Z*M.M[2][2] + M.M[3][2] };
            };

            // Base de luz: forward = direção de viagem da luz = -DirToSun.
            const Vec3 fwd = Vec3{ -_DirToSun.X, -_DirToSun.Y, -_DirToSun.Z }
                                 .NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
            const Vec3 up0   = (std::fabs(fwd.Y) > 0.99f) ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                          : Vec3{ 0.0f, 1.0f, 0.0f };
            const Vec3 right = up0.Cross(fwd).Normalized();
            const Vec3 up    = fwd.Cross(right);

            // NDC[-1,1] -> UV[0,1] com flip de Y (z inalterado), convenção vetor-linha.
            Mat44 BiasUV = Mat44::Identity();
            BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
            BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;

            f32* sf = &CPUConstants.CascadeTexelWorld.X; // texel-em-mundo por cascata (normal-offset)
            for (int c = 0; c < numC; ++c) {
                const f32 dn = Splits[c], df = Splits[c + 1];
                const f32 FarX = tanH*df, FarY = tanV*df, NearX = tanH*dn, NearY = tanV*dn;
                const f32 diagFar2 = FarX*FarX + FarY*FarY, diagNear2 = NearX*NearX + NearY*NearY;
                const f32 len = df - dn;
                f32 offset = (diagNear2 - diagFar2) / (2.0f * len) + len * 0.5f;
                f32 czv = df - offset;
                if (czv < dn) czv = dn; if (czv > df) czv = df;

                // Raio = maior distância de um canto do sub-frustum ao centro (0,0,czv).
                f32 r2 = 0.0f;
                const f32 cz[2] = { dn, df }, ex[2] = { NearX, FarX }, ey[2] = { NearY, FarY };
                for (int pl = 0; pl < 2; ++pl)
                    for (int sx = -1; sx <= 1; sx += 2)
                        for (int sy = -1; sy <= 1; sy += 2) {
                            const f32 x = sx*ex[pl], y = sy*ey[pl], z = cz[pl] - czv;
                            const f32 d = x*x + y*y + z*z;
                            if (d > r2) r2 = d;
                        }
                f32 radius = std::ceil(std::sqrt(r2));
                if (radius < 1.0f) radius = 1.0f;

                // Centro em mundo + texel-snap em light-space (anti-shimmer).
                const Vec3 centerWorld = TransformPoint(InvView, Vec3{ 0.0f, 0.0f, czv });
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
                CascadeViewProj[c]            = LightViewProj; // p/ culling de casters
                sf[c] = texel; // tamanho de 1 texel em mundo (normal-offset no shader)

                // CB por-cascata do depth pass (clip ortho cru, sem BiasUV).
                ShadowCascadeConstants Cascade{ LightViewProj };
                std::memcpy(MappedCascade +
                                (static_cast<size_t>(FrameSlot) * kNumCascades + c) *
                                    sizeof(ShadowCascadeConstants),
                            &Cascade, sizeof(ShadowCascadeConstants));
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
                                      const FShadowDrawItem* _Items, size_t _Count) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        _CommandList->SetGraphicsRootSignature(RootSig.Get());

        D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kResolution),
                           static_cast<FLOAT>(kResolution), 0.0f, 1.0f };
        D3D12_RECT     SC{ 0, 0, static_cast<LONG>(kResolution), static_cast<LONG>(kResolution) };
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &SC);

        for (u32 c = 0; c < kNumCascades; ++c) {
            auto DSV = DSVHeap.CpuHandle(c);
            _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
            _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            _CommandList->SetGraphicsRootConstantBufferView(0, CascadeCBAddr(c));

            // Culling de casters contra o frustum DESTA cascata (planos das colunas da
            // LightViewProj; convenção vetor-linha, D3D z[0,1]). Independe da câmera ->
            // objetos atrás/ao lado da câmera ainda projetam sombra na área visível.
            const Mat44& VP = CascadeViewProj[c];
            const Vec4 c0{ VP.M[0][0], VP.M[1][0], VP.M[2][0], VP.M[3][0] };
            const Vec4 c1{ VP.M[0][1], VP.M[1][1], VP.M[2][1], VP.M[3][1] };
            const Vec4 c2{ VP.M[0][2], VP.M[1][2], VP.M[2][2], VP.M[3][2] };
            const Vec4 c3{ VP.M[0][3], VP.M[1][3], VP.M[2][3], VP.M[3][3] };
            const Vec4 Planes[6] = {
                { c3.X+c0.X, c3.Y+c0.Y, c3.Z+c0.Z, c3.W+c0.W }, // left
                { c3.X-c0.X, c3.Y-c0.Y, c3.Z-c0.Z, c3.W-c0.W }, // right
                { c3.X+c1.X, c3.Y+c1.Y, c3.Z+c1.Z, c3.W+c1.W }, // bottom
                { c3.X-c1.X, c3.Y-c1.Y, c3.Z-c1.Z, c3.W-c1.W }, // top
                { c2.X, c2.Y, c2.Z, c2.W },                     // near (z>=0)
                { c3.X-c2.X, c3.Y-c2.Y, c3.Z-c2.Z, c3.W-c2.W }, // far
            };
            auto Outside = [&](const Vec3& Mn, const Vec3& Mx) -> bool {
                for (int i = 0; i < 6; ++i) {
                    const Vec4& p = Planes[i];
                    const f32 px = (p.X >= 0.0f) ? Mx.X : Mn.X;
                    const f32 py = (p.Y >= 0.0f) ? Mx.Y : Mn.Y;
                    const f32 pz = (p.Z >= 0.0f) ? Mx.Z : Mn.Z;
                    if (p.X*px + p.Y*py + p.Z*pz + p.W < 0.0f) return true;
                }
                return false;
            };

            ID3D12PipelineState* Cur = nullptr;
            for (size_t k = 0; k < _Count; ++k) {
                const FShadowDrawItem& It = _Items[k];
                if (!It.Mesh) continue;
                if (Outside(It.AABBMin, It.AABBMax)) continue;
                const bool Masked = It.Mat && It.Mat->TwoSided;
                ID3D12PipelineState* Want = Masked ? MaskedPSO.Get() : OpaquePSO.Get();
                if (Want != Cur) { _CommandList->SetPipelineState(Want); Cur = Want; }
                _CommandList->SetGraphicsRootConstantBufferView(3, It.ObjectCB); // b2 (Model)
                if (Masked) It.Mat->Bind(_CommandList, _SRVHeap);                // b1 + tabela t0-t7
                It.Mesh->Draw(_CommandList);
            }
        }

        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void FSunShadows::EnsureReadable(ID3D12GraphicsCommandList* _CommandList) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
