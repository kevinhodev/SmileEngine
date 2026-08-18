#include "Smile/Graphics/Lighting/LocalShadows.h"
#include "Smile/Graphics/RHI/GpuResources.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/RHI/CommandQueue.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/RHI/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>

namespace Smile {
    void FLocalShadows::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;
        CreateResources(_Device, _SRVHeap);
        BuildRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffers(_Device);
        Initialized = true;
        LogDebug("LocalShadows inicializado: atlas " + std::to_string(kMaxShadows) + "x" +
                std::to_string(kResolution) + "^2 (spot)");
    }

    void FLocalShadows::CreateResources(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        D3D12_CLEAR_VALUE Clear{};
        Clear.Format             = DXGI_FORMAT_D32_FLOAT;
        Clear.DepthStencil.Depth = 1.0f;

        ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        DepthArray = GpuResources::CreateTex2D(
            _Device, kResolution, kResolution, DXGI_FORMAT_R32_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, ArrayState, EVramCategory::Shadows,
            &Clear, 1, kMaxShadows, "Atlas de spot");

        DSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kMaxShadows, false);
        for (u32 s = 0; s < kMaxShadows; ++s) {
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
            DSVDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            DSVDesc.Texture2DArray.MipSlice        = 0;
            DSVDesc.Texture2DArray.FirstArraySlice = s;
            DSVDesc.Texture2DArray.ArraySize       = 1;
            _Device->CreateDepthStencilView(DepthArray.Get(), &DSVDesc, DSVHeap.CpuHandle(s));
        }

        // Cube array dos points (F3b): mesma textura D32, 6 faces por luz, vista como
        // TextureCubeArray no SRV (cube e um conceito de view, o recurso e um array 2D).
        CubeState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        CubeArray = GpuResources::CreateTex2D(
            _Device, kCubeResolution, kCubeResolution, DXGI_FORMAT_R32_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, CubeState, EVramCategory::Shadows,
            &Clear, 1, kMaxCubeShadows * 6, "Cube array de point");

        CubeDSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kMaxCubeShadows * 6, false);
        for (u32 s = 0; s < kMaxCubeShadows * 6; ++s) {
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
            DSVDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            DSVDesc.Texture2DArray.MipSlice        = 0;
            DSVDesc.Texture2DArray.FirstArraySlice = s;
            DSVDesc.Texture2DArray.ArraySize       = 1;
            _Device->CreateDepthStencilView(CubeArray.Get(), &DSVDesc, CubeDSVHeap.CpuHandle(s));
        }

        // SRVs CONTIGUOS: spot em Slot, cube em Slot+1 — a tabela t18..t19 do root param
        // do deferred lighting aponta pro primeiro e cobre os dois.
        ShadowSRVSlot_ = _SRVHeap.Allocate(2);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                         = DXGI_FORMAT_R32_FLOAT;
        SRVDesc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        SRVDesc.Texture2DArray.MostDetailedMip = 0;
        SRVDesc.Texture2DArray.MipLevels       = 1;
        SRVDesc.Texture2DArray.FirstArraySlice = 0;
        SRVDesc.Texture2DArray.ArraySize       = kMaxShadows;
        _SRVHeap.CreateSRV(_Device, DepthArray.Get(), SRVDesc, ShadowSRVSlot_);

        D3D12_SHADER_RESOURCE_VIEW_DESC CubeSRV{};
        CubeSRV.Format                          = DXGI_FORMAT_R32_FLOAT;
        CubeSRV.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        CubeSRV.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        CubeSRV.TextureCubeArray.MostDetailedMip = 0;
        CubeSRV.TextureCubeArray.MipLevels       = 1;
        CubeSRV.TextureCubeArray.First2DArrayFace = 0;
        CubeSRV.TextureCubeArray.NumCubes         = kMaxCubeShadows;
        _SRVHeap.CreateSRV(_Device, CubeArray.Get(), CubeSRV, ShadowSRVSlot_ + 1);
    }

    void FLocalShadows::BuildRootSignature(ID3D12Device* _Device) {
        // Identica a do FSunShadows (mesmos shaders ShadowDepth.vs/ps): b0 = matriz da luz,
        // b1 = material (alpha-test), b2 = objeto, tabela t0..t7 = texturas do material.
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
                LogError(std::string("LocalShadows root sig: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FLocalShadows::BuildPSOs(ID3D12Device* _Device) {
        auto VS = LoadShaderBytecode("ShadowDepth.vs_6_0.cso");
        auto PS = LoadShaderBytecode("ShadowDepth.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        // Oclusores two-sided pelo mesmo motivo do CSM (cascas single-sided do Bistro).
        Raster.CullMode              = D3D12_CULL_MODE_NONE;
        Raster.FrontCounterClockwise = FALSE;
        // Perspectiva (nao ortho): pancaking nao se aplica — caster atras do near da luz esta
        // ATRAS da propria luz e nao pode projetar sombra; depth clip normal.
        Raster.DepthClipEnable       = TRUE;
        Raster.DepthBias             = 0;
        Raster.SlopeScaledDepthBias  = 2.0f; // perspectiva concentra precisao no near: um pouco mais que o CSM
        // Teto do slope bias — 0 seria SEM CLAMP no D3D12 e o termo explodiria em poligono
        // rasante a luz (ver a nota longa no PSO do CSM, em SunShadows.cpp).
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
        PSODesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OpaquePSO)));

        PSODesc.PS = { PS.data(), PS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&MaskedPSO)));
    }

    void FLocalShadows::CreateConstantBuffers(ID3D12Device* _Device) {
        // 1 Mat44 por slice+face, num passo de 256 (alinhamento de CBV).
        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, 256,
            FCommandQueue::kFramesInFlight * (kMaxShadows + kMaxCubeShadows * 6));
        SliceCB     = Upload.Resource;
        MappedSlice = Upload.Mapped;
    }

    void FLocalShadows::TransitionArray(ID3D12GraphicsCommandList* _CommandList,
                                        D3D12_RESOURCE_STATES _After) {
        if (ArrayState == _After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = DepthArray.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = ArrayState;
        Barrier.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &Barrier);
        ArrayState = _After;
    }

    void FLocalShadows::TransitionCube(ID3D12GraphicsCommandList* _CommandList,
                                       D3D12_RESOURCE_STATES _After) {
        if (CubeState == _After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = CubeArray.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = CubeState;
        Barrier.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &Barrier);
        CubeState = _After;
    }

    // Planos do frustum (espaco de mundo) de um ViewProj row-major com x' = x * M. Ao
    // contrario do CSM, aqui INCLUI o near: a projecao e perspectiva com depth clip ligado,
    // entao caster antes do near da luz nao rasteriza de jeito nenhum. No CSM o plano e
    // omitido porque com pancaking (depth clip off) o caster ainda achata no near e projeta.
    static void ExtractFrustumPlanes(const Mat44& _M, Vec4 _Out[6]) {
        const Vec4 c0{ _M.M[0][0], _M.M[1][0], _M.M[2][0], _M.M[3][0] };
        const Vec4 c1{ _M.M[0][1], _M.M[1][1], _M.M[2][1], _M.M[3][1] };
        const Vec4 c2{ _M.M[0][2], _M.M[1][2], _M.M[2][2], _M.M[3][2] };
        const Vec4 c3{ _M.M[0][3], _M.M[1][3], _M.M[2][3], _M.M[3][3] };
        _Out[0] = { c3.X + c0.X, c3.Y + c0.Y, c3.Z + c0.Z, c3.W + c0.W }; // esquerda
        _Out[1] = { c3.X - c0.X, c3.Y - c0.Y, c3.Z - c0.Z, c3.W - c0.W }; // direita
        _Out[2] = { c3.X + c1.X, c3.Y + c1.Y, c3.Z + c1.Z, c3.W + c1.W }; // baixo
        _Out[3] = { c3.X - c1.X, c3.Y - c1.Y, c3.Z - c1.Z, c3.W - c1.W }; // cima
        _Out[4] = { c2.X,        c2.Y,        c2.Z,        c2.W        }; // near (z' >= 0)
        _Out[5] = { c3.X - c2.X, c3.Y - c2.Y, c3.Z - c2.Z, c3.W - c2.W }; // far
    }

    static bool AABBOutsidePlanes(const Vec4 _Planes[6], const Vec3& _Mn, const Vec3& _Mx) {
        for (int i = 0; i < 6; ++i) {
            const Vec4& p = _Planes[i];
            const f32 px = (p.X >= 0.0f) ? _Mx.X : _Mn.X;
            const f32 py = (p.Y >= 0.0f) ? _Mx.Y : _Mn.Y;
            const f32 pz = (p.Z >= 0.0f) ? _Mx.Z : _Mn.Z;
            if (p.X * px + p.Y * py + p.Z * pz + p.W < 0.0f) return true;
        }
        return false;
    }

    void FLocalShadows::RecordDepthPass(ID3D12GraphicsCommandList* _CommandList,
                                        FTextureSRVHeap& _SRVHeap, u32 _FrameSlot,
                                        const FShadowDrawItem* _Items, size_t _Count,
                                        const FShadowJob* _Jobs, u32 _JobCount,
                                        const FCubeShadowJob* _CubeJobs, u32 _CubeJobCount,
                                        const FExtraLocalDraw& _ExtraDraw) {
        if (!Initialized) return;
        if (_JobCount == 0 && _CubeJobCount == 0) { EnsureReadable(_CommandList); return; }

        // Broad phase por LUZ (uma varredura da cena, AABB vs esfera de influencia). As views
        // que vem depois — 1 slice do spot, 6 faces do point — refinam com o proprio frustum
        // SO em cima desta lista curta. Antes cada face repetia o teste de esfera na cena
        // inteira e mandava o resto pro clipper: ate 32*N testes de CPU por frame e draws que
        // so morriam na GPU. Duas fases e o que Cry e Flax fazem (broad phase por luz, depois
        // um contexto/frustum por face).
        auto BroadPhase = [&](const Vec3& _LightPos, f32 _Radius) {
            CullScratch.clear();
            const f32 R2 = _Radius * _Radius;
            const f32 P[3] = { _LightPos.X, _LightPos.Y, _LightPos.Z };
            for (u32 k = 0; k < static_cast<u32>(_Count); ++k) {
                const FShadowDrawItem& It = _Items[k];
                if (!It.Mesh) continue;
                const f32 Lo[3] = { It.AABBMin.X, It.AABBMin.Y, It.AABBMin.Z };
                const f32 Hi[3] = { It.AABBMax.X, It.AABBMax.Y, It.AABBMax.Z };
                f32 D2 = 0.0f;
                for (int a = 0; a < 3; ++a) {
                    if (P[a] < Lo[a])      { const f32 d = Lo[a] - P[a]; D2 += d * d; }
                    else if (P[a] > Hi[a]) { const f32 d = P[a] - Hi[a]; D2 += d * d; }
                }
                if (D2 <= R2) CullScratch.push_back(k);
            }
        };

        // Narrow phase + draws desta view (DSV/CB ja setados pelo chamador).
        auto DrawView = [&](const Mat44& _ViewProj) {
            Vec4 Planes[6];
            ExtractFrustumPlanes(_ViewProj, Planes);
            ID3D12PipelineState* Cur = nullptr; // local: o ExtraDraw da view anterior trocou o PSO
            FDrawSubmitCache Submit;
            _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (u32 k : CullScratch) {
                const FShadowDrawItem& It = _Items[k];
                if (AABBOutsidePlanes(Planes, It.AABBMin, It.AABBMax)) continue;
                const bool AlphaTested = It.Mat && (It.Mat->Constants.AlphaTest != 0);
                ID3D12PipelineState* Want = AlphaTested ? MaskedPSO.Get() : OpaquePSO.Get();
                if (Want != Cur) { _CommandList->SetPipelineState(Want); Cur = Want; }
                _CommandList->SetGraphicsRootConstantBufferView(3, It.ObjectCB);
                if (AlphaTested) Submit.BindMaterial(_CommandList, _SRVHeap, It.Mat);
                Submit.DrawMesh(_CommandList, It.Mesh);
            }
        };

        // Slots do SliceCB por frame: [0..kMaxShadows) = spots; depois 6 por cubo.
        const size_t FrameCBBase =
            static_cast<size_t>(_FrameSlot) * (kMaxShadows + kMaxCubeShadows * 6) * 256;

        // ---- Spots (F3a) ----
        if (_JobCount > 0) {
            TransitionArray(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

            D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kResolution),
                               static_cast<FLOAT>(kResolution), 0.0f, 1.0f };
            D3D12_RECT     SC{ 0, 0, static_cast<LONG>(kResolution), static_cast<LONG>(kResolution) };
            _CommandList->RSSetViewports(1, &VP);
            _CommandList->RSSetScissorRects(1, &SC);

            for (u32 j = 0; j < _JobCount && j < kMaxShadows; ++j) {
                const FShadowJob& Job = _Jobs[j];
                const u32 Slice = Job.Slice < kMaxShadows ? Job.Slice : 0;

                const size_t CBOffset = FrameCBBase + static_cast<size_t>(Slice) * 256;
                std::memcpy(MappedSlice + CBOffset, &Job.ViewProj, sizeof(Mat44));
                const D3D12_GPU_VIRTUAL_ADDRESS SliceCBAddr =
                    SliceCB->GetGPUVirtualAddress() + CBOffset;

                // Root sig por slice (nao uma vez fora do loop): o ExtraDraw do slice anterior
                // trocou root signature e PSO.
                _CommandList->SetGraphicsRootSignature(RootSig.Get());
                auto DSV = DSVHeap.CpuHandle(Slice);
                _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
                _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                _CommandList->SetGraphicsRootConstantBufferView(0, SliceCBAddr);

                BroadPhase(Job.LightPos, Job.Radius);
                DrawView(Job.ViewProj);

                if (_ExtraDraw)
                    _ExtraDraw(_CommandList, SliceCBAddr, Job.ViewProj, Job.LightPos, Job.Radius);
            }

            TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // ---- Points (F3b): 6 faces de 90 graus por luz, convencao de faces D3D ----
        if (_CubeJobCount > 0) {
            TransitionCube(_CommandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

            D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kCubeResolution),
                               static_cast<FLOAT>(kCubeResolution), 0.0f, 1.0f };
            D3D12_RECT     SC{ 0, 0, static_cast<LONG>(kCubeResolution),
                               static_cast<LONG>(kCubeResolution) };
            _CommandList->RSSetViewports(1, &VP);
            _CommandList->RSSetScissorRects(1, &SC);

            static const Vec3 kFaceFwd[6] = {
                {  1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                {  0.0f, 1.0f, 0.0f }, {  0.0f,-1.0f, 0.0f },
                {  0.0f, 0.0f, 1.0f }, {  0.0f, 0.0f,-1.0f },
            };
            static const Vec3 kFaceUp[6] = {
                { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f,-1.0f }, { 0.0f, 0.0f, 1.0f },
                { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
            };
            constexpr f32 kHalfPi = 1.57079632679f;

            for (u32 j = 0; j < _CubeJobCount && j < kMaxCubeShadows; ++j) {
                const FCubeShadowJob& Job = _CubeJobs[j];
                const u32 Cube = Job.CubeIndex < kMaxCubeShadows ? Job.CubeIndex : 0;
                const f32 FarP = std::max(Job.Radius, kPointNear * 2.0f);

                const Mat44 Proj = Mat44::PerspectiveFovLH(kHalfPi, 1.0f, kPointNear, FarP);

                // Uma broad phase para as SEIS faces: a esfera de influencia e a mesma.
                BroadPhase(Job.LightPos, FarP);

                for (u32 f = 0; f < 6; ++f) {
                    const Mat44 FaceVP =
                        Mat44::LookAtLH(Job.LightPos, Job.LightPos + kFaceFwd[f], kFaceUp[f]) * Proj;

                    const u32 CBSlot  = kMaxShadows + Cube * 6 + f;
                    const size_t CBOffset = FrameCBBase + static_cast<size_t>(CBSlot) * 256;
                    std::memcpy(MappedSlice + CBOffset, &FaceVP, sizeof(Mat44));
                    const D3D12_GPU_VIRTUAL_ADDRESS FaceCBAddr =
                        SliceCB->GetGPUVirtualAddress() + CBOffset;

                    _CommandList->SetGraphicsRootSignature(RootSig.Get());
                    auto DSV = CubeDSVHeap.CpuHandle(Cube * 6 + f);
                    _CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
                    _CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    _CommandList->SetGraphicsRootConstantBufferView(0, FaceCBAddr);

                    DrawView(FaceVP);

                    if (_ExtraDraw)
                        _ExtraDraw(_CommandList, FaceCBAddr, FaceVP, Job.LightPos, FarP);
                }
            }

            TransitionCube(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        EnsureReadable(_CommandList);
    }

    void FLocalShadows::EnsureReadable(ID3D12GraphicsCommandList* _CommandList) {
        TransitionArray(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionCube(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void FLocalShadows::EnsureReadableCompute(ID3D12GraphicsCommandList* _CommandList) {
        const D3D12_RESOURCE_STATES Both = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        TransitionArray(_CommandList, Both);
        TransitionCube(_CommandList, Both);
    }
}
