#include "Smile/Graphics/RainWetness.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cmath>
#include <cstring>

namespace Smile {

    void FRainWetness::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                  u32 _Width, u32 _Height) {
        if (IsInitialized()) return;
        BuildRootSignature(_Device);
        BuildPSO(_Device);
        BuildCurtainPSO(_Device);
        BuildParticlesPSO(_Device);
        CreateConstantBuffer(_Device);
        CreateScratch(_Device, _SRVHeap, _Width, _Height);
        BuildOcclusion(_Device, _SRVHeap);
        LogInfo("Chuva deferred (wetness + occlusion map + cortina + particulas) inicializada");
    }

    void FRainWetness::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                              u32 _Width, u32 _Height) {
        if (!IsInitialized()) return;
        CreateScratch(_Device, _SRVHeap, _Width, _Height);
    }

    void FRainWetness::Recreate(ID3D12Device* _Device) {
        if (!IsInitialized()) return;
        PSO.Reset();
        CurtainPSO.Reset();
        ParticlesPSO.Reset();
        SplashPSO.Reset();
        BuildPSO(_Device);
        BuildCurtainPSO(_Device);
        BuildParticlesPSO(_Device);
    }

    void FRainWetness::BuildRootSignature(ID3D12Device* _Device) {
        // [0] CBV b0 | [1] tabela [t0,t1] = scratch A/B (contiguos) | [2] tabela t2 = depth
        // | [3] tabela t3 = occlusion map (F2)
        D3D12_DESCRIPTOR_RANGE ScratchRange{};
        ScratchRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ScratchRange.NumDescriptors                    = 2;
        ScratchRange.BaseShaderRegister                = 0;
        ScratchRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE DepthRange = ScratchRange;
        DepthRange.NumDescriptors     = 1;
        DepthRange.BaseShaderRegister = 2;

        D3D12_DESCRIPTOR_RANGE OccRange = DepthRange;
        OccRange.BaseShaderRegister = 3;

        // Visibilidade ALL em CBV/depth/occ: o VS das particulas (F5) le o CB, projeta com a
        // RainViewProj e amostra o occlusion map como heightmap de colisao. Scratch segue
        // PIXEL (so o PS da wetness le).
        D3D12_ROOT_PARAMETER RootParams[4]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &ScratchRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &DepthRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &OccRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters = _countof(RootParams);
        Desc.pParameters   = RootParams;
        Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("RainWetness root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FRainWetness::BuildPSO(ID3D12Device* _Device) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso"); // fullscreen tri com uv
        auto PS = LoadShaderBytecode("RainWetness.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        // Sem blend: o PS reescreve A/B por inteiro; pixel seco/ceu da discard e preserva
        // o conteudo original do geometry pass.
        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 2;
        PSODesc.RTVFormats[0]         = FGBuffer::kFormatA;
        PSODesc.RTVFormats[1]         = FGBuffer::kFormatB;
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FRainWetness::BuildCurtainPSO(ID3D12Device* _Device) {
        // F3: fullscreen sobre o HDR ja iluminado/fogado, blend PREMULTIPLICADO (o PS devolve
        // cor ja escalada pelo alpha) — streak soma luz e vela levemente o fundo.
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("RainCurtain.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        auto& RT0                 = Blend.RenderTarget[0];
        RT0.BlendEnable           = TRUE;
        RT0.SrcBlend              = D3D12_BLEND_ONE;
        RT0.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        RT0.BlendOp               = D3D12_BLEND_OP_ADD;
        RT0.SrcBlendAlpha         = D3D12_BLEND_ONE;
        RT0.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        RT0.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        RT0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE; // soft-depth manual no PS (depth vem como SRV)
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR scene color
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&CurtainPSO)));
    }

    void FRainWetness::BuildParticlesPSO(ID3D12Device* _Device) {
        // F5: quads instanciados 100% procedurais (SV_VertexID/SV_InstanceID, sem VB) com o
        // mesmo blend premultiplicado da cortina; soft-depth manual no PS (depth como SRV).
        // Dois PSOs no mesmo molde: gota (F5a) e splash de impacto (F5b).
        auto VS = LoadShaderBytecode("RainParticles.vs_6_0.cso");
        auto PS = LoadShaderBytecode("RainParticles.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        auto& RT0                 = Blend.RenderTarget[0];
        RT0.BlendEnable           = TRUE;
        RT0.SrcBlend              = D3D12_BLEND_ONE;
        RT0.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        RT0.BlendOp               = D3D12_BLEND_OP_ADD;
        RT0.SrcBlendAlpha         = D3D12_BLEND_ONE;
        RT0.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        RT0.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        RT0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR scene color
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&ParticlesPSO)));

        auto SplashVS = LoadShaderBytecode("RainSplash.vs_6_0.cso");
        auto SplashPS = LoadShaderBytecode("RainSplash.ps_6_0.cso");
        PSODesc.VS = { SplashVS.data(), SplashVS.size() };
        PSODesc.PS = { SplashPS.data(), SplashPS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&SplashPSO)));
    }

    void FRainWetness::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                sizeof(RainWetnessConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);
        RainWetnessConstants Zero{};
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(RainWetnessConstants),
                        &Zero, sizeof(RainWetnessConstants));
    }

    void FRainWetness::CreateScratch(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        if (_Width == ScratchW && _Height == ScratchH && ScratchA) return;
        ScratchW = _Width;
        ScratchH = _Height;

        if (ScratchSRVBase == 0xFFFFFFFFu)
            ScratchSRVBase = _SRVHeap.Allocate(2); // [A,B] contiguos = uma tabela

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto Make = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& _Out, DXGI_FORMAT _Fmt, u32 _Slot) {
            _Out.Reset();

            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = _Width;
            Desc.Height           = _Height;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = _Fmt;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            // Ciclo de estados deterministico por Execute: COPY_DEST -> PSR -> COPY_DEST.
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&_Out)));

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = _Fmt;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Texture2D.MipLevels     = 1;
            _SRVHeap.CreateSRV(_Device, _Out.Get(), SRVDesc, _Slot);
        };

        Make(ScratchA, FGBuffer::kFormatA, ScratchSRVBase);
        Make(ScratchB, FGBuffer::kFormatB, ScratchSRVBase + 1);
    }

    void FRainWetness::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProjFull,
                                      const Mat44& _ViewProjFull, const Vec3& _CameraWorldPos,
                                      f32 _TimeSec, const FWeather& _Weather,
                                      const Vec3& _KeyDir, const Vec3& _KeyColorTimesInt,
                                      const Vec3& _SkyAmbient) {
        FrameSlot = _FrameSlot;
        if (!MappedBase) return;

        RainWetnessConstants c{};
        c.InvViewProj    = _InvViewProjFull;
        c.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, _TimeSec };
        // F4: a superficie usa o molhado ACUMULADO (Wetness, com inercia de secagem);
        // o instantaneo vai em RainParams1.z p/ aneis/splashes (param de "chovendo agora").
        c.RainParams0    = { _Weather.Wetness, _Weather.PuddleAmount,
                             _Weather.RippleStrength, _Weather.WetDarkening };
        // F2: matriz do ULTIMO render do mapa (cacheado); bias/banda em unidades de depth
        // do ortho (range vertical kOccUp+kOccDown). Sem mapa ainda = desligado (tudo molha).
        const f32 OccRange = kOccUp + kOccDown;

        const f32 Scale  = _Weather.PuddleScale > 1e-3f ? _Weather.PuddleScale : 1.0f;
        c.RainParams1    = { 1.0f / Scale, 1.0f / OccRange, _Weather.RainAmount, 0.0f };
        c.RainOccMatrix = OccWorldToUVZ;
        c.RainOccParams = { (_Weather.RainOcclusion && OccEverRendered) ? 1.0f : 0.0f,
                            0.75f / OccRange,            // bias: ~0.75 m acima ja e "coberto"
                            OccRange / 1.5f,             // banda suave de 1.5 m na transicao
                            static_cast<f32>(kOccSize) };

        // F3: cortina — queda ~12 m/s (chuva real ~9, um pouco mais rapido le melhor em tela).
        // F4: pre-multiplicada pelo RainAmount INSTANTANEO (parou de chover = gota some na
        // hora, mesmo com o chao ainda molhado pela inercia do Wetness).
        // F5: com particulas ON a cortina poupa os 2 cilindros proximos (CurtainParams.z);
        // as particulas so existem com o occlusion map valido (a colisao vem dele).
        const bool ParticlesOn = _Weather.RainParticles && OccEverRendered &&
                                 _Weather.RainAmount > 0.001f;
        ParticleCount = ParticlesOn
            ? static_cast<u32>(static_cast<f32>(kMaxRainParticles) * _Weather.RainAmount)
            : 0u;
        c.CurtainParams  = { _Weather.CurtainAmount * _Weather.RainAmount, 12.0f,
                             ParticlesOn ? 1.0f : 0.0f, 0.0f };
        c.KeyLightDir    = { _KeyDir.X, _KeyDir.Y, _KeyDir.Z, 0.0f };
        c.KeyLightColor  = { _KeyColorTimesInt.X, _KeyColorTimesInt.Y, _KeyColorTimesInt.Z, 0.0f };
        c.SkyAmbientRain = { _SkyAmbient.X, _SkyAmbient.Y, _SkyAmbient.Z, 0.0f };
        c.RainViewProj   = _ViewProjFull;
        c.RainParticleParams = { 12.0f, 24.0f, 0.0f, static_cast<f32>(ParticleCount) };

        std::memcpy(MappedBase + static_cast<size_t>(FrameSlot) * sizeof(RainWetnessConstants),
                    &c, sizeof(RainWetnessConstants));
    }

    void FRainWetness::Execute(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                               FGBuffer& _GBuffer, ID3D12Resource* _DepthBuffer, u32 _DepthSRVSlot,
                               u32 _Width, u32 _Height) {
        if (!IsInitialized() || !ScratchA) return;

        auto Barrier = [&](ID3D12Resource* _R, D3D12_RESOURCE_STATES _Before,
                           D3D12_RESOURCE_STATES _After) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _R;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = _Before;
            B.Transition.StateAfter  = _After;
            _Cmd->ResourceBarrier(1, &B);
        };

        // 1) Copia A/B pro scratch (Cry copia os RTs pelo mesmo motivo: nao da pra ler e
        //    escrever o mesmo RT no fullscreen pass).
        _GBuffer.TransitionToRead(_Cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        _Cmd->CopyResource(ScratchA.Get(), _GBuffer.Resource(0));
        _Cmd->CopyResource(ScratchB.Get(), _GBuffer.Resource(1));
        _GBuffer.TransitionToWrite(_Cmd);

        Barrier(ScratchA.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(ScratchB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(_DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 2) Fullscreen: reescreve GBufferA/B com a versao molhada.
        D3D12_CPU_DESCRIPTOR_HANDLE RTVs[2] = { _GBuffer.RTVHandle(0), _GBuffer.RTVHandle(1) };
        _Cmd->OMSetRenderTargets(2, RTVs, FALSE, nullptr);

        D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
                                 static_cast<f32>(_Width), static_cast<f32>(_Height),
                                 0.0f, 1.0f };
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(_Width), static_cast<LONG>(_Height) };
        _Cmd->RSSetViewports(1, &Viewport);
        _Cmd->RSSetScissorRects(1, &Scissor);

        _Cmd->SetGraphicsRootSignature(RootSig.Get());
        _Cmd->SetPipelineState(PSO.Get());
        _Cmd->SetGraphicsRootConstantBufferView(0, CBAddr());
        _Cmd->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(ScratchSRVBase));
        _Cmd->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _Cmd->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(OccSRVSlot));
        _Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _Cmd->IASetVertexBuffers(0, 0, nullptr);
        _Cmd->IASetIndexBuffer(nullptr);
        _Cmd->DrawInstanced(3, 1, 0, 0);

        // 3) Restaura as pre-condicoes (depth de volta a DEPTH_WRITE, scratch pro proximo frame).
        Barrier(_DepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
        Barrier(ScratchA.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(ScratchB.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
    }

    void FRainWetness::ExecuteCurtain(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                      u32 _DepthSRVSlot, u32 _Width, u32 _Height) {
        if (!CurtainPSO) return;

        // Pre-condicoes do caller (padrao do Fog.Execute): RTV do HDR ja setado sem DSV,
        // depth transicionado pra PIXEL_SHADER_RESOURCE. O scratch nao e lido pelo shader
        // da cortina, mas a tabela [1] e setada mesmo assim (root sig compartilhado).
        D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
                                 static_cast<f32>(_Width), static_cast<f32>(_Height),
                                 0.0f, 1.0f };
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(_Width), static_cast<LONG>(_Height) };
        _Cmd->RSSetViewports(1, &Viewport);
        _Cmd->RSSetScissorRects(1, &Scissor);

        _Cmd->SetGraphicsRootSignature(RootSig.Get());
        _Cmd->SetPipelineState(CurtainPSO.Get());
        _Cmd->SetGraphicsRootConstantBufferView(0, CBAddr());
        _Cmd->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(ScratchSRVBase));
        _Cmd->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _Cmd->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(OccSRVSlot));
        _Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _Cmd->IASetVertexBuffers(0, 0, nullptr);
        _Cmd->IASetIndexBuffer(nullptr);
        _Cmd->DrawInstanced(3, 1, 0, 0);
    }

    // ---- F2: mapa de oclusao top-down ----

    void FRainWetness::BuildOcclusion(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        // Root sig CLONE do CSM: [0] CBV b0 VS (view-proj do ortho) | [1] CBV b1 PS (material)
        // | [2] tabela t0-t7 (texturas do material) | [3] CBV b2 VS (ObjectCB) + sampler s0.
        // Material::Bind assume os indices 1/2 — por isso o layout precisa bater.
        {
            D3D12_DESCRIPTOR_RANGE MatRange{};
            MatRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            MatRange.NumDescriptors                    = 8;
            MatRange.BaseShaderRegister                = 0;
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
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                     &Blob, &ErrorBlob);
            if (FAILED(Hr)) {
                if (ErrorBlob)
                    LogError(std::string("RainOcc root sig error: ") +
                             static_cast<const char*>(ErrorBlob->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(),
                                                  Blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&OccRootSig)));
        }

        // PSOs: reusa os shaders do CSM (ShadowDepth.vs/ps). Depth clip OFF = pancaking:
        // telhado acima do teto do volume achata no near plane e segue ocluindo.
        {
            auto VS = LoadShaderBytecode("ShadowDepth.vs_6_0.cso");
            auto PS = LoadShaderBytecode("ShadowDepth.ps_6_0.cso");

            D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };

            D3D12_RASTERIZER_DESC Raster{};
            Raster.FillMode             = D3D12_FILL_MODE_SOLID;
            Raster.CullMode             = D3D12_CULL_MODE_NONE;
            Raster.DepthClipEnable      = FALSE;
            Raster.SlopeScaledDepthBias = 1.0f;

            D3D12_BLEND_DESC Blend{};
            Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_DEPTH_STENCIL_DESC Depth{};
            Depth.DepthEnable    = TRUE;
            Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature        = OccRootSig.Get();
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
            SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OccOpaquePSO)));

            PSODesc.PS = { PS.data(), PS.size() }; // alpha-test: copa de arvore filtra chuva
            SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&OccMaskedPSO)));
        }

        // Depth 512^2 D32 + SRV R32. Criado em PSR: so vira DEPTH_WRITE dentro do Record;
        // ate o 1o render o shader nem amostra (RainOccParams.x = 0 sem OccEverRendered).
        {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = kOccSize;
            Desc.Height           = kOccSize;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_R32_TYPELESS;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE Clear{};
            Clear.Format             = DXGI_FORMAT_D32_FLOAT;
            Clear.DepthStencil.Depth = 1.0f;

            // PSR|NPSR: o PS da wetness/cortina e o VS das particulas (F5) leem o mapa
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &Clear, IID_PPV_ARGS(&OccDepth)));
            OccState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            OccDSVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
            D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
            DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
            DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            _Device->CreateDepthStencilView(OccDepth.Get(), &DSVDesc, OccDSVHeap.CpuHandle(0));

            OccSRVSlot = _SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Texture2D.MipLevels     = 1;
            _SRVHeap.CreateSRV(_Device, OccDepth.Get(), SRVDesc, OccSRVSlot);
        }

        // CB do ortho (uma matriz por frame em voo; so escrito no frame que re-renderiza).
        {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * 256;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&OccCB)));
            D3D12_RANGE NoRead{ 0, 0 };
            void* Ptr = nullptr;
            SMILE_HR(OccCB->Map(0, &NoRead, &Ptr));
            MappedOccCB = reinterpret_cast<u8*>(Ptr);
        }
    }

    bool FRainWetness::RecordOcclusionMap(ID3D12GraphicsCommandList* _Cmd,
                                          FTextureSRVHeap& _SRVHeap, u32 _FrameSlot,
                                          const Vec3& _CamPos,
                                          const FOccluderItem* _Items, size_t _Count) {
        if (!OccOpaquePSO) return false;

        // Centro snapado num quantum grosso (16 m): andar so re-renderiza ao cruzar a
        // fronteira — estilo do bProcessed da Cry, mas automatico. Snap tambem em Y (a
        // codificacao de depth referencia o teto do volume).
        const Vec3 Snapped{ std::floor(_CamPos.X / kOccSnap + 0.5f) * kOccSnap,
                            std::floor(_CamPos.Y / kOccSnap + 0.5f) * kOccSnap,
                            std::floor(_CamPos.Z / kOccSnap + 0.5f) * kOccSnap };
        if (OccValid &&
            Snapped.X == OccSnapped.X && Snapped.Y == OccSnapped.Y && Snapped.Z == OccSnapped.Z)
            return false;

        const f32  Top    = Snapped.Y + kOccUp;
        const f32  Range  = kOccUp + kOccDown;
        const Vec3 Eye{ Snapped.X, Top, Snapped.Z };
        const Mat44 View = Mat44::LookAtLH(Eye, Vec3{ Eye.X, Eye.Y - 1.0f, Eye.Z },
                                           Vec3{ 0.0f, 0.0f, 1.0f });
        const Mat44 Proj = Mat44::OrthographicLH(2.0f * kOccHalfExtent, 2.0f * kOccHalfExtent,
                                                 0.0f, Range);
        const Mat44 ViewProj = View * Proj;

        Mat44 BiasUV = Mat44::Identity();
        BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
        BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;
        OccWorldToUVZ   = ViewProj * BiasUV;
        OccSnapped      = Snapped;
        OccValid        = true;
        OccEverRendered = true;

        std::memcpy(MappedOccCB + static_cast<size_t>(_FrameSlot) * 256, &ViewProj, sizeof(Mat44));

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = OccDepth.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = OccState;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        _Cmd->ResourceBarrier(1, &B);
        OccState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        auto DSV = OccDSVHeap.CpuHandle(0);
        _Cmd->OMSetRenderTargets(0, nullptr, FALSE, &DSV);
        _Cmd->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT VP{ 0.0f, 0.0f, static_cast<FLOAT>(kOccSize),
                           static_cast<FLOAT>(kOccSize), 0.0f, 1.0f };
        D3D12_RECT SC{ 0, 0, static_cast<LONG>(kOccSize), static_cast<LONG>(kOccSize) };
        _Cmd->RSSetViewports(1, &VP);
        _Cmd->RSSetScissorRects(1, &SC);
        _Cmd->SetGraphicsRootSignature(OccRootSig.Get());
        _Cmd->SetGraphicsRootConstantBufferView(
            0, OccCB->GetGPUVirtualAddress() + static_cast<u64>(_FrameSlot) * 256);

        // Cull XZ trivial: o volume e axis-aligned (olhando reto pra baixo).
        const f32 MinX = Snapped.X - kOccHalfExtent, MaxX = Snapped.X + kOccHalfExtent;
        const f32 MinZ = Snapped.Z - kOccHalfExtent, MaxZ = Snapped.Z + kOccHalfExtent;

        ID3D12PipelineState* Cur = nullptr;
        for (size_t k = 0; k < _Count; ++k) {
            const FOccluderItem& It = _Items[k];
            if (!It.Mesh) continue;
            if (It.AABBMax.X < MinX || It.AABBMin.X > MaxX ||
                It.AABBMax.Z < MinZ || It.AABBMin.Z > MaxZ) continue;
            const bool AlphaTested = It.Mat && (It.Mat->Constants.AlphaTest != 0);
            ID3D12PipelineState* Want = AlphaTested ? OccMaskedPSO.Get() : OccOpaquePSO.Get();
            if (Want != Cur) { _Cmd->SetPipelineState(Want); Cur = Want; }
            _Cmd->SetGraphicsRootConstantBufferView(3, It.ObjectCB);
            if (AlphaTested) It.Mat->Bind(_Cmd, _SRVHeap);
            It.Mesh->Draw(_Cmd);
        }

        B.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        _Cmd->ResourceBarrier(1, &B);
        OccState = B.Transition.StateAfter;
        return true;
    }

    // F5a: gotas por particula. Mesmo estado de pipeline da cortina (RTV do HDR ja setado,
    // depth em PSR); um DrawInstanced de quads que o VS posiciona por hash+tempo e mata
    // pelo heightmap de oclusao — sem buffer de particulas, sem compute.
    void FRainWetness::ExecuteParticles(ID3D12GraphicsCommandList* _Cmd,
                                        FTextureSRVHeap& _SRVHeap, u32 _DepthSRVSlot,
                                        u32 _Width, u32 _Height) {
        if (!ParticlesPSO || ParticleCount == 0) return;

        D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
                                 static_cast<f32>(_Width), static_cast<f32>(_Height),
                                 0.0f, 1.0f };
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(_Width), static_cast<LONG>(_Height) };
        _Cmd->RSSetViewports(1, &Viewport);
        _Cmd->RSSetScissorRects(1, &Scissor);

        _Cmd->SetGraphicsRootSignature(RootSig.Get());
        _Cmd->SetPipelineState(ParticlesPSO.Get());
        _Cmd->SetGraphicsRootConstantBufferView(0, CBAddr());
        _Cmd->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(ScratchSRVBase));
        _Cmd->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _Cmd->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(OccSRVSlot));
        _Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _Cmd->IASetVertexBuffers(0, 0, nullptr);
        _Cmd->IASetIndexBuffer(nullptr);
        _Cmd->DrawInstanced(6, ParticleCount, 0, 0);

        // F5b: splashes de impacto — mesmas instancias (cada gota tem exatamente 1 splash
        // em voo: a fracao "subterranea" do ciclo da queda e a janela dele). Estado 100%
        // compartilhado; so troca o PSO.
        if (SplashPSO) {
            _Cmd->SetPipelineState(SplashPSO.Get());
            _Cmd->DrawInstanced(6, ParticleCount, 0, 0);
        }
    }
}
